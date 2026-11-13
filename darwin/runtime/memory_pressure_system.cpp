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
#include <cstring>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <new>
#include <utility>
#include <time.h>
#include <unistd.h>

namespace uml::runtime {

namespace {

/* The longest line the console gets in one write. */
constexpr std::size_t line_limit = 1000;

class SystemActions final : public PressureActions {
public:
	SystemActions(GuestTrim trim, void *context) noexcept
		: trim_{trim}, context_{context}
	{
	}

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

	TrimOutcome trim_page_cache() noexcept override
	{
		return trim_ == nullptr ? TrimOutcome::skipped : trim_(context_);
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

private:
	GuestTrim trim_;
	void *context_;
};

} // namespace

struct MemoryPressureWatch::State final {
	State(GuestTrim trim, void *context) noexcept
		: actions{trim, context}, response{actions}
	{
	}

	~State()
	{
		if (source != nullptr)
			dispatch_source_cancel(source);
		if (queue != nullptr) {
			/* A response that is running ends before the state goes, and
			 * none starts after the cancel. */
			dispatch_sync_f(queue, nullptr, [](void *) {});
			dispatch_release(queue);
		}
		if (source != nullptr)
			dispatch_release(source);
	}

	State(const State &) = delete;
	State &operator=(const State &) = delete;

	/* On the watch's queue. The source's data is the status flags the system
	 * raised since the last call. */
	static void changed(void *context) noexcept
	{
		auto *const state = static_cast<State *>(context);
		const std::uint64_t flags = dispatch_source_get_data(state->source);
		(void)state->response.on_level(pressure_level_from_dispatch(flags));
	}

	SystemActions actions;
	MemoryPressureResponse response;
	dispatch_queue_t queue{};
	dispatch_source_t source{};
};

MemoryPressureWatch::MemoryPressureWatch(std::unique_ptr<State> state) noexcept
	: state_{std::move(state)}
{
}

MemoryPressureWatch::~MemoryPressureWatch() = default;

std::unique_ptr<MemoryPressureWatch> start_memory_pressure_response(
	GuestTrim trim, void *context)
{
	auto state = std::unique_ptr<MemoryPressureWatch::State>{
		new (std::nothrow) MemoryPressureWatch::State{trim, context}};
	if (state == nullptr)
		return nullptr;
	/* A queue of its own: a trim waits for the guest, and what else a shared
	 * queue serves must not wait behind it. */
	state->queue = dispatch_queue_create("uml.memory-pressure",
		dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
			QOS_CLASS_USER_INITIATED, 0));
	if (state->queue == nullptr)
		return nullptr;
	state->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE,
		0,
		DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN |
			DISPATCH_MEMORYPRESSURE_CRITICAL,
		state->queue);
	if (state->source == nullptr)
		return nullptr;
	dispatch_set_context(state->source, state.get());
	dispatch_source_set_event_handler_f(state->source,
		&MemoryPressureWatch::State::changed);
	dispatch_resume(state->source);
	return std::unique_ptr<MemoryPressureWatch>{
		new (std::nothrow) MemoryPressureWatch{std::move(state)}};
}

} // namespace uml::runtime
