// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The memory pressure response (runtime/memory_pressure_response): the policy
 * against a clock and actions of the test's own, and the system's side of it,
 * the memory pressure source, against a source this binary substitutes: the
 * binary defines dispatch_source_create, so the response's call binds here and
 * a memory pressure source becomes one the test can raise (a real event needs
 * a privileged tool).
 */
#include "memory_pressure_response.hpp"
#include "named_tests.hpp"
#include "platform/gpu_cache_reclaim.hpp"
#include "require.hpp"
#include "wait_until.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

/* The memory pressure source the response made, as a source the test merges
 * data into: DISPATCH_SOURCE_TYPE_DATA_OR delivers the merged bits to the
 * handler as the status flags a memory pressure source delivers. */
std::atomic<dispatch_source_t> pressure_source{nullptr};

} // namespace

extern "C" dispatch_source_t dispatch_source_create(
	dispatch_source_type_t type, uintptr_t handle, uintptr_t mask,
	dispatch_queue_t queue)
{
	using Create = dispatch_source_t (*)(
		dispatch_source_type_t, uintptr_t, uintptr_t, dispatch_queue_t);
	static const auto real = reinterpret_cast<Create>(
		dlsym(RTLD_NEXT, "dispatch_source_create"));
	if (type != DISPATCH_SOURCE_TYPE_MEMORYPRESSURE)
		return real(type, handle, mask, queue);
	const dispatch_source_t source =
		real(DISPATCH_SOURCE_TYPE_DATA_OR, 0, 0, queue);
	pressure_source.store(source);
	return source;
}

namespace {

using uml::runtime::MemoryPressureResponse;
using uml::runtime::PressureActions;
using uml::runtime::PressureFigures;
using uml::runtime::PressureLevel;
using uml::runtime::TrimOutcome;
using uml::testing::require;
using uml::testing::waitUntil;
using Outcome = MemoryPressureResponse::Outcome;

constexpr std::uint64_t mib = 1024ULL * 1024ULL;
constexpr std::uint64_t second = 1'000'000'000ULL;
constexpr std::uint64_t millisecond = 1'000'000ULL;

/* A clock, a process and a guest the test moves by hand. */
class FakeActions final : public PressureActions {
public:
	std::uint64_t now_ns() noexcept override
	{
		const std::lock_guard lock{mutex};
		return now;
	}

	PressureFigures figures() noexcept override
	{
		const std::lock_guard lock{mutex};
		return current;
	}

	std::uint64_t reclaim_gpu() noexcept override
	{
		const std::lock_guard lock{mutex};
		calls.emplace_back("gpu");
		current.graphics -= std::min(gpu_bytes, current.graphics);
		current.footprint -= std::min(gpu_bytes, current.footprint);
		return gpu_freed;
	}

	TrimOutcome trim_page_cache() noexcept override
	{
		std::function<void()> inside;
		{
			const std::lock_guard lock{mutex};
			calls.emplace_back("trim");
			inside = inside_trim;
		}
		if (inside)
			inside();
		const std::lock_guard lock{mutex};
		now += trim_ns;
		const std::uint64_t released = std::min(trim_bytes, current.guest_held);
		current.guest_held -= released;
		current.footprint -= std::min(released, current.footprint);
		return outcome;
	}

	void log(std::string_view line) noexcept override
	{
		const std::lock_guard lock{mutex};
		lines.emplace_back(line);
	}

	void advance(std::uint64_t nanoseconds)
	{
		const std::lock_guard lock{mutex};
		now += nanoseconds;
	}

	[[nodiscard]] std::vector<std::string> calls_taken()
	{
		const std::lock_guard lock{mutex};
		return calls;
	}

	[[nodiscard]] std::vector<std::string> lines_taken()
	{
		const std::lock_guard lock{mutex};
		return lines;
	}

	[[nodiscard]] std::size_t trims()
	{
		const std::lock_guard lock{mutex};
		std::size_t count = 0;
		for (const auto &call : calls)
			count += call == "trim" ? 1U : 0U;
		return count;
	}

	std::mutex mutex;
	std::uint64_t now = 100 * second;
	PressureFigures current{
		.footprint = 2026 * mib,
		.guest_held = 1500 * mib,
		.graphics = 150 * mib,
		.wire_failures = 16,
	};
	std::uint64_t gpu_bytes = 12 * mib;
	std::uint64_t gpu_freed = 3;
	std::uint64_t trim_bytes = 200 * mib;
	std::uint64_t trim_ns = 80 * millisecond;
	TrimOutcome outcome = TrimOutcome::done;
	std::function<void()> inside_trim;
	std::vector<std::string> calls;
	std::vector<std::string> lines;
};

[[nodiscard]] bool contains(const std::string &text, const char *part)
{
	return text.find(part) != std::string::npos;
}

void theLevelsOfTheSourceAreTheMostSeriousFlag()
{
	using uml::runtime::pressure_level_from_dispatch;
	require(pressure_level_from_dispatch(0) == PressureLevel::normal);
	require(pressure_level_from_dispatch(DISPATCH_MEMORYPRESSURE_NORMAL) ==
		PressureLevel::normal);
	require(pressure_level_from_dispatch(DISPATCH_MEMORYPRESSURE_WARN) ==
		PressureLevel::warn);
	require(pressure_level_from_dispatch(DISPATCH_MEMORYPRESSURE_CRITICAL) ==
		PressureLevel::critical);
	/* Events the handler had not yet run for arrive together. */
	require(pressure_level_from_dispatch(DISPATCH_MEMORYPRESSURE_NORMAL |
			DISPATCH_MEMORYPRESSURE_WARN) == PressureLevel::warn);
	require(pressure_level_from_dispatch(DISPATCH_MEMORYPRESSURE_WARN |
			DISPATCH_MEMORYPRESSURE_CRITICAL) == PressureLevel::critical);
}

void theNormalLevelDoesNothing()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.calls_taken().empty());
	require(actions.lines_taken().empty(), "no change, no line");

	/* A way back to normal is a change and one line, and still no action. */
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.trims() == 1);
	const auto lines = actions.lines_taken();
	require(lines.size() == 3, "warn change, the response, normal change");
	require(contains(lines[2], "memory pressure normal (was warn)"), lines[2]);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.lines_taken().size() == 3);
}

void aWarningFreesTheGpuCachesThenTrimsOnce()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	require((actions.calls_taken() ==
		std::vector<std::string>{"gpu", "trim"}));
	const auto lines = actions.lines_taken();
	require(lines.size() == 2, "the change and the response");
	require(lines[0] ==
		"uml: memory pressure warn (was normal): footprint 2026 MiB, "
		"16 guest RAM wire failures", lines[0]);
	/* 2026 - 12 graphics - 200 guest = 1814. */
	require(lines[1] ==
		"uml: memory pressure warn: footprint 2026 -> 1814 MiB; released "
		"12 MiB graphics (3 cached allocations), 200 MiB guest RAM (page "
		"cache trim done, 80 ms); 16 guest RAM wire failures", lines[1]);
	const auto counts = response.counts();
	require(counts.responses == 1 && counts.limited == 0 && counts.busy == 0);
}

void aTrimThatCouldNotBeSentOrAnsweredIsSaidSo()
{
	FakeActions actions;
	actions.outcome = TrimOutcome::skipped;
	actions.trim_bytes = 0;
	actions.gpu_freed = 0;
	actions.gpu_bytes = 0;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	auto lines = actions.lines_taken();
	require(contains(lines.back(),
		"released 0 MiB graphics (0 cached allocations), 0 MiB guest RAM "
		"(page cache trim not sent, 80 ms)"), lines.back());

	FakeActions slow;
	slow.outcome = TrimOutcome::unanswered;
	slow.trim_ns = 3000 * millisecond;
	MemoryPressureResponse patient{slow};
	require(patient.on_level(PressureLevel::critical) == Outcome::responded);
	lines = slow.lines_taken();
	require(contains(lines.back(), "page cache trim unanswered, 3000 ms"),
		lines.back());

	/* Memory the guest took meanwhile is no release. */
	FakeActions growing;
	growing.trim_bytes = 0;
	growing.inside_trim = [&growing] {
		growing.current.guest_held += 50 * mib;
	};
	MemoryPressureResponse grown{growing};
	require(grown.on_level(PressureLevel::warn) == Outcome::responded);
	lines = growing.lines_taken();
	require(contains(lines.back(), "0 MiB guest RAM"), lines.back());
}

void aWarningStartsOneResponsePerInterval()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	/* The interval counts from the start: the trim itself took 80 ms. */
	actions.advance(MemoryPressureResponse::warn_interval_ns -
		actions.trim_ns - millisecond);
	require(response.on_level(PressureLevel::warn) == Outcome::limited);
	require(actions.trims() == 1);
	actions.advance(millisecond);
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	require(actions.trims() == 2);
	require(contains(actions.lines_taken().back(),
		"1 events skipped since the last response"),
		actions.lines_taken().back());
	/* And again, a nanosecond short of the interval from that start. */
	actions.advance(MemoryPressureResponse::warn_interval_ns -
		actions.trim_ns - 1);
	require(response.on_level(PressureLevel::warn) == Outcome::limited);
	require(response.counts().limited == 2);
	actions.advance(2);
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	require(actions.trims() == 3);
	require(contains(actions.lines_taken().back(),
		"1 events skipped since the last response"),
		actions.lines_taken().back());
}

void aCriticalLevelAlwaysStartsOne()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(actions.trims() == 2);
	/* Not even the interval stands in its way, a second after the last. */
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(actions.trims() == 3);
	/* The critical response began the interval a warning waits for. */
	actions.advance(10 * second);
	require(response.on_level(PressureLevel::warn) == Outcome::limited);
	require(actions.trims() == 3);
}

void aResponseThatIsRunningIsNotJoined()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	std::vector<Outcome> met;
	actions.inside_trim = [&] {
		/* Another event, from a thread that is not the one that runs. */
		met.push_back(response.on_level(PressureLevel::critical));
		met.push_back(response.on_level(PressureLevel::warn));
	};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	actions.inside_trim = nullptr;
	require(met == (std::vector<Outcome>{Outcome::busy, Outcome::busy}));
	require(actions.trims() == 1);
	require(response.counts().busy == 2);
	/* Once it ended, a critical level is answered, and says what it skipped. */
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(contains(actions.lines_taken().back(),
		"2 events skipped since the last response"),
		actions.lines_taken().back());
}

void anEventOnAnotherThreadMeetsTheRunningTrim()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	std::mutex gate;
	std::condition_variable changed;
	bool inside = false;
	bool release = false;
	actions.inside_trim = [&] {
		std::unique_lock lock{gate};
		inside = true;
		changed.notify_all();
		require(changed.wait_for(lock, std::chrono::seconds{20},
			[&] { return release; }), "the test never released the trim");
	};
	std::thread running{[&] {
		require(response.on_level(PressureLevel::warn) == Outcome::responded);
	}};
	{
		std::unique_lock lock{gate};
		require(changed.wait_for(lock, std::chrono::seconds{20},
			[&] { return inside; }), "the trim never began");
	}
	/* It returns at once and starts nothing. */
	require(response.on_level(PressureLevel::critical) == Outcome::busy);
	require(actions.trims() == 1);
	{
		const std::lock_guard lock{gate};
		release = true;
	}
	changed.notify_all();
	running.join();
	require(actions.trims() == 1);
	actions.inside_trim = nullptr;
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(actions.trims() == 2);
}

void aStormOfLevelChangesCannotFloodTheConsole()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	for (int flip = 0; flip < 10; ++flip) {
		(void)response.on_level(PressureLevel::warn);
		(void)response.on_level(PressureLevel::normal);
	}
	/* Twenty changes in one window: the burst's worth of lines. */
	std::size_t changes = 0;
	for (const auto &line : actions.lines_taken())
		changes += contains(line, "(was ") &&
			!contains(line, "released") ? 1U : 0U;
	require(changes == MemoryPressureResponse::log_burst,
		std::to_string(changes));
	require(response.counts().lines_dropped >= 16);
	/* The next window's first line says how many it left out. */
	actions.advance(MemoryPressureResponse::log_window_ns);
	(void)response.on_level(PressureLevel::warn);
	const auto lines = actions.lines_taken();
	const auto &last_change = *std::find_if(lines.rbegin(), lines.rend(),
		[](const std::string &line) { return contains(line, "(was "); });
	require(contains(last_change, "earlier lines left out"), last_change);
}

void aStormOfCriticalLevelsCannotFloodTheConsole()
{
	FakeActions actions;
	actions.trim_ns = 0;
	MemoryPressureResponse response{actions};
	for (int event = 0; event < 12; ++event)
		require(response.on_level(PressureLevel::critical) ==
			Outcome::responded);
	/* Every one trimmed, a few were written. */
	require(actions.trims() == 12);
	std::size_t responses = 0;
	for (const auto &line : actions.lines_taken())
		responses += contains(line, "released") ? 1U : 0U;
	require(responses == MemoryPressureResponse::log_burst,
		std::to_string(responses));
	actions.advance(MemoryPressureResponse::log_window_ns);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(contains(actions.lines_taken().back(),
		"8 earlier lines left out"), actions.lines_taken().back());
}

/* ---- The system's side, against a source the test raises. ---- */

/* Stderr, captured through a pipe for the case that follows. */
class StderrCapture final {
public:
	StderrCapture()
	{
		int descriptors[2];
		require(::pipe(descriptors) == 0, "pipe");
		read_ = descriptors[0];
		write_ = descriptors[1];
		(void)::fcntl(read_, F_SETFL, O_NONBLOCK);
		saved_ = ::dup(STDERR_FILENO);
		require(saved_ >= 0, "dup");
		require(::dup2(write_, STDERR_FILENO) >= 0, "dup2");
	}

	~StderrCapture()
	{
		restore();
		::close(read_);
		::close(write_);
	}

	StderrCapture(const StderrCapture &) = delete;
	StderrCapture &operator=(const StderrCapture &) = delete;

	void restore()
	{
		if (saved_ >= 0) {
			::dup2(saved_, STDERR_FILENO);
			::close(saved_);
			saved_ = -1;
		}
	}

	/* What has been written since the last call. */
	[[nodiscard]] std::string take()
	{
		char buffer[512];
		for (;;) {
			const auto got = ::read(read_, buffer, sizeof buffer);
			if (got <= 0)
				break;
			text_.append(buffer, static_cast<std::size_t>(got));
		}
		return text_;
	}

private:
	int read_{-1};
	int write_{-1};
	int saved_{-1};
	std::string text_;
};

std::atomic<int> gpu_reclaims{0};
std::atomic<int> guest_trims{0};

std::uint64_t countedGpuReclaim() noexcept
{
	gpu_reclaims.fetch_add(1);
	return 5;
}

uml::runtime::TrimOutcome countedGuestTrim(void *context) noexcept
{
	guest_trims.fetch_add(1);
	*static_cast<int *>(context) += 1;
	return TrimOutcome::done;
}

void theSystemSourceReachesTheGpuAndTheGuest()
{
	gpu_reclaims = 0;
	guest_trims = 0;
	pressure_source.store(nullptr);
	uml::platform::publishGpuCacheReclaimer(&countedGpuReclaim);
	int context = 0;
	StderrCapture capture;
	{
		const auto watch =
			uml::runtime::start_memory_pressure_response(&countedGuestTrim,
				&context);
		require(watch != nullptr);
		const dispatch_source_t source = pressure_source.load();
		require(source != nullptr, "no memory pressure source was made");

		/* Normal: nothing. */
		dispatch_source_merge_data(source, DISPATCH_MEMORYPRESSURE_NORMAL);
		require(waitUntil([&] { return capture.take().empty() == false; },
			std::chrono::milliseconds{200}) == false,
			"the normal level wrote a line");
		require(gpu_reclaims == 0 && guest_trims == 0);

		/* A warning frees the caches and trims, and says so on stderr. */
		dispatch_source_merge_data(source, DISPATCH_MEMORYPRESSURE_WARN);
		require(waitUntil([&] {
			return contains(capture.take(), "memory pressure warn:");
		}, std::chrono::seconds{20}), "the warning wrote no line");
		require(gpu_reclaims == 1 && guest_trims == 1 && context == 1);
		const std::string text = capture.take();
		require(contains(text, "memory pressure warn (was normal): footprint "),
			text);
		/* The figures are the process's own. */
		const auto footprint = text.find("footprint ");
		require(footprint != std::string::npos &&
			std::stoull(text.substr(footprint + 10)) > 0, text);
		require(contains(text, " -> "), text);
		require(contains(text, "(5 cached allocations)"), text);
		require(contains(text, "page cache trim done"), text);
		require(contains(text, "guest RAM wire failures"), text);

		/* A second warning inside the interval does nothing. */
		dispatch_source_merge_data(source, DISPATCH_MEMORYPRESSURE_WARN);
		/* A critical level always acts. */
		dispatch_source_merge_data(source, DISPATCH_MEMORYPRESSURE_CRITICAL);
		require(waitUntil([&] { return guest_trims == 2; },
			std::chrono::seconds{20}), "the critical level did not trim");
		require(gpu_reclaims == 2);
	}
	/* The watch has ended; its source and queue are gone. */
	capture.restore();
	uml::platform::publishGpuCacheReclaimer(nullptr);
}

void aWatchWithNoGpuAndNoGuestStillAnswers()
{
	pressure_source.store(nullptr);
	uml::platform::publishGpuCacheReclaimer(nullptr);
	StderrCapture capture;
	const auto watch =
		uml::runtime::start_memory_pressure_response(nullptr, nullptr);
	require(watch != nullptr);
	dispatch_source_merge_data(pressure_source.load(),
		DISPATCH_MEMORYPRESSURE_CRITICAL);
	require(waitUntil([&] {
		return contains(capture.take(), "page cache trim not sent");
	}, std::chrono::seconds{20}), capture.take());
	require(contains(capture.take(), "(0 cached allocations)"));
}

void aWatchEndsWhileItsResponseIsRunning()
{
	pressure_source.store(nullptr);
	std::mutex gate;
	std::condition_variable changed;
	bool inside = false;
	struct Hold final {
		std::mutex *gate;
		std::condition_variable *changed;
		bool *inside;
	} hold{&gate, &changed, &inside};
	StderrCapture capture;
	auto watch = uml::runtime::start_memory_pressure_response(
		[](void *context) noexcept {
			auto &hold = *static_cast<Hold *>(context);
			{
				const std::lock_guard lock{*hold.gate};
				*hold.inside = true;
			}
			hold.changed->notify_all();
			std::this_thread::sleep_for(std::chrono::milliseconds{300});
			return TrimOutcome::done;
		}, &hold);
	require(watch != nullptr);
	dispatch_source_merge_data(pressure_source.load(),
		DISPATCH_MEMORYPRESSURE_CRITICAL);
	{
		std::unique_lock lock{gate};
		require(changed.wait_for(lock, std::chrono::seconds{20},
			[&] { return inside; }), "the trim never began");
	}
	/* The watch goes with a trim still running: the destructor returns once
	 * it has ended, and what the trim reads (the hold) is alive for it. */
	const auto begun = std::chrono::steady_clock::now();
	watch.reset();
	require(std::chrono::steady_clock::now() - begun >=
		std::chrono::milliseconds{100}, "the watch did not wait");
	capture.restore();
	require(contains(capture.take(), "page cache trim done"));
}

} // namespace

int main()
{
	static constexpr uml::testing::NamedTest tests[] {
		{"the-levels-of-the-source-are-the-most-serious-flag",
			theLevelsOfTheSourceAreTheMostSeriousFlag},
		{"the-normal-level-does-nothing", theNormalLevelDoesNothing},
		{"a-warning-frees-the-gpu-caches-then-trims-once",
			aWarningFreesTheGpuCachesThenTrimsOnce},
		{"a-trim-that-could-not-be-sent-or-answered-is-said-so",
			aTrimThatCouldNotBeSentOrAnsweredIsSaidSo},
		{"a-warning-starts-one-response-per-interval",
			aWarningStartsOneResponsePerInterval},
		{"a-critical-level-always-starts-one",
			aCriticalLevelAlwaysStartsOne},
		{"a-response-that-is-running-is-not-joined",
			aResponseThatIsRunningIsNotJoined},
		{"an-event-on-another-thread-meets-the-running-trim",
			anEventOnAnotherThreadMeetsTheRunningTrim},
		{"a-storm-of-level-changes-cannot-flood-the-console",
			aStormOfLevelChangesCannotFloodTheConsole},
		{"a-storm-of-critical-levels-cannot-flood-the-console",
			aStormOfCriticalLevelsCannotFloodTheConsole},
		{"the-system-source-reaches-the-gpu-and-the-guest",
			theSystemSourceReachesTheGpuAndTheGuest},
		{"a-watch-with-no-gpu-and-no-guest-still-answers",
			aWatchWithNoGpuAndNoGuestStillAnswers},
		{"a-watch-ends-while-its-response-is-running",
			aWatchEndsWhileItsResponseIsRunning},
	};
	return uml::testing::runNamedTests(tests);
}
