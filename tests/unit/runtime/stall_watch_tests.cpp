// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The stall watch against real threads (runtime/stall_watch): carriers the
 * kernel thread registry lists, named as the kernel names them, spend their
 * time in marked waits; the watch writes what it sees into the stall slot of
 * a session record file. A stall here is a carrier that stays in one marked
 * wait, which needs no load on the host.
 */
#include "kernel_threads.hpp"
#include "kernel_waits.hpp"
#include "platform/host_thermal.hpp"
#include "platform/host_wait.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"
#include "session_record.hpp"
#include "stall_record.hpp"
#include "stall_watch.hpp"
#include "thread_fence.hpp"
#include "thread_nap.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dispatch/dispatch.h>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

/* The main queue's ping is a call of dispatch_async_f. This binary defines
 * the function, so the watch's call binds here and not to libdispatch: it
 * counts the callers by thread, answers the ping at once, and the test
 * requires that no thread of the process's own (uml-*), which the thread fence
 * can hold, ever sends one. */
namespace {
std::atomic<int> pings_from_held_threads{0};
std::atomic<int> pings_from_other_threads{0};
} // namespace

extern "C" void dispatch_async_f(dispatch_queue_t, void *context,
	dispatch_function_t work)
{
	char name[64]{};
	(void)pthread_getname_np(pthread_self(), name, sizeof(name));
	if (std::strncmp(name, "uml-", 4) == 0)
		++pings_from_held_threads;
	else
		++pings_from_other_threads;
	work(context);
}

namespace {

using uml::platform::HostThermalLevel;
using uml::platform::HostThermalState;
using uml::runtime::session_memory_slot_bytes;
using uml::runtime::session_stall_slot_bytes;
using uml::testing::nap;
using uml::testing::require;

/* A thermal source the test moves. It counts the readings made from a thread
 * of the process's own (uml-*), which the thread fence can hold: the watch
 * thread must never read it. */
struct Thermal final : uml::platform::HostThermalProvider {
	static inline std::atomic<int> level{0};
	static inline std::atomic<int> processors{6};
	static inline std::atomic<int> from_held_threads{0};
	static inline std::atomic<int> readings{0};

	HostThermalState state() noexcept override
	{
		char name[64]{};
		(void)pthread_getname_np(pthread_self(), name, sizeof(name));
		if (std::strncmp(name, "uml-", 4) == 0)
			++from_held_threads;
		++readings;
		HostThermalState result;
		result.level = static_cast<HostThermalLevel>(level.load());
		result.activeProcessors = static_cast<std::uint8_t>(processors.load());
		return result;
	}
};

/* A carrier as the kernel makes one: a registered thread that names itself. */
struct Carrier final {
	pthread_t thread{};
	unsigned cpu{};
	std::function<void()> body;
	std::atomic<bool> stop{false};
};

void *carrier_main(void *argument)
{
	auto *carrier = static_cast<Carrier *>(argument);
	char name[32];
	std::snprintf(name, sizeof(name), "uml-vcpu-%u-carrier", carrier->cpu);
	(void)pthread_setname_np(name);
	carrier->body();
	return nullptr;
}

void start(Carrier &carrier, unsigned cpu, std::function<void()> body)
{
	carrier.cpu = cpu;
	carrier.body = std::move(body);
	require(uml::runtime::kernel_pthread_create(&carrier.thread, nullptr,
		carrier_main, &carrier) == 0);
}

/* The idle wait of a vCPU, through the loader's wrapper: 20 ms at a time. */
void idle_until_stopped(const Carrier &carrier)
{
	using Pselect = int (*)(int, fd_set *, fd_set *, fd_set *, const timespec *,
		const sigset_t *);
	const auto wait = reinterpret_cast<Pselect>(
		uml::runtime::kernel_wait_import("_pselect$DARWIN_EXTSN"));
	require(wait != nullptr);
	while (!carrier.stop.load(std::memory_order_acquire)) {
		const timespec period{0, 20L * 1000L * 1000L};
		(void)wait(0, nullptr, nullptr, nullptr, &period, nullptr);
	}
}

/* One wait for a device service's answer, as long as the test says. */
void wait_for_a_device_service(int descriptor)
{
	using Poll = int (*)(pollfd *, nfds_t, int);
	const auto wait = reinterpret_cast<Poll>(
		uml::runtime::kernel_wait_import("poll"));
	require(wait != nullptr);
	pollfd entry{descriptor, POLLIN, 0};
	(void)wait(&entry, 1, 10'000);
}

std::string stall_slot(const std::string &path)
{
	std::ifstream file{path, std::ios::binary};
	const std::string content{std::istreambuf_iterator<char>{file},
		std::istreambuf_iterator<char>{}};
	require(content.size() >= session_memory_slot_bytes + session_stall_slot_bytes,
		"the record is shorter than its slots");
	return content.substr(session_memory_slot_bytes, session_stall_slot_bytes);
}

/* The events that name a vCPU. A host so loaded that the watch's own tick runs
 * 150 ms late records an event of the whole process as well, which names none
 * and says nothing of the carriers. */
std::size_t vcpu_events(const std::string &slot)
{
	std::size_t found = 0;
	std::size_t at = 0;
	while ((at = slot.find("\nstall ", at)) != std::string::npos) {
		++at;
		const std::size_t end = slot.find('\n', at);
		const std::string line = slot.substr(at, end - at);
		/* stall SEQ START DUR[+] CAUSES ... */
		std::size_t field = 0;
		std::size_t from = 0;
		for (; field < 4; ++field)
			from = line.find(' ', from) + 1;
		const std::string causes = line.substr(from, line.find(' ', from) - from);
		if (causes.find_first_not_of('.') != std::string::npos)
			++found;
	}
	return found;
}

/* The events whose watch lateness, the eighth field, is at least `late_ms`. */
std::size_t late_events(const std::string &slot, unsigned late_ms)
{
	std::size_t found = 0;
	std::size_t at = 0;
	while ((at = slot.find("\nstall ", at)) != std::string::npos) {
		++at;
		const std::size_t end = slot.find('\n', at);
		const std::string line = slot.substr(at, end - at);
		std::size_t from = 0;
		for (unsigned field = 0; field < 7; ++field)
			from = line.find(' ', from) + 1;
		if (std::strtoul(line.c_str() + from, nullptr, 10) >= late_ms)
			++found;
	}
	return found;
}

/* The count after `word` in the slot's header. */
unsigned long header_count(const std::string &slot, const std::string &word)
{
	const std::size_t at = slot.find(" " + word + " ");
	return at == std::string::npos ? 0 :
		std::strtoul(slot.c_str() + at + word.size() + 2, nullptr, 10);
}

void sleep_ms(unsigned milliseconds)
{
	nap(milliseconds * 1000U);
}

void a_stalled_carrier_is_recorded_and_idle_ones_are_not()
{
	const uml::testing::ScratchSubdirectory directory{"uml-stall-watch.XXXXXX"};
	const std::string path = (directory.path() / "session").string();
	uml::runtime::reset_session_record();
	require(uml::runtime::begin_session_record(path, {}), "begin the record");
	require(uml::runtime::session_record_open());

	int pipe_fds[2];
	require(pipe(pipe_fds) == 0);
	Carrier idle0;
	Carrier idle1;
	Carrier stalled;
	start(idle0, 0, [&] { idle_until_stopped(idle0); });
	start(idle1, 1, [&] { idle_until_stopped(idle1); });
	Thermal::level = 0;
	auto watch = uml::runtime::start_stall_watch(std::make_unique<Thermal>());
	require(watch != nullptr, "a watch without a record");

	/* Only idle carriers: no event, and the watch has seen them in the idle
	 * wait the loader's wrapper marks, which no load on the host changes. */
	sleep_ms(400);
	require(vcpu_events(stall_slot(path)) == 0, "idle carriers stalled:\n" + stall_slot(path));
	require(header_count(stall_slot(path), "idle") >= 1,
		"no carrier was seen in a marked idle wait:\n" + stall_slot(path));

	/* A carrier starts a wait that lasts, and the thermal state and the core
	 * count move. */
	start(stalled, 2, [&] {
		wait_for_a_device_service(pipe_fds[0]);
		idle_until_stopped(stalled);
	});
	sleep_ms(200);
	Thermal::level = 2;
	Thermal::processors = 4;
	sleep_ms(1000);
	const std::string during = stall_slot(path);
	require(vcpu_events(during) == 1, "one event is open:\n" + during);
	require(during.find("+ ..a ") != std::string::npos,
		"the third carrier is stalled on a device service's answer:\n" + during);
	require(during.find("\nthermal ") != std::string::npos &&
		during.find("\npower ") != std::string::npos, "transitions:\n" + during);

	/* The service answers: the event closes. */
	require(write(pipe_fds[1], "x", 1) == 1);
	sleep_ms(500);
	const std::string after = stall_slot(path);
	require(vcpu_events(after) == 1 && after.find("+ ..a ") == std::string::npos &&
		after.find(" ..a ") != std::string::npos,
		"the event is closed:\n" + after);

	/* Ending the watch leaves a valid slot of exactly its size. */
	watch.reset();
	const std::string last = stall_slot(path);
	require(last.size() == session_stall_slot_bytes && last.back() == '\n');
	/* The thermal source was read, and never from a thread the fence holds. */
	require(Thermal::readings.load() > 0, "the thermal source was never read");
	require(Thermal::from_held_threads.load() == 0,
		"the thermal source was read from a thread of the process's own");
	/* The main queue was asked while the stall was open, and never from a
	 * thread the fence holds. */
	require(pings_from_other_threads.load() > 0,
		"the main queue was never asked during the stall");
	require(pings_from_held_threads.load() == 0,
		"the main queue was asked from a thread of the process's own");

	for (Carrier *carrier : {&idle0, &idle1, &stalled})
		carrier->stop.store(true, std::memory_order_release);
	for (Carrier *carrier : {&idle0, &idle1, &stalled})
		require(pthread_join(carrier->thread, nullptr) == 0);
	close(pipe_fds[0]);
	close(pipe_fds[1]);
	uml::runtime::reset_session_record();
}

void a_hold_of_the_threads_is_no_stall()
{
	const uml::testing::ScratchSubdirectory directory{"uml-stall-fence.XXXXXX"};
	const std::string path = (directory.path() / "session").string();
	uml::runtime::reset_session_record();
	require(uml::runtime::begin_session_record(path, {}), "begin the record");

	Carrier idle0;
	start(idle0, 0, [&] { idle_until_stopped(idle0); });
	auto watch = uml::runtime::start_stall_watch(std::make_unique<Thermal>());
	require(watch != nullptr);
	sleep_ms(300);

	/* The fence holds every thread named uml-*, the watch's included, and the
	 * registered carrier; it wakes late. */
	uml::runtime::ThreadFence fence;
	require(fence.hold(2000), "the watch thread could not be held");
	sleep_ms(600);
	fence.release();
	sleep_ms(400);

	/* No event names a vCPU, and none is as late as the hold was; a loaded
	 * host may still make a short lateness of its own. */
	const std::string slot = stall_slot(path);
	require(vcpu_events(slot) == 0 && late_events(slot, 400) == 0,
		"a hold of the threads was recorded as a stall:\n" + slot);

	watch.reset();
	idle0.stop.store(true, std::memory_order_release);
	require(pthread_join(idle0.thread, nullptr) == 0);
	uml::runtime::reset_session_record();
}

/* The record's times are on the clock that counts the system's sleep. */
void the_record_is_on_the_continuous_clock()
{
	const auto continuous = static_cast<long long>(
		clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW));
	const auto ours = static_cast<long long>(uml::runtime::stall_clock_ns());
	const auto uptime = static_cast<long long>(
		clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
	require(ours - continuous < 50'000'000 && continuous - ours < 50'000'000,
		"the record's clock is not the continuous one");
	/* The continuous clock is the uptime and the time spent asleep. */
	require(ours + 50'000'000 >= uptime);
}

void no_session_means_no_watch()
{
	uml::runtime::reset_session_record();
	require(uml::runtime::start_stall_watch() == nullptr);
}

} // namespace

int main()
{
	no_session_means_no_watch();
	the_record_is_on_the_continuous_clock();
	a_stalled_carrier_is_recorded_and_idle_ones_are_not();
	a_hold_of_the_threads_is_no_stall();
	std::puts("stall watch tests passed");
	return 0;
}
