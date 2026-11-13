// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The system's side of the memory pressure response (memory_pressure_response.
 * hpp): the memory pressure source, and the clock, figures and actions the
 * policy reads and takes from the process.
 */
#include "memory_pressure_response.hpp"

#include "guest_statistics.hpp"
#include "platform/gpu_cache_reclaim.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <memory>
#include <mutex>
#include <new>
#include <sys/sysctl.h>
#include <utility>
#include <time.h>
#include <unistd.h>

namespace uml::runtime {

namespace {

/* The longest line the console gets in one write. */
constexpr std::size_t line_limit = 1000;

/* The process's one observer of the reports. A report is delivered with the
 * mutex held, which is what makes clearing the observer wait for a call in
 * progress, and a report allocates nothing: it is made when the system is short
 * of memory. The pressure queue outlives static destruction (a guest still runs
 * when the process exits), so the slot is never destroyed: an event that comes
 * after exit() has run the other destructors still finds a live mutex. */
struct ObserverSlot final {
	std::mutex mutex;
	PressureObserver observer{};
	void *context{};
};

[[clang::no_destroy]] constinit ObserverSlot observer_slot;

class SystemActions final : public PressureActions {
public:
	SystemActions(GuestTrim trim, void *context) noexcept
		: trim_{trim}, context_{context}
	{
	}

	/* The clock of the 30 s between warnings and of the lines' windows:
	 * monotonic, and stopped while the system sleeps, so a long sleep does
	 * not end the interval. */
	std::uint64_t now_ns() noexcept override
	{
		return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	}

	PressureFigures figures() noexcept override
	{
		PressureFigures figures;
		task_vm_info_data_t information{};
		mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
		if (task_info(mach_task_self(), TASK_VM_INFO,
			    reinterpret_cast<task_info_t>(&information), &count) ==
		    KERN_SUCCESS) {
			figures.footprint = information.phys_footprint;
			if (count >= TASK_VM_INFO_REV3_COUNT)
				figures.graphics = static_cast<std::uint64_t>(
					std::max<std::int64_t>(
						information.ledger_tag_graphics_footprint +
							information.ledger_tag_graphics_footprint_compressed,
						0));
		}
		if (const auto ram = guest_ram_usage()) {
			figures.guest_held = ram->held_bytes;
			figures.wire_failures = ram->wire_failures;
		}
		return figures;
	}

	std::uint64_t reclaim_gpu() noexcept override
	{
		const auto reclaim = ::uml::platform::publishedGpuCacheReclaimer();
		return reclaim == nullptr ? 0U : reclaim();
	}

	TrimOutcome trim_page_cache(TrimKind kind) noexcept override
	{
		return trim_ == nullptr ? TrimOutcome::unreachable :
			trim_(context_, kind);
	}

	/* The level the kernel holds now (1 normal, 2 warn, 4 critical, the
	 * source's flags). A sandbox may refuse the question. */
	SystemLevel system_level() noexcept override
	{
		int value = 0;
		std::size_t size = sizeof(value);
		if (sysctlbyname("kern.memorystatus_vm_pressure_level", &value, &size,
			    nullptr, 0) != 0)
			return {.readable = false, .level = PressureLevel::normal,
				.error = errno};
		if (size != sizeof(value))
			return {.readable = false, .level = PressureLevel::normal,
				.error = EINVAL};
		return {.readable = true,
			.level = pressure_level_from_dispatch(
				static_cast<std::uint64_t>(value)),
			.error = 0};
	}

	void log(std::string_view line) noexcept override
	{
		char buffer[line_limit + 1];
		const std::size_t size = std::min(line.size(), line_limit);
		std::memcpy(buffer, line.data(), size);
		buffer[size] = '\n';
		/* One write, so that the line is not split among the console's
		 * other writers. */
		(void)write(STDERR_FILENO, buffer, size + 1);
	}

	void report(const PressureReport &report) noexcept override
	{
		const std::lock_guard lock{observer_slot.mutex};
		if (observer_slot.observer != nullptr)
			observer_slot.observer(observer_slot.context, report);
	}

private:
	GuestTrim trim_;
	void *context_;
};

/*
 * The process's memory pressure source and the serial queue that runs its
 * handler and the retries, made on the first watch and kept. A run binds its
 * state to it and unbinds before it returns. They are never cancelled or
 * released: cancelling a source leaves a libdispatch worker thread behind
 * (still there seconds later), and a run that ends leaves no thread of its own.
 * A source that nothing is bound to drops what it delivers.
 */
struct Hub final {
	dispatch_queue_t queue{};
	dispatch_source_t source{};
	/* The timer of the retries, made with the hub so that arming a retry
	 * allocates nothing of its own: a retry is wanted when the system is
	 * short of memory. One-shot, set again for each retry and disarmed at
	 * an unbind. It is resumed by the first retry: a resumed timer source
	 * costs the process a thread of libdispatch's, and a process that
	 * never needs a retry keeps the one thread of the pressure source. */
	dispatch_source_t retry_timer{};
	bool retry_timer_resumed{};
	/* The run that listens; touched only on the queue. */
	MemoryPressureWatch::State *current{};
	/* Counts every bind and unbind, so that a retry that fires after its run
	 * ended finds it; touched only on the queue. */
	std::uint64_t generation{};
	/* The run and the generation the timer was last set for; touched only
	 * on the queue. */
	MemoryPressureWatch::State *retry_state{};
	std::uint64_t retry_generation{};
};

} // namespace

struct MemoryPressureWatch::State final {
	State(Hub &hub_, GuestTrim trim, void *context,
		PressureTiming timing) noexcept
		: hub{hub_}, actions{trim, context}, response{actions, timing}
	{
	}

	~State();

	State(const State &) = delete;
	State &operator=(const State &) = delete;

	Hub &hub;
	SystemActions actions;
	MemoryPressureResponse response;
	/* A retry is due on the queue; only the queue touches it. */
	bool retry_armed{};
};

namespace {

void retry_due(void *context) noexcept;

/* On the queue, after anything that may have left a guest trim pending. */
void arm_retry(Hub &hub, MemoryPressureWatch::State &state) noexcept
{
	if (state.retry_armed)
		return;
	const std::uint64_t delay = state.response.retry_delay_ns();
	if (delay == 0U)
		return;
	state.retry_armed = true;
	hub.retry_state = &state;
	hub.retry_generation = hub.generation;
	dispatch_source_set_timer(hub.retry_timer,
		dispatch_time(DISPATCH_TIME_NOW, static_cast<std::int64_t>(delay)),
		DISPATCH_TIME_FOREVER, delay / 10U);
	if (!hub.retry_timer_resumed) {
		hub.retry_timer_resumed = true;
		dispatch_resume(hub.retry_timer);
	}
}

void disarm_retry(Hub &hub) noexcept
{
	hub.retry_state = nullptr;
	if (hub.retry_timer_resumed)
		dispatch_source_set_timer(hub.retry_timer, DISPATCH_TIME_FOREVER,
			DISPATCH_TIME_FOREVER, 0);
}

/* On the queue, when the timer fires. A retry that was set for a run that has
 * since ended finds the hub bound elsewhere, or bound again. */
void retry_due(void *context) noexcept
{
	Hub &hub = *static_cast<Hub *>(context);
	MemoryPressureWatch::State *const state = hub.current;
	if (state == nullptr || hub.retry_state != state ||
	    hub.retry_generation != hub.generation)
		return;
	state->retry_armed = false;
	(void)state->response.on_retry();
	arm_retry(hub, *state);
}

/* On the queue. The source's data is the status flags the system raised since
 * the last call. */
void level_changed(void *context) noexcept
{
	Hub &hub = *static_cast<Hub *>(context);
	const std::uint64_t flags = dispatch_source_get_data(hub.source);
	if (hub.current == nullptr)
		return;
	(void)hub.current->response.on_delivery(flags);
	arm_retry(hub, *hub.current);
}

Hub *make_hub() noexcept
{
	auto *const hub = new (std::nothrow) Hub;
	if (hub == nullptr)
		return nullptr;
	/* A queue of its own: a trim waits for the guest, and what else a shared
	 * queue serves must not wait behind it. */
	hub->queue = dispatch_queue_create("uml.memory-pressure",
		dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
			QOS_CLASS_USER_INITIATED, 0));
	if (hub->queue == nullptr)
		return nullptr;
	hub->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
		DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN |
			DISPATCH_MEMORYPRESSURE_CRITICAL,
		hub->queue);
	if (hub->source == nullptr)
		return nullptr;
	hub->retry_timer =
		dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, hub->queue);
	if (hub->retry_timer == nullptr)
		return nullptr;
	dispatch_set_context(hub->source, hub);
	dispatch_source_set_event_handler_f(hub->source, &level_changed);
	dispatch_resume(hub->source);
	dispatch_set_context(hub->retry_timer, hub);
	dispatch_source_set_event_handler_f(hub->retry_timer, &retry_due);
	return hub;
}

Hub *shared_hub() noexcept
{
	static Hub *const hub = make_hub();
	return hub;
}

struct Binding final {
	Hub *hub;
	MemoryPressureWatch::State *state;
};

/* Both wait, on the queue, for a handler or retry that is running. */
void bind(Hub &hub, MemoryPressureWatch::State *state) noexcept
{
	Binding binding{&hub, state};
	dispatch_sync_f(hub.queue, &binding, [](void *context) {
		auto &binding = *static_cast<Binding *>(context);
		binding.hub->current = binding.state;
		++binding.hub->generation;
	});
}

void unbind(Hub &hub, MemoryPressureWatch::State *state) noexcept
{
	Binding binding{&hub, state};
	dispatch_sync_f(hub.queue, &binding, [](void *context) {
		auto &binding = *static_cast<Binding *>(context);
		if (binding.hub->current == binding.state) {
			binding.hub->current = nullptr;
			++binding.hub->generation;
			disarm_retry(*binding.hub);
		}
	});
}

} // namespace

/* A response that is running ends before the state goes, and none, nor any
 * retry, starts after the unbind. */
MemoryPressureWatch::State::~State()
{
	unbind(hub, this);
}

MemoryPressureWatch::MemoryPressureWatch(std::unique_ptr<State> state) noexcept
	: state_{std::move(state)}
{
}

MemoryPressureWatch::~MemoryPressureWatch() = default;

void set_pressure_observer(PressureObserver observer, void *context) noexcept
{
	const std::lock_guard lock{observer_slot.mutex};
	observer_slot.observer = observer;
	observer_slot.context = context;
}

std::unique_ptr<MemoryPressureWatch> start_memory_pressure_response(
	GuestTrim trim, void *context, PressureTiming timing)
{
	Hub *const hub = shared_hub();
	if (hub == nullptr)
		return nullptr;
	auto state = std::unique_ptr<MemoryPressureWatch::State>{
		new (std::nothrow) MemoryPressureWatch::State{*hub, trim, context, timing}};
	if (state == nullptr)
		return nullptr;
	bind(*hub, state.get());
	state->response.announce_start();
	return std::unique_ptr<MemoryPressureWatch>{
		new (std::nothrow) MemoryPressureWatch{std::move(state)}};
}

} // namespace uml::runtime
