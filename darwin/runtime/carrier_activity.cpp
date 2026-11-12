// SPDX-License-Identifier: GPL-2.0-or-later
#include "carrier_activity.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>

namespace uml::runtime {
namespace {

constexpr std::uint64_t nanoseconds_per_millisecond = 1'000'000;
constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000;
constexpr std::uint64_t arm_ns = carrier_arm_ms * nanoseconds_per_millisecond;
/* No deadline. A wait of more than a day counts as one without a limit. */
constexpr std::uint64_t never = ~std::uint64_t{0};
constexpr std::uint64_t longest_wait_ns = 86'400 * nanoseconds_per_second;

/* The identities of the queue's events. The carriers' timer and the timer of
 * a host wake are separate, so that no carrier removes the second. */
constexpr std::uintptr_t timer_ident = 1;
constexpr std::uintptr_t host_wake_ident = 2;
constexpr std::uintptr_t poke_ident = 3;
constexpr std::uintptr_t stop_ident = 4;

/* The continuous clock, which counts the time the system sleeps, in
 * nanoseconds: the clock of the stall watch's times. */
std::uint64_t continuous_ns() noexcept
{
	return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
}

/* Per-process state, constant-initialized and never destroyed: carriers use it
 * until the process ends. */
struct alignas(64) Limit final {
	/* When the carrier's timed idle wait ends, on the continuous clock;
	 * `never` outside one or in one without a limit. */
	std::atomic<std::uint64_t> deadline_ns{never};
};

Limit limits[carrier_capacity];
/* Bit v: the carrier of vCPU v is outside an idle wait. */
std::atomic<std::uint32_t> outside_idle{0};
/* Bit v: a carrier of vCPU v reports its idle waits here. */
std::atomic<std::uint32_t> reporting{0};
/* The watch's kqueue, -1 before the first watch; never closed, because a
 * carrier may be arming it when a watch ends and the descriptor must not be
 * reused by then. */
std::atomic<int> queue{-1};
/* A watch is listening: the carriers arm the queue. */
std::atomic<bool> listening{false};

struct ThreadState final {
	std::int8_t vcpu;
	/* How many idle scopes of the thread are open. */
	std::uint8_t depth;
};

thread_local ThreadState this_thread{-1, 0};

[[nodiscard]] constexpr std::uint32_t bit_of(unsigned vcpu) noexcept
{
	return std::uint32_t{1} << vcpu;
}

/* Sets a timer of the watch's queue to go off in `delay_ns`; setting a timer
 * that is set moves it. */
void set_timer(std::uintptr_t ident, std::uint64_t delay_ns) noexcept
{
	const int fd = queue.load(std::memory_order_acquire);
	if (fd < 0 || !listening.load(std::memory_order_acquire))
		return;
	struct kevent change;
	EV_SET(&change, ident, EVFILT_TIMER,
		EV_ADD | EV_ENABLE | EV_ONESHOT,
		NOTE_NSECONDS | NOTE_MACH_CONTINUOUS_TIME,
		static_cast<std::intptr_t>(std::clamp<std::uint64_t>(delay_ns, 1,
			longest_wait_ns + arm_ns)),
		nullptr);
	(void)kevent(fd, &change, 1, nullptr, 0, nullptr);
}

void remove_timer(std::uintptr_t ident) noexcept
{
	const int fd = queue.load(std::memory_order_acquire);
	if (fd < 0 || !listening.load(std::memory_order_acquire))
		return;
	struct kevent change;
	EV_SET(&change, ident, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
	/* A timer that has gone off and was collected is gone already. */
	(void)kevent(fd, &change, 1, nullptr, 0, nullptr);
}

/* All the carriers are in an idle wait. The timer goes off 50 ms after the
 * earliest deadline of a timed wait, or is removed when none has one, so that
 * the watch wakes for a carrier that is due and does not run, and for nothing
 * else.
 *
 * The ordering argument. A carrier clears its bit in `outside_idle` before it
 * calls this (and the watch calls it only to sleep), and a carrier that
 * leaves its idle wait sets its bit before it arms the timer. The call above
 * can remove or move the timer after such a carrier has armed it, since the
 * two system calls are not ordered. The last step settles it: it reads the
 * mask once the timer has been changed, and a carrier outside its wait then
 * arms the timer again. Of the carrier's set and this read, one comes first
 * in the order of the atomics. If the set came first, the read sees the bit
 * and arms. If the read came first, the change of the timer came before it,
 * hence before the set and before the carrier's arm, which then stands. Either
 * way the timer is armed for a carrier outside its wait. A call that arms for
 * nothing costs the watch one wake. */
void settle_timer() noexcept
{
	if (!listening.load(std::memory_order_acquire))
		return;
	const std::uint32_t known = reporting.load(std::memory_order_acquire);
	std::uint64_t earliest = never;
	for (unsigned vcpu = 0; vcpu < carrier_capacity; ++vcpu)
		if ((known & bit_of(vcpu)) != 0)
			earliest = std::min(earliest,
				limits[vcpu].deadline_ns.load(std::memory_order_relaxed));
	if (earliest == never) {
		remove_timer(timer_ident);
	} else {
		const std::uint64_t now = continuous_ns();
		const std::uint64_t at = earliest > never - arm_ns ? never :
			earliest + arm_ns;
		set_timer(timer_ident, at > now ? at - now : 1);
	}
	if (outside_idle.load(std::memory_order_seq_cst) != 0)
		set_timer(timer_ident, arm_ns);
}

/* The carrier of `vcpu` leaves its idle wait, or starts. */
void leave_idle(unsigned vcpu) noexcept
{
	limits[vcpu].deadline_ns.store(never, std::memory_order_relaxed);
	if (outside_idle.fetch_or(bit_of(vcpu), std::memory_order_seq_cst) == 0)
		set_timer(timer_ident, arm_ns);
}

/* The carrier of `vcpu` enters an idle wait that ends by `deadline_ns`, or
 * ends for good. */
void enter_idle(unsigned vcpu, std::uint64_t deadline_ns) noexcept
{
	/* Before the bit clears: the carrier that enters last reads it. */
	limits[vcpu].deadline_ns.store(deadline_ns, std::memory_order_relaxed);
	const std::uint32_t bit = bit_of(vcpu);
	if (outside_idle.fetch_and(~bit, std::memory_order_seq_cst) == bit)
		settle_timer();
}

/* A carrier thread ends: it reports no more. */
struct EndOfCarrier final {
	int vcpu{-1};

	~EndOfCarrier()
	{
		if (vcpu < 0)
			return;
		const auto number = static_cast<unsigned>(vcpu);
		reporting.fetch_and(~bit_of(number), std::memory_order_seq_cst);
		enter_idle(number, never);
	}
};

thread_local EndOfCarrier end_of_carrier;

[[nodiscard]] std::uint64_t deadline_of(const timespec *limit,
	CarrierIdle::Limit clock) noexcept
{
	if (limit == nullptr)
		return never;
	constexpr auto per_second = static_cast<std::int64_t>(nanoseconds_per_second);
	timespec remaining = *limit;
	if (clock == CarrierIdle::Limit::realtime) {
		timespec now{};
		(void)clock_gettime(CLOCK_REALTIME, &now);
		remaining.tv_sec -= now.tv_sec;
		remaining.tv_nsec -= now.tv_nsec;
	}
	/* A wait of more than a day has no limit worth keeping. */
	if (remaining.tv_sec > static_cast<time_t>(longest_wait_ns / nanoseconds_per_second))
		return never;
	const std::int64_t remaining_ns =
		static_cast<std::int64_t>(remaining.tv_sec) * per_second + remaining.tv_nsec;
	if (remaining_ns <= 0)
		return continuous_ns();
	return continuous_ns() + static_cast<std::uint64_t>(remaining_ns);
}

} // namespace

void carrier_named(unsigned vcpu) noexcept
{
	if (vcpu >= carrier_capacity || this_thread.vcpu >= 0)
		return;
	this_thread.vcpu = static_cast<std::int8_t>(vcpu);
	this_thread.depth = 0;
	end_of_carrier.vcpu = static_cast<int>(vcpu);
	reporting.fetch_or(bit_of(vcpu), std::memory_order_seq_cst);
	leave_idle(vcpu);
}

CarrierIdle::CarrierIdle() noexcept
	: counted_{this_thread.vcpu >= 0}
{
	if (counted_ && this_thread.depth++ == 0)
		enter_idle(static_cast<unsigned>(this_thread.vcpu), never);
}

CarrierIdle::CarrierIdle(const timespec *limit, Limit clock) noexcept
	: counted_{this_thread.vcpu >= 0}
{
	if (counted_ && this_thread.depth++ == 0)
		enter_idle(static_cast<unsigned>(this_thread.vcpu),
			deadline_of(limit, clock));
}

CarrierIdle::~CarrierIdle()
{
	if (counted_ && --this_thread.depth == 0)
		leave_idle(static_cast<unsigned>(this_thread.vcpu));
}

void note_host_wake() noexcept
{
	set_timer(host_wake_ident, arm_ns);
}

bool watch_queue_open() noexcept
{
	int fd = queue.load(std::memory_order_acquire);
	if (fd < 0) {
		fd = kqueue();
		if (fd < 0)
			return false;
		(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
		struct kevent users[2];
		EV_SET(&users[0], poke_ident, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0,
			nullptr);
		EV_SET(&users[1], stop_ident, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0,
			nullptr);
		if (kevent(fd, users, 2, nullptr, 0, nullptr) < 0) {
			(void)close(fd);
			return false;
		}
		int expected = -1;
		if (!queue.compare_exchange_strong(expected, fd,
			    std::memory_order_acq_rel)) {
			(void)close(fd);
			fd = expected;
		}
	}
	/* What an earlier watch left: timers, and a poke or stop it did not
	 * collect. */
	struct kevent removed[2];
	EV_SET(&removed[0], timer_ident, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
	EV_SET(&removed[1], host_wake_ident, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
	for (struct kevent &one : removed)
		(void)kevent(fd, &one, 1, nullptr, 0, nullptr);
	for (;;) {
		struct kevent pending[4];
		const timespec now{0, 0};
		if (kevent(fd, nullptr, 0, pending, 4, &now) <= 0)
			break;
	}
	listening.store(true, std::memory_order_release);
	return true;
}

void watch_queue_close() noexcept
{
	listening.store(false, std::memory_order_release);
}

unsigned watch_queue_wait(const std::uint64_t *timeout_ns) noexcept
{
	const int fd = queue.load(std::memory_order_acquire);
	if (fd < 0)
		return 0;
	timespec limit{};
	if (timeout_ns != nullptr) {
		limit.tv_sec = static_cast<time_t>(*timeout_ns / nanoseconds_per_second);
		limit.tv_nsec = static_cast<long>(*timeout_ns % nanoseconds_per_second);
	}
	struct kevent events[4];
	const int count = kevent(fd, nullptr, 0, events, 4,
		timeout_ns != nullptr ? &limit : nullptr);
	unsigned woken = 0;
	for (int index = 0; index < count; ++index) {
		if (events[index].filter == EVFILT_TIMER)
			woken |= watch_woken_by_timer;
		else if (events[index].filter == EVFILT_USER)
			woken |= events[index].ident == stop_ident ?
				watch_woken_by_stop : watch_woken_by_poke;
	}
	return woken;
}

void watch_poke() noexcept
{
	const int fd = queue.load(std::memory_order_acquire);
	if (fd < 0)
		return;
	struct kevent trigger;
	EV_SET(&trigger, poke_ident, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
	(void)kevent(fd, &trigger, 1, nullptr, 0, nullptr);
}

void watch_stop() noexcept
{
	const int fd = queue.load(std::memory_order_acquire);
	if (fd < 0)
		return;
	struct kevent trigger;
	EV_SET(&trigger, stop_ident, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
	(void)kevent(fd, &trigger, 1, nullptr, 0, nullptr);
}

std::uint32_t carriers_outside_idle() noexcept
{
	return outside_idle.load(std::memory_order_seq_cst);
}

std::uint32_t carriers_reporting() noexcept
{
	return reporting.load(std::memory_order_acquire);
}

void watch_settle() noexcept
{
	settle_timer();
}

} // namespace uml::runtime
