// SPDX-License-Identifier: GPL-2.0-or-later
#include "stall_watch.hpp"

#include "kernel_threads.hpp"
#include "session_record.hpp"
#include "stall_record.hpp"
#include "vcpu_carrier_name.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/semaphore.h>
#include <mach/task.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <memory>
#include <string_view>
#include <sys/resource.h>
#include <time.h>
#include <utility>

namespace uml::runtime {
namespace {

using namespace ::uml::runtime::stall;
using ::uml::platform::HostThermalLevel;
using ::uml::platform::HostThermalProvider;
using ::uml::platform::HostThermalState;

constexpr std::uint64_t nanoseconds_per_millisecond = 1'000'000;
constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000;
/* How often the host's thermal state, the memory pressure and the main queue
 * are looked at, and the process's counters sampled. */
constexpr std::uint64_t host_poll_ms = 250;
constexpr std::uint64_t context_period_ms = 1000;
/* An event that is open is written this often. */
constexpr std::uint64_t open_write_ms = 1000;
/* The context ring covers the seconds before an event. */
constexpr std::size_t context_ring = 8;
constexpr std::uint64_t before_window_ns = 5 * nanoseconds_per_second;
/* A thread that has no name yet is looked at again this often. */
constexpr std::uint64_t unnamed_retry_ms = 200;

mach_timebase_info_data_t timebase() noexcept
{
	static const mach_timebase_info_data_t base = [] {
		mach_timebase_info_data_t value{};
		(void)mach_timebase_info(&value);
		if (value.numer == 0 || value.denom == 0)
			value = {1, 1};
		return value;
	}();
	return base;
}

std::uint64_t ticks_to_ns(std::uint64_t ticks) noexcept
{
	const auto base = timebase();
	return ticks * base.numer / base.denom;
}

/* The continuous clock keeps counting while the system sleeps, as the wall
 * clock and the guest's own do, so a time in the record is the base plus its
 * offset whatever the session slept through. */
std::uint64_t continuous_ns() noexcept
{
	return ticks_to_ns(mach_continuous_time());
}

std::int64_t wall_seconds() noexcept
{
	timespec time{};
	(void)clock_gettime(CLOCK_REALTIME, &time);
	return static_cast<std::int64_t>(time.tv_sec);
}

/* ---- The main queue's lateness. ----
 * The callbacks outlive any watch, so they keep to process-wide state. */
struct MainQueuePing final {
	std::atomic<bool> outstanding{false};
	std::atomic<std::uint64_t> sent_ns{0};
	/* The worst answer since the last reading, -1 for none. */
	std::atomic<std::int64_t> worst_ms{-1};
	/* Whether the main queue has ever answered: a process that runs no main
	 * queue, as a command-line run, never does, and then it says nothing. */
	std::atomic<bool> serviced{false};
	/* The watch wants the main queue asked: an event is open or a stall is
	 * being confirmed. */
	std::atomic<bool> wanted{false};
};

MainQueuePing main_queue_ping;

void main_queue_answered(void *) noexcept
{
	const std::uint64_t sent = main_queue_ping.sent_ns.load(
		std::memory_order_acquire);
	const auto late = static_cast<std::int64_t>(
		(continuous_ns() - sent) / nanoseconds_per_millisecond);
	std::int64_t worst = main_queue_ping.worst_ms.load(std::memory_order_relaxed);
	while (late > worst && !main_queue_ping.worst_ms.compare_exchange_weak(
		       worst, late, std::memory_order_relaxed)) {
	}
	main_queue_ping.serviced.store(true, std::memory_order_release);
	main_queue_ping.outstanding.store(false, std::memory_order_release);
}

/* Asks the main queue, if the watch wants it asked and no ask is out. Only
 * the host observer's queue calls it: dispatch_async_f is libdispatch, which
 * the watch's own thread, a thread the fence can hold, never calls. */
void send_main_queue_ping(std::uint64_t now_ns) noexcept
{
	if (!main_queue_ping.wanted.load(std::memory_order_relaxed) ||
	    main_queue_ping.outstanding.load(std::memory_order_acquire))
		return;
	main_queue_ping.sent_ns.store(now_ns, std::memory_order_release);
	main_queue_ping.outstanding.store(true, std::memory_order_release);
	dispatch_async_f(dispatch_get_main_queue(), nullptr, &main_queue_answered);
}

/* The worst lateness of the main queue since the last call, -1 when it is
 * not serviced. */
std::int32_t main_queue_lateness_ms(std::uint64_t now_ns) noexcept
{
	if (!main_queue_ping.serviced.load(std::memory_order_acquire))
		return -1;
	std::int64_t worst = main_queue_ping.worst_ms.exchange(-1,
		std::memory_order_relaxed);
	if (main_queue_ping.outstanding.load(std::memory_order_acquire)) {
		const std::uint64_t sent = main_queue_ping.sent_ns.load(
			std::memory_order_acquire);
		if (now_ns > sent)
			worst = std::max<std::int64_t>(worst,
				static_cast<std::int64_t>((now_ns - sent) /
					nanoseconds_per_millisecond));
	}
	return static_cast<std::int32_t>(std::min<std::int64_t>(worst, 0x7fffffff));
}

/* ---- The memory pressure level. ---- */
std::atomic<std::uint8_t> memory_pressure_level{0};

void memory_pressure_changed(void *context) noexcept
{
	const auto source = static_cast<dispatch_source_t>(context);
	const auto flags = dispatch_source_get_data(source);
	memory_pressure_level.store(
		(flags & DISPATCH_MEMORYPRESSURE_CRITICAL) != 0 ? 2 :
		(flags & DISPATCH_MEMORYPRESSURE_WARN) != 0 ? 1 : 0,
		std::memory_order_relaxed);
}

/* ---- What the process's counters read at one moment. ---- */
struct Context final {
	std::uint64_t at_ns{};
	std::uint64_t cpu_ns{};
	std::uint64_t performance_ns{};
	std::uint64_t energy_nj{};
	std::uint64_t involuntary{};
	std::uint64_t footprint{};
	std::int64_t left{-1};
	std::uint64_t compressed{};
	std::uint32_t pageins{};
	std::uint32_t faults{};
	std::uint64_t system_pageins{};
	std::uint64_t system_decompressions{};
};

std::uint16_t hundredths(std::uint64_t amount, std::uint64_t wall_ns) noexcept
{
	if (wall_ns == 0)
		return 0;
	return static_cast<std::uint16_t>(std::min<std::uint64_t>(
		amount * 100 / wall_ns, 9999));
}

std::uint32_t mebibytes(std::uint64_t bytes) noexcept
{
	return static_cast<std::uint32_t>(std::min<std::uint64_t>(bytes >> 20,
		0xffffffffU));
}

RunState run_state(int value) noexcept
{
	switch (value) {
	case TH_STATE_RUNNING:
		return RunState::running;
	case TH_STATE_STOPPED:
		return RunState::stopped;
	case TH_STATE_UNINTERRUPTIBLE:
		return RunState::uninterruptible;
	case TH_STATE_HALTED:
		return RunState::halted;
	default:
		return RunState::waiting;
	}
}

/* A registry thread's identity: which vCPU it carries, if any. */
struct Identity final {
	mach_port_t port{MACH_PORT_NULL};
	/* >= 0 the vCPU; -1 not a carrier; -2 not named yet. */
	int cpu{-1};
	std::uint64_t retry_ns{};
};

} // namespace

struct StallWatch::State final {
	std::unique_ptr<HostThermalProvider> thermal;
	StallRecord record;
	StallDetector detector{record};
	KernelThreadSnapshot snapshot{};
	Identity known[kernel_thread_capacity]{};
	char slot[session_stall_slot_bytes]{};

	std::atomic<bool> stopping{false};
	pthread_t thread{};
	bool started{};

	mach_port_t host{MACH_PORT_NULL};
	/* The tick's wait, in libsystem_kernel alone because the thread fence
	 * defers a thread inside libsystem_c on every pass; a signal on it ends
	 * the watch at once. */
	semaphore_t wake{MACH_PORT_NULL};
	/* The host observer: a serial queue of the system's worker threads,
	 * which the fence never holds, takes the memory pressure and runs a timer
	 * that reads the thermal source and asks the main queue. The watch thread
	 * calls into no framework: it reads what the observer left in atomics. */
	dispatch_queue_t observer_queue{};
	dispatch_source_t pressure_source{};
	dispatch_source_t observer_timer{};
	std::atomic<std::uint32_t> observed_thermal{0};

	/* Thread-confined state. */
	std::uint32_t fence_epoch{};
	HostThermalState host_state{};
	std::uint8_t pressure{};
	std::uint64_t next_poll_ns{};
	std::uint64_t next_context_ns{};
	std::uint64_t next_write_ns{};
	bool idle_seen{};
	Context recent[context_ring]{};
	std::size_t recent_count{};
	Context opened_at{};

	explicit State(std::unique_ptr<HostThermalProvider> source)
		: thermal{std::move(source)}
	{
	}

	/* ---- What the host observer saw. ---- */

	static std::uint32_t pack(const HostThermalState &state) noexcept
	{
		return static_cast<std::uint32_t>(state.level) |
			(state.lowPowerMode ? 0x100U : 0U) |
			static_cast<std::uint32_t>(state.activeProcessors) << 16;
	}

	[[nodiscard]] HostThermalState observed() const noexcept
	{
		const std::uint32_t word = observed_thermal.load(std::memory_order_relaxed);
		HostThermalState state;
		state.level = static_cast<HostThermalLevel>(word & 0xffU);
		state.lowPowerMode = (word & 0x100U) != 0;
		state.activeProcessors = static_cast<std::uint8_t>(word >> 16 & 0xffU);
		return state;
	}

	/* On the observer's queue. */
	static void observe_host(void *argument) noexcept
	{
		auto *self = static_cast<State *>(argument);
		self->observed_thermal.store(pack(self->thermal->state()),
			std::memory_order_relaxed);
		send_main_queue_ping(continuous_ns());
	}

	/* ---- Reading the system. ---- */

	[[nodiscard]] Context read_context(std::uint64_t now_ns) const noexcept
	{
		Context context;
		context.at_ns = now_ns;
		rusage usage{};
		if (getrusage(RUSAGE_SELF, &usage) == 0) {
			context.cpu_ns = (static_cast<std::uint64_t>(usage.ru_utime.tv_sec) +
				static_cast<std::uint64_t>(usage.ru_stime.tv_sec)) *
				nanoseconds_per_second +
				(static_cast<std::uint64_t>(usage.ru_utime.tv_usec) +
				 static_cast<std::uint64_t>(usage.ru_stime.tv_usec)) * 1000;
			context.involuntary = static_cast<std::uint64_t>(usage.ru_nivcsw);
		}
		task_power_info_v2_data_t power{};
		mach_msg_type_number_t power_count = TASK_POWER_INFO_V2_COUNT;
		if (task_info(mach_task_self(), TASK_POWER_INFO_V2,
			    reinterpret_cast<task_info_t>(&power), &power_count) ==
		    KERN_SUCCESS) {
			context.energy_nj = power.task_energy;
			context.performance_ns = ticks_to_ns(power.task_ptime);
		}
		task_vm_info_data_t memory{};
		mach_msg_type_number_t memory_count = TASK_VM_INFO_COUNT;
		if (task_info(mach_task_self(), TASK_VM_INFO,
			    reinterpret_cast<task_info_t>(&memory), &memory_count) ==
		    KERN_SUCCESS) {
			context.footprint = memory.phys_footprint;
			context.compressed = memory.compressed;
			/* Zero where the host sets no limit, as on a Mac. */
			if (memory_count >= TASK_VM_INFO_COUNT &&
			    memory.limit_bytes_remaining != 0)
				context.left = static_cast<std::int64_t>(
					memory.limit_bytes_remaining);
		}
		task_events_info_data_t events{};
		mach_msg_type_number_t events_count = TASK_EVENTS_INFO_COUNT;
		if (task_info(mach_task_self(), TASK_EVENTS_INFO,
			    reinterpret_cast<task_info_t>(&events), &events_count) ==
		    KERN_SUCCESS) {
			context.pageins = static_cast<std::uint32_t>(events.pageins);
			context.faults = static_cast<std::uint32_t>(events.faults);
		}
		if (host != MACH_PORT_NULL) {
			vm_statistics64_data_t system{};
			mach_msg_type_number_t system_count = HOST_VM_INFO64_COUNT;
			if (host_statistics64(host, HOST_VM_INFO64,
				    reinterpret_cast<host_info64_t>(&system),
				    &system_count) == KERN_SUCCESS) {
				context.system_pageins = system.pageins;
				context.system_decompressions = system.decompressions;
			}
		}
		return context;
	}

	void sample_context(std::uint64_t now_ns) noexcept
	{
		if (recent_count == context_ring) {
			std::copy(recent + 1, recent + context_ring, recent);
			--recent_count;
		}
		recent[recent_count++] = read_context(now_ns);
	}

	/* ---- The threads. ---- */

	void resolve(Identity &identity, mach_port_t port,
		std::uint64_t now_ns) noexcept
	{
		identity.port = port;
		thread_extended_info_data_t information{};
		mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
		if (thread_info(port, THREAD_EXTENDED_INFO,
			    reinterpret_cast<thread_info_t>(&information),
			    &count) != KERN_SUCCESS) {
			identity.cpu = -1;
			return;
		}
		const std::string_view name{information.pth_name,
			::strnlen(information.pth_name, sizeof(information.pth_name))};
		if (name.empty()) {
			identity.cpu = -2;
			identity.retry_ns = now_ns + unnamed_retry_ms *
				nanoseconds_per_millisecond;
			return;
		}
		const int cpu = vcpu_carrier_index(name);
		identity.cpu = cpu < static_cast<int>(max_vcpus) ? cpu : -1;
	}

	void read_carriers(Tick &tick) noexcept
	{
		snapshot_kernel_threads(snapshot);
		for (std::size_t index = 0; index < snapshot.thread_count; ++index) {
			const mach_port_t port = snapshot.threads[index];
			Identity &identity = known[index];
			if (identity.port != port ||
			    (identity.cpu == -2 && tick.now_ns >= identity.retry_ns))
				resolve(identity, port, tick.now_ns);
			if (identity.cpu < 0)
				continue;
			thread_extended_info_data_t information{};
			mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
			if (thread_info(port, THREAD_EXTENDED_INFO,
				    reinterpret_cast<thread_info_t>(&information),
				    &count) != KERN_SUCCESS)
				continue;
			VcpuSample &sample = tick.vcpus[identity.cpu];
			sample.present = true;
			sample.state = run_state(information.pth_run_state);
			sample.cpu_ns = information.pth_user_time +
				information.pth_system_time;
			sample.wait_word = snapshot.waits[index];
		}
	}

	/* ---- The host's state. ---- */

	void write_slot() noexcept
	{
		(void)format_stall_slot(slot, sizeof(slot), record, wall_seconds());
		write_session_stall_slot({slot, sizeof(slot)});
	}

	void poll_host(std::uint64_t now_ns) noexcept
	{
		bool changed = false;
		const HostThermalState state = observed();
		const std::uint32_t at = detector.since_base_ms(now_ns);
		if (state.level != host_state.level) {
			record.thermal.push({at,
				static_cast<std::uint8_t>(host_state.level),
				static_cast<std::uint8_t>(state.level)});
			changed = true;
		}
		if (state.lowPowerMode != host_state.lowPowerMode ||
		    state.activeProcessors != host_state.activeProcessors) {
			record.power.push({at, static_cast<std::uint8_t>(
				state.lowPowerMode ? 1 : 0), state.activeProcessors});
			changed = true;
		}
		host_state = state;
		const std::uint8_t level = memory_pressure_level.load(
			std::memory_order_relaxed);
		if (level != pressure) {
			record.pressure.push({at, level, pressure});
			pressure = level;
			changed = true;
		}
		if (changed)
			write_slot();
	}

	/* ---- Events. ---- */

	void begin_context(StallEvent &event, std::uint64_t now_ns) noexcept
	{
		opened_at = read_context(now_ns);
		const HostThermalState state = observed();
		event.thermal_start = static_cast<std::uint8_t>(state.level);
		event.low_power = state.lowPowerMode;
		event.processors_start = state.activeProcessors;
		event.pressure = memory_pressure_level.load(std::memory_order_relaxed);
		/* The five seconds before, from the samples the ring holds. */
		if (recent_count >= 2) {
			const Context &last = recent[recent_count - 1];
			std::size_t first = recent_count - 1;
			while (first > 0 &&
			       last.at_ns - recent[first - 1].at_ns <= before_window_ns)
				--first;
			const Context &from = recent[first];
			const std::uint64_t wall = last.at_ns - from.at_ns;
			event.cores_before = hundredths(last.cpu_ns - from.cpu_ns, wall);
			event.performance_before = hundredths(
				last.performance_ns - from.performance_ns, wall);
			event.watts_before = hundredths(
				last.energy_nj - from.energy_nj, wall);
		}
	}

	void end_context(StallEvent &event, std::uint64_t now_ns) noexcept
	{
		const Context closed = read_context(now_ns);
		const std::uint64_t wall = closed.at_ns - opened_at.at_ns;
		event.cores_during = hundredths(closed.cpu_ns - opened_at.cpu_ns, wall);
		event.performance_during = hundredths(
			closed.performance_ns - opened_at.performance_ns, wall);
		event.watts_during = hundredths(
			closed.energy_nj - opened_at.energy_nj, wall);
		event.involuntary_switches = static_cast<std::uint32_t>(
			closed.involuntary - opened_at.involuntary);
		event.footprint_mib = mebibytes(closed.footprint);
		event.left_mib = closed.left < 0 ? -1 :
			static_cast<std::int32_t>(std::min<std::uint64_t>(
				mebibytes(static_cast<std::uint64_t>(closed.left)), 0x7fffffff));
		event.compressed_mib = static_cast<std::int32_t>(std::clamp<std::int64_t>(
			(static_cast<std::int64_t>(closed.compressed) -
			 static_cast<std::int64_t>(opened_at.compressed)) / (1 << 20),
			-0x7fffffff, 0x7fffffff));
		event.pageins = closed.pageins - opened_at.pageins;
		event.faults = closed.faults - opened_at.faults;
		event.system_pageins = static_cast<std::uint32_t>(std::min<std::uint64_t>(
			closed.system_pageins - opened_at.system_pageins, 0xffffffffU));
		event.system_decompressions = static_cast<std::uint32_t>(
			std::min<std::uint64_t>(closed.system_decompressions -
				opened_at.system_decompressions, 0xffffffffU));
		const HostThermalState state = observed();
		event.thermal_end = static_cast<std::uint8_t>(state.level);
		event.processors_end = state.activeProcessors;
		event.low_power = event.low_power || state.lowPowerMode;
		event.pressure = std::max(event.pressure,
			memory_pressure_level.load(std::memory_order_relaxed));
	}

	void handle(StallDetector::Change change, std::uint64_t now_ns) noexcept
	{
		switch (change) {
		case StallDetector::Change::none:
			return;
		case StallDetector::Change::opened:
			begin_context(record.events.back(), now_ns);
			write_slot();
			next_write_ns = now_ns + open_write_ms * nanoseconds_per_millisecond;
			return;
		case StallDetector::Change::updated:
			if (now_ns >= next_write_ns) {
				end_context(record.events.back(), now_ns);
				write_slot();
				next_write_ns = now_ns +
					open_write_ms * nanoseconds_per_millisecond;
			}
			return;
		case StallDetector::Change::closed:
			end_context(record.events.back(), now_ns);
			write_slot();
			return;
		}
	}

	void tick(std::uint64_t now_ns, std::uint32_t late_ms) noexcept
	{
		Tick tick;
		tick.now_ns = now_ns;
		tick.late_ms = late_ms;
		const std::uint32_t epoch = kernel_thread_fence_epoch();
		tick.disturbed = kernel_thread_reclaim_inhibited() || epoch != fence_epoch;
		fence_epoch = epoch;
		tick.ui_late_ms = main_queue_lateness_ms(now_ns);
		read_carriers(tick);
		handle(detector.observe(tick), now_ns);
		main_queue_ping.wanted.store(detector.suspicious(),
			std::memory_order_relaxed);
		/* The first sight of a carrier in a marked idle wait says the
		 * markers reach the wrappers; it is written once, then rides with
		 * the next write. */
		if (!idle_seen && record.idle_samples != 0) {
			idle_seen = true;
			write_slot();
		}
		if (now_ns >= next_context_ns) {
			sample_context(now_ns);
			next_context_ns = now_ns + context_period_ms *
				nanoseconds_per_millisecond;
		}
		if (now_ns >= next_poll_ns) {
			poll_host(now_ns);
			next_poll_ns = now_ns + host_poll_ms * nanoseconds_per_millisecond;
		}
	}

	void run() noexcept
	{
		(void)pthread_setname_np("uml-stall-watch");
		/* This thread's local storage exists before anything marks a wait. */
		::uml::platform::current_wait_cell = nullptr;
		/* The set-up is done before the measurement starts, so its time is
		 * nobody's lateness. */
		host_state = observed();
		sample_context(continuous_ns());
		std::uint64_t now_ns = continuous_ns();
		detector.reset(now_ns);
		record.base_unix = wall_seconds();
		next_context_ns = now_ns + context_period_ms * nanoseconds_per_millisecond;
		next_poll_ns = now_ns;
		write_slot();
		constexpr std::uint64_t period_ns = tick_ms * nanoseconds_per_millisecond;
		/* The watch wakes at a deadline of its own and measures how late it
		 * woke; the work of a tick (a slow write of the slot, say) is not
		 * part of that. */
		std::uint64_t deadline_ns = continuous_ns() + period_ns;
		while (!stopping.load(std::memory_order_acquire)) {
			now_ns = continuous_ns();
			if (now_ns < deadline_ns) {
				const std::uint64_t wait_ns = deadline_ns - now_ns;
				const mach_timespec_t period{
					static_cast<unsigned int>(wait_ns / nanoseconds_per_second),
					static_cast<clock_res_t>(wait_ns % nanoseconds_per_second)};
				(void)semaphore_timedwait(wake, period);
				now_ns = continuous_ns();
			}
			if (stopping.load(std::memory_order_acquire))
				break;
			const std::uint64_t late_ns =
				now_ns > deadline_ns ? now_ns - deadline_ns : 0;
			tick(now_ns, static_cast<std::uint32_t>(std::min<std::uint64_t>(
				late_ns / nanoseconds_per_millisecond, 0xffffffffU)));
			deadline_ns = continuous_ns() + period_ns;
		}
		/* An event in progress ends where it was last seen. */
		Tick last;
		last.now_ns = continuous_ns();
		last.disturbed = true;
		handle(detector.observe(last), last.now_ns);
		write_slot();
	}

	static void *entry(void *argument) noexcept
	{
		static_cast<State *>(argument)->run();
		return nullptr;
	}

	void start_observer() noexcept
	{
		observed_thermal.store(pack(thermal->state()), std::memory_order_relaxed);
		observer_queue = dispatch_queue_create("uml.stall-watch.host",
			DISPATCH_QUEUE_SERIAL);
		if (observer_queue == nullptr)
			return;
		pressure_source = dispatch_source_create(
			DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
			DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN |
				DISPATCH_MEMORYPRESSURE_CRITICAL,
			observer_queue);
		if (pressure_source != nullptr) {
			dispatch_set_context(pressure_source, pressure_source);
			dispatch_source_set_event_handler_f(pressure_source,
				&memory_pressure_changed);
			dispatch_resume(pressure_source);
		}
		observer_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
			observer_queue);
		if (observer_timer != nullptr) {
			dispatch_set_context(observer_timer, this);
			dispatch_source_set_event_handler_f(observer_timer, &State::observe_host);
			dispatch_source_set_timer(observer_timer, DISPATCH_TIME_NOW,
				host_poll_ms * nanoseconds_per_millisecond,
				host_poll_ms * nanoseconds_per_millisecond / 4);
			dispatch_resume(observer_timer);
		}
	}

	void stop_observer() noexcept
	{
		if (observer_timer != nullptr) {
			dispatch_source_cancel(observer_timer);
			dispatch_release(observer_timer);
			observer_timer = nullptr;
		}
		if (pressure_source != nullptr) {
			dispatch_source_cancel(pressure_source);
			dispatch_release(pressure_source);
			pressure_source = nullptr;
		}
		if (observer_queue != nullptr) {
			/* A handler that is running ends before the state goes. */
			dispatch_sync_f(observer_queue, nullptr, [](void *) {});
			dispatch_release(observer_queue);
			observer_queue = nullptr;
		}
	}

	void start() noexcept
	{
		if (semaphore_create(mach_task_self(), &wake, SYNC_POLICY_FIFO, 0) !=
		    KERN_SUCCESS)
			return;
		host = mach_host_self();
		memory_pressure_level.store(0, std::memory_order_relaxed);
		start_observer();
		/* The thread takes no signal: the kernel's process-directed signals
		 * must never pick it. */
		sigset_t all;
		sigset_t previous;
		sigfillset(&all);
		if (pthread_sigmask(SIG_SETMASK, &all, &previous) != 0)
			return;
		pthread_attr_t attributes;
		if (pthread_attr_init(&attributes) == 0) {
			(void)pthread_attr_setstacksize(&attributes, 128 * 1024);
			(void)pthread_attr_set_qos_class_np(&attributes,
				QOS_CLASS_USER_INTERACTIVE, 0);
			started = pthread_create(&thread, &attributes, &State::entry,
				this) == 0;
			(void)pthread_attr_destroy(&attributes);
		}
		(void)pthread_sigmask(SIG_SETMASK, &previous, nullptr);
	}

	void stop() noexcept
	{
		if (started) {
			stopping.store(true, std::memory_order_release);
			(void)semaphore_signal(wake);
			(void)pthread_join(thread, nullptr);
			started = false;
		}
		if (wake != MACH_PORT_NULL) {
			(void)semaphore_destroy(mach_task_self(), wake);
			wake = MACH_PORT_NULL;
		}
		stop_observer();
		if (host != MACH_PORT_NULL) {
			(void)mach_port_deallocate(mach_task_self(), host);
			host = MACH_PORT_NULL;
		}
	}
};

StallWatch::StallWatch(std::unique_ptr<HostThermalProvider> thermal)
	: state_{std::make_unique<State>(
		  thermal != nullptr ? std::move(thermal) :
		  ::uml::platform::makeAppleHostThermalProvider())}
{
	state_->start();
}

StallWatch::~StallWatch()
{
	state_->stop();
}

std::uint64_t stall_clock_ns() noexcept
{
	return continuous_ns();
}

std::unique_ptr<StallWatch> start_stall_watch(
	std::unique_ptr<HostThermalProvider> thermal)
{
	if (!session_record_open())
		return nullptr;
	return std::make_unique<StallWatch>(std::move(thermal));
}

} // namespace uml::runtime
