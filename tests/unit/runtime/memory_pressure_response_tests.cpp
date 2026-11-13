// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The memory pressure response (runtime/memory_pressure_response): the policy
 * against a clock and actions of the test's own, and the system's side of it,
 * the memory pressure source, against a source this binary substitutes: the
 * binary defines dispatch_source_create, so the response's call binds here and
 * a memory pressure source becomes one the test can raise (a real event needs
 * a privileged tool). The process makes that source once, on the first watch.
 */
#include "memory_pressure_response.hpp"
#include "named_tests.hpp"
#include "platform/gpu_cache_reclaim.hpp"
#include "require.hpp"
#include "wait_until.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <memory>
#include <new>
#include <optional>
#include <sys/sysctl.h>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

/* The memory pressure source the response made, as a source the test merges
 * data into: DISPATCH_SOURCE_TYPE_DATA_OR delivers the merged bits to the
 * handler as the status flags a memory pressure source delivers. */
std::atomic<dispatch_source_t> pressure_source{nullptr};
/* The queue the source's handler runs on: a call on it from the test, once an
 * event was merged, returns after the handler that event started. */
std::atomic<dispatch_queue_t> pressure_queue{nullptr};
/* While set, the process's nothrow allocations fail, as they may when the
 * system is short of memory, which is what a pressure response runs in. */
std::atomic<bool> nothrow_allocation_fails{false};

} // namespace

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
	if (nothrow_allocation_fails.load())
		return nullptr;
	try {
		return ::operator new(size);
	} catch (...) {
		return nullptr;
	}
}

/* The ordinary allocations of the thread that is counting, as the process's
 * own operator new makes them: the count of a thread that does not count stays
 * as it was. */
thread_local bool counting_allocations = false;
thread_local std::size_t allocations_counted = 0;

void *operator new(std::size_t size)
{
	if (counting_allocations)
		++allocations_counted;
	if (void *const memory = std::malloc(size == 0 ? 1 : size))
		return memory;
	throw std::bad_alloc{};
}

void operator delete(void *memory) noexcept
{
	std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept
{
	std::free(memory);
}

namespace {

/* The handler the response gave the pressure source, which runs through a
 * counting wrapper: the ordinary allocations of one event's whole handling, on
 * the thread that handles it. */
std::atomic<dispatch_function_t> pressure_handler{nullptr};
std::atomic<std::size_t> last_event_allocations{0};

void countingPressureHandler(void *context)
{
	allocations_counted = 0;
	counting_allocations = true;
	pressure_handler.load()(context);
	counting_allocations = false;
	last_event_allocations.store(allocations_counted);
}

} // namespace

extern "C" void dispatch_source_set_event_handler_f(
	dispatch_source_t source, dispatch_function_t handler)
{
	using Set = void (*)(dispatch_source_t, dispatch_function_t);
	static const auto real = reinterpret_cast<Set>(
		dlsym(RTLD_NEXT, "dispatch_source_set_event_handler_f"));
	if (handler != nullptr && source == pressure_source.load()) {
		pressure_handler.store(handler);
		real(source, &countingPressureHandler);
		return;
	}
	real(source, handler);
}

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
	pressure_queue.store(queue);
	return source;
}

namespace {

/* What the system's pressure level reads as, for the tests that need it to be
 * so: the value the sysctl returns (1 normal, 2 warn, 4 critical), or the error
 * it fails with. Without one the real sysctl answers. */
struct SystemLevelOverride final {
	bool set{};
	int value{};
	int error{};
};
std::mutex system_level_mutex;
SystemLevelOverride system_level_override;

} // namespace

extern "C" int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
	void *newp, size_t newlen)
{
	using Sysctl = int (*)(const char *, void *, size_t *, void *, size_t);
	static const auto real = reinterpret_cast<Sysctl>(
		dlsym(RTLD_NEXT, "sysctlbyname"));
	if (std::string_view{name} == "kern.memorystatus_vm_pressure_level") {
		const std::lock_guard lock{system_level_mutex};
		if (system_level_override.set) {
			if (system_level_override.error != 0) {
				errno = system_level_override.error;
				return -1;
			}
			if (oldp != nullptr && oldlenp != nullptr &&
			    *oldlenp >= sizeof(int)) {
				*static_cast<int *>(oldp) = system_level_override.value;
				*oldlenp = sizeof(int);
			}
			return 0;
		}
	}
	return real(name, oldp, oldlenp, newp, newlen);
}

namespace {

/* While alive, the system's pressure level reads as `value`, or fails with
 * `error`. */
class SystemLevelIs final {
public:
	explicit SystemLevelIs(int value, int error = 0)
	{
		const std::lock_guard lock{system_level_mutex};
		system_level_override = {true, value, error};
	}

	~SystemLevelIs()
	{
		const std::lock_guard lock{system_level_mutex};
		system_level_override = {};
	}

	SystemLevelIs(const SystemLevelIs &) = delete;
	SystemLevelIs &operator=(const SystemLevelIs &) = delete;
};

using uml::runtime::MemoryPressureResponse;
using uml::runtime::PressureActions;
using uml::runtime::PressureFigures;
using uml::runtime::PressureLevel;
using uml::runtime::PressureReaction;
using uml::runtime::PressureReport;
using uml::runtime::SystemLevel;
using uml::runtime::PressureTiming;
using uml::runtime::TrimKind;
using uml::runtime::TrimOutcome;
using uml::testing::require;
using uml::testing::waitUntil;
using Outcome = MemoryPressureResponse::Outcome;

constexpr std::uint64_t mib = 1024ULL * 1024ULL;
constexpr std::uint64_t second = 1'000'000'000ULL;
constexpr std::uint64_t millisecond = 1'000'000ULL;

/* The memory pressure source the first watch made. */
dispatch_source_t raisable()
{
	const dispatch_source_t source = pressure_source.load();
	require(source != nullptr, "no memory pressure source was made");
	return source;
}

/* Raises `flags` on the source and returns once its handler has run for them:
 * the handler is on a serial queue, so a call on that queue made after the
 * merge waits for it. */
void raiseAndWait(std::uint64_t flags)
{
	dispatch_source_merge_data(raisable(), flags);
	dispatch_sync_f(pressure_queue.load(), nullptr, [](void *) {});
}

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

	/* A trim of all the cache takes `trim_ns` and releases `trim_bytes`; the
	 * gradual trimmer's requests are one short message each, with an
	 * outcome of their own. */
	TrimOutcome trim_page_cache(TrimKind kind) noexcept override
	{
		std::function<void()> inside;
		{
			const std::lock_guard lock{mutex};
			calls.emplace_back(kind == TrimKind::once ? "trim" :
				kind == TrimKind::gradual_on ? "trim-on" : "trim-off");
			inside = inside_trim;
		}
		if (inside)
			inside();
		const std::lock_guard lock{mutex};
		if (kind != TrimKind::once) {
			/* The scripted answers come first, then the standing ones. */
			if (script_next < gradual_script.size())
				return gradual_script[script_next++];
			if (kind == TrimKind::gradual_off && off_outcome)
				return *off_outcome;
			return gradual_outcome;
		}
		now += trim_ns;
		const std::uint64_t released = std::min(trim_bytes, current.guest_held);
		current.guest_held -= released;
		current.footprint -= std::min(released, current.footprint);
		return outcome;
	}

	SystemLevel system_level() noexcept override
	{
		const std::lock_guard lock{mutex};
		return system;
	}

	void log(std::string_view line) noexcept override
	{
		const std::lock_guard lock{mutex};
		lines.emplace_back(line);
	}

	/* What the application heard, and how much the response had done when it
	 * heard it. */
	struct Heard final {
		PressureReport report;
		std::size_t calls_before;
		std::size_t lines_before;
	};

	void report(const PressureReport &heard) noexcept override
	{
		const std::lock_guard lock{mutex};
		reports.push_back({heard, calls.size(), lines.size()});
	}

	[[nodiscard]] std::vector<Heard> reports_taken()
	{
		const std::lock_guard lock{mutex};
		return reports;
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

	/* The trims of all the cache. */
	[[nodiscard]] std::size_t trims()
	{
		return calls_named("trim");
	}

	/* Every request to the guest, of any kind. */
	[[nodiscard]] std::size_t requests()
	{
		return calls_named("trim") + calls_named("trim-on") +
			calls_named("trim-off");
	}

	[[nodiscard]] std::size_t calls_named(const char *name)
	{
		const std::lock_guard lock{mutex};
		std::size_t count = 0;
		for (const auto &call : calls)
			count += call == name ? 1U : 0U;
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
	/* What the trim of all the cache and the gradual trimmer's requests
	 * answer. */
	TrimOutcome outcome = TrimOutcome::done;
	TrimOutcome gradual_outcome = TrimOutcome::done;
	/* The answers of the next requests of the gradual trimmer, in order, of
	 * either kind; and what a request to turn it off answers when it is not
	 * the same as turning it on. */
	std::vector<TrimOutcome> gradual_script;
	std::size_t script_next = 0;
	std::optional<TrimOutcome> off_outcome;
	/* What the system reports of its own level. */
	SystemLevel system{};
	std::function<void()> inside_trim;
	std::vector<std::string> calls;
	std::vector<std::string> lines;
	std::vector<Heard> reports;
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

void theNormalLevelTakesNothing()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.calls_taken().empty());
	require(actions.lines_taken().empty(), "no change, no line");

	/* A way back to normal is a change and one line, and takes nothing: it
	 * ends the gradual trimmer the warning began, once. */
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	require((actions.calls_taken() == std::vector<std::string>{"trim-on"}));
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require((actions.calls_taken() ==
		std::vector<std::string>{"trim-on", "trim-off"}));
	const auto lines = actions.lines_taken();
	require(lines.size() == 3, "warn change, the response, normal change");
	require(contains(lines[2], "memory pressure normal (was warn)"), lines[2]);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.lines_taken().size() == 3);
	require(actions.requests() == 2, "the trimmer was ended twice");
}

void aWarningTurnsTheGradualTrimmerOnAndTakesNothingElse()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	/* Neither the GPU's caches nor a trim of all the cache. */
	require((actions.calls_taken() == std::vector<std::string>{"trim-on"}));
	const auto lines = actions.lines_taken();
	require(lines.size() == 2, "the change and the response");
	require(lines[0] ==
		"uml: memory pressure warn (was normal): footprint 2026 MiB, "
		"16 guest RAM wire failures", lines[0]);
	require(lines[1] ==
		"uml: memory pressure warn: footprint 2026 -> 2026 MiB; guest page "
		"cache trimmer on (done); 16 guest RAM wire failures",
		lines[1]);
	const auto counts = response.counts();
	require(counts.responses == 1 && counts.limited == 0 && counts.busy == 0);
	/* Nothing waited for the guest: the response took no time of the clock. */
	require(actions.now_ns() == 100 * second);
}

void aWarningSaysWhatBecameOfTheGradualTrimmer()
{
	const struct {
		TrimOutcome outcome;
		const char *words;
	} endings[] = {
		{TrimOutcome::unreachable, "trimmer on (no console)"},
		{TrimOutcome::quiesced, "trimmer on (guest quiesced)"},
		{TrimOutcome::refused, "trimmer on (refused)"},
		{TrimOutcome::busy, "trimmer on (busy)"},
		{TrimOutcome::unanswered, "trimmer on (unanswered)"},
		{TrimOutcome::sent, "trimmer on (sent)"},
		{TrimOutcome::done, "trimmer on (done)"},
	};
	for (const auto &ending : endings) {
		FakeActions actions;
		actions.gradual_outcome = ending.outcome;
		MemoryPressureResponse response{actions};
		require(response.on_level(PressureLevel::warn) == Outcome::responded);
		require(contains(actions.lines_taken().back(), ending.words),
			actions.lines_taken().back());
	}
}

void aCriticalLevelFreesTheGpuCachesTrimsOnceAndLeavesTheGradualTrimmerOn()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require((actions.calls_taken() ==
		std::vector<std::string>{"gpu", "trim", "trim-on"}));
	const auto lines = actions.lines_taken();
	require(lines.size() == 2, "the change and the response");
	require(lines[0] ==
		"uml: memory pressure critical (was normal): footprint 2026 MiB, "
		"16 guest RAM wire failures", lines[0]);
	/* 2026 - 12 graphics - 200 guest = 1814. */
	require(lines[1] ==
		"uml: memory pressure critical: footprint 2026 -> 1814 MiB; "
		"released 12 MiB graphics (3 cached allocations), 200 MiB guest RAM "
		"(page cache trim done, 80 ms); 16 guest RAM wire failures",
		lines[1]);

	/* The next one asks the trimmer on again (the guest side answers that it
	 * already is), so that a request that was lost is not owed for ever. */
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(actions.calls_named("gpu") == 2 && actions.trims() == 2);
	require(actions.calls_named("trim-on") == 2);

	/* And the way back to normal ends it. */
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.calls_named("trim-off") == 1);

	/* A critical level whose trim found the guest busy does not turn the
	 * gradual trimmer on before the trim was answered. */
	FakeActions busy;
	busy.outcome = TrimOutcome::busy;
	MemoryPressureResponse waiting{busy};
	require(waiting.on_level(PressureLevel::critical) == Outcome::responded);
	require((busy.calls_taken() == std::vector<std::string>{"gpu", "trim"}));
}

void aTrimThatCouldNotBeSentOrAnsweredIsSaidSo()
{
	/* Each ending has its own word, and only the ones nothing was sent for
	 * say so: a refusal was sent. */
	const struct {
		TrimOutcome outcome;
		const char *words;
	} endings[] = {
		{TrimOutcome::unreachable, "page cache trim no console, 80 ms"},
		{TrimOutcome::quiesced, "page cache trim guest quiesced, 80 ms"},
		{TrimOutcome::refused, "page cache trim refused, 80 ms"},
		{TrimOutcome::busy, "page cache trim busy, 80 ms"},
		{TrimOutcome::unanswered, "page cache trim unanswered, 80 ms"},
		{TrimOutcome::done, "page cache trim done, 80 ms"},
	};
	for (const auto &ending : endings) {
		FakeActions actions;
		actions.outcome = ending.outcome;
		actions.trim_bytes = 0;
		actions.gpu_freed = 0;
		actions.gpu_bytes = 0;
		MemoryPressureResponse response{actions};
		require(response.on_level(PressureLevel::critical) ==
			Outcome::responded);
		const auto lines = actions.lines_taken();
		require(contains(lines.back(),
			"released 0 MiB graphics (0 cached allocations), 0 MiB guest "
			"RAM ("), lines.back());
		require(contains(lines.back(), ending.words), lines.back());
		require(!contains(lines.back(), "not sent"), lines.back());
	}

	FakeActions slow;
	slow.outcome = TrimOutcome::unanswered;
	slow.trim_ns = 3000 * millisecond;
	MemoryPressureResponse patient{slow};
	require(patient.on_level(PressureLevel::critical) == Outcome::responded);
	const auto lines = slow.lines_taken();
	require(contains(lines.back(), "page cache trim unanswered, 3000 ms"),
		lines.back());

	/* Memory the guest took meanwhile is no release. */
	FakeActions growing;
	growing.trim_bytes = 0;
	growing.inside_trim = [&growing] {
		growing.current.guest_held += 50 * mib;
	};
	MemoryPressureResponse grown{growing};
	require(grown.on_level(PressureLevel::critical) == Outcome::responded);
	require(contains(growing.lines_taken().back(), "0 MiB guest RAM"),
		growing.lines_taken().back());
}

void aWarningStartsOneResponsePerInterval()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	/* The interval counts from the start; a warning takes no time. */
	actions.advance(MemoryPressureResponse::warn_interval_ns - millisecond);
	require(response.on_level(PressureLevel::warn) == Outcome::limited);
	require(actions.requests() == 1);
	actions.advance(millisecond);
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	require(actions.requests() == 2);
	require(contains(actions.lines_taken().back(),
		"1 events skipped since the last response"),
		actions.lines_taken().back());
	/* And again, a nanosecond short of the interval from that start. */
	actions.advance(MemoryPressureResponse::warn_interval_ns - 1);
	require(response.on_level(PressureLevel::warn) == Outcome::limited);
	require(response.counts().limited == 2);
	actions.advance(1);
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	require(actions.requests() == 3);
	require(contains(actions.lines_taken().back(),
		"1 events skipped since the last response"),
		actions.lines_taken().back());
	require(actions.trims() == 0 && actions.calls_named("gpu") == 0,
		"a warning took more than the gradual trimmer");
}

void aWarningTheIntervalHoldsBackStillTurnsTheTrimmerOn()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	actions.advance(second);
	/* The pressure eases and comes back inside the interval: the response is
	 * held back, and the trimmer is on again all the same, as it is on exactly
	 * while the last level is not normal. */
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	actions.advance(second);
	require(response.on_level(PressureLevel::warn) == Outcome::limited);
	require((actions.calls_taken() ==
		std::vector<std::string>{"trim-on", "trim-off", "trim-on"}));
	/* Only the request: no response, and none is reported. */
	require(response.counts().responses == 1 && response.counts().limited == 1);
	/* A further withheld warning asks nothing: it is on. */
	actions.advance(second);
	require(response.on_level(PressureLevel::warn) == Outcome::limited);
	require(actions.requests() == 3);
	actions.advance(MemoryPressureResponse::warn_interval_ns);
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	require(actions.calls_named("trim-on") == 3);
}

void aCriticalLevelAlwaysStartsOne()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(actions.trims() == 1);
	/* Not even the interval stands in its way, a second after the last. */
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(actions.trims() == 2);
	/* The critical response began the interval a warning waits for. */
	actions.advance(10 * second);
	require(response.on_level(PressureLevel::warn) == Outcome::limited);
	require(actions.trims() == 2);
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
	require(actions.requests() == 1);
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
	require(actions.requests() == 1);
	{
		const std::lock_guard lock{gate};
		release = true;
	}
	changed.notify_all();
	running.join();
	require(actions.requests() == 1);
	actions.inside_trim = nullptr;
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(actions.trims() == 1);
}

/* ---- A guest trim that met another lifecycle call is tried again. ---- */

void aBusyTrimIsTriedAgain()
{
	FakeActions actions;
	actions.outcome = TrimOutcome::busy;
	actions.trim_bytes = 0;
	const PressureTiming timing;
	MemoryPressureResponse response{actions, timing};
	require(response.retry_delay_ns() == 0, "nothing pending yet");
	require(response.on_retry() == Outcome::nothing);
	require(actions.trims() == 0);

	/* A critical level whose trim found the guest busy keeps one pending. */
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(contains(actions.lines_taken().back(), "page cache trim busy"),
		actions.lines_taken().back());
	require(response.retry_delay_ns() == timing.retry_interval_ns);
	require(actions.trims() == 1);

	/* The guest is free when the retry comes: it sends the trim and the
	 * response's last line says what it got. */
	actions.outcome = TrimOutcome::done;
	actions.trim_bytes = 150 * mib;
	require(response.on_retry() == Outcome::retried);
	require(actions.trims() == 2);
	require(response.retry_delay_ns() == 0, "one pending trim, tried once");
	require(contains(actions.lines_taken().back(),
		"uml: memory pressure critical (retry 1 of 4): footprint "),
		actions.lines_taken().back());
	require(contains(actions.lines_taken().back(),
		"released 150 MiB guest RAM (page cache trim done, 80 ms)"),
		actions.lines_taken().back());
	require(response.on_retry() == Outcome::nothing);
	require(actions.trims() == 2);
	require(response.counts().retries == 1);
	/* A trim that came late leaves the gradual trimmer on, as one that did
	 * not wait does. */
	require((actions.calls_taken() ==
		std::vector<std::string>{"gpu", "trim", "trim", "trim-on"}));
}

void aRetryThatIsStillBusyStopsAfterItsAttempts()
{
	FakeActions actions;
	actions.outcome = TrimOutcome::busy;
	actions.gradual_outcome = TrimOutcome::busy;
	PressureTiming timing;
	timing.retry_attempts = 3;
	MemoryPressureResponse response{actions, timing};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	const auto lines_before = actions.lines_taken().size();
	for (unsigned attempt = 1; attempt <= 3; ++attempt) {
		require(response.retry_delay_ns() == timing.retry_interval_ns,
			std::to_string(attempt));
		require(response.on_retry() == Outcome::retried);
	}
	/* Three tries and no more, one line for the last. */
	require(response.retry_delay_ns() == 0);
	require(response.on_retry() == Outcome::nothing);
	require(actions.requests() == 4);
	const auto lines = actions.lines_taken();
	require(lines.size() == lines_before + 1, "a line for each try");
	require(contains(lines.back(), "(retry 3 of 3)") &&
		contains(lines.back(), "trimmer on (busy)"),
		lines.back());

	/* No retries configured, none pending. */
	FakeActions never;
	never.outcome = TrimOutcome::busy;
	PressureTiming none;
	none.retry_attempts = 0;
	MemoryPressureResponse once{never, none};
	require(once.on_level(PressureLevel::critical) == Outcome::responded);
	require(once.retry_delay_ns() == 0);
}

void aRetryIsNotForAGuestTheHostHasQuiesced()
{
	FakeActions actions;
	actions.outcome = TrimOutcome::busy;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(response.retry_delay_ns() != 0);
	/* The guest was quiesced meanwhile: the trim said so, and that is the end
	 * of it. */
	actions.outcome = TrimOutcome::quiesced;
	require(response.on_retry() == Outcome::retried);
	require(response.retry_delay_ns() == 0);
	require(actions.trims() == 2);
	require(contains(actions.lines_taken().back(),
		"page cache trim guest quiesced"), actions.lines_taken().back());
	require(response.on_retry() == Outcome::nothing);
	require(actions.trims() == 2);

	/* Neither does a trim that was refused or had no console come again. */
	for (const auto ending : {TrimOutcome::refused, TrimOutcome::unreachable,
		     TrimOutcome::unanswered}) {
		FakeActions other;
		other.outcome = ending;
		MemoryPressureResponse once{other};
		require(once.on_level(PressureLevel::critical) == Outcome::responded);
		require(once.retry_delay_ns() == 0);
	}
}

void aRetryDoesNotMoveTheWarnSpacing()
{
	FakeActions actions;
	actions.gradual_outcome = TrimOutcome::busy;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	actions.advance(second);
	actions.gradual_outcome = TrimOutcome::done;
	require(response.on_retry() == Outcome::retried);
	/* The interval still runs from the warning's start, a second before the
	 * retry. */
	actions.advance(MemoryPressureResponse::warn_interval_ns - second -
		millisecond);
	require(response.on_level(PressureLevel::warn) == Outcome::limited);
	actions.advance(millisecond);
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	require(actions.requests() == 3);
}

void aLaterResponseOrTheNormalLevelTakesThePendingTrim()
{
	FakeActions actions;
	actions.outcome = TrimOutcome::busy;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(response.retry_delay_ns() != 0);

	/* A warning the interval limits leaves it. */
	actions.advance(second);
	require(response.on_level(PressureLevel::warn) == Outcome::limited);
	require(response.retry_delay_ns() != 0);

	/* A response of its own tries for itself: that trim replaces the pending
	 * one, and when it finds the guest free nothing is left to retry. */
	actions.outcome = TrimOutcome::done;
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(response.retry_delay_ns() == 0);
	require(response.on_retry() == Outcome::nothing);
	require(actions.trims() == 2);

	/* The way back to normal ends the pressure and the retry with it. */
	actions.outcome = TrimOutcome::busy;
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(response.retry_delay_ns() != 0);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(response.retry_delay_ns() == 0);
	require(response.on_retry() == Outcome::nothing);
	require(actions.trims() == 3);

	/* A response that is busy again starts a fresh set of attempts. */
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(response.on_retry() == Outcome::retried);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	for (unsigned attempt = 0; attempt < 4; ++attempt)
		require(response.on_retry() == Outcome::retried);
	require(response.on_retry() == Outcome::nothing);
}

void theNormalLevelDuringAResponseLeavesNoTrimPending()
{
	FakeActions actions;
	actions.outcome = TrimOutcome::busy;
	MemoryPressureResponse response{actions};
	/* The way back to normal comes from another thread while the response's
	 * trim is in the guest's hands, and the trim then reports busy. */
	actions.inside_trim = [&] {
		require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(response.retry_delay_ns() == 0,
		"a trim was left for a pressure that ended");
	require(response.on_retry() == Outcome::nothing);
	require(actions.trims() == 1);

	/* Without it the same busy trim is kept for a retry. */
	actions.inside_trim = nullptr;
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(response.retry_delay_ns() != 0);
}

void theNormalLevelDuringAWarningEndsTheGradualTrimmerWhenItReturns()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	/* The way back to normal comes from another thread while the warning's
	 * request is in the guest's hands: that request ends it when it returns. */
	actions.inside_trim = [&] {
		actions.inside_trim = nullptr;
		require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	require((actions.calls_taken() ==
		std::vector<std::string>{"trim-on", "trim-off"}));
	require(response.retry_delay_ns() == 0);
}

void aBusyGuestKeepsTheEndOfTheGradualTrimmerPending()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	actions.gradual_outcome = TrimOutcome::busy;
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(response.retry_delay_ns() != 0);
	/* The guest is free when the retry comes. */
	actions.gradual_outcome = TrimOutcome::done;
	require(response.on_retry() == Outcome::retried);
	require(response.retry_delay_ns() == 0);
	require(contains(actions.lines_taken().back(),
		"uml: memory pressure normal (retry 1 of 4): guest page cache "
		"trimmer off (done); 16 guest RAM wire failures"),
		actions.lines_taken().back());
	require((actions.calls_taken() == std::vector<std::string>{
		"trim-on", "trim-off", "trim-off"}));
	/* Ended: the normal level asks no more. */
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.requests() == 3);

	/* A pressure that came back first takes the request: it asks for the
	 * trimmer on itself. */
	FakeActions other;
	MemoryPressureResponse again{other};
	require(again.on_level(PressureLevel::warn) == Outcome::responded);
	other.gradual_outcome = TrimOutcome::busy;
	require(again.on_level(PressureLevel::normal) == Outcome::nothing);
	require(again.retry_delay_ns() != 0);
	other.gradual_outcome = TrimOutcome::done;
	other.advance(MemoryPressureResponse::warn_interval_ns);
	require(again.on_level(PressureLevel::warn) == Outcome::responded);
	require(again.retry_delay_ns() == 0);
	require(again.on_retry() == Outcome::nothing);
	require((other.calls_taken() == std::vector<std::string>{
		"trim-on", "trim-off", "trim-on"}));
}

void aRetryIsNotJoinedToAResponseThatIsRunning()
{
	FakeActions actions;
	actions.outcome = TrimOutcome::busy;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	std::vector<Outcome> met;
	actions.inside_trim = [&] {
		met.push_back(response.on_retry());
		met.push_back(response.on_level(PressureLevel::critical));
	};
	require(response.on_retry() == Outcome::retried);
	actions.inside_trim = nullptr;
	require(met == (std::vector<Outcome>{Outcome::busy, Outcome::busy}));
	require(actions.trims() == 2);
}

[[nodiscard]] std::vector<PressureReport> reportsOf(FakeActions &actions);

/* ---- The gradual trimmer follows the level; its requests are retried
 * within a finite allowance. ---- */

void aWarningOrACriticalLevelCancelsAPendingEndOfTheTrimmer()
{
	FakeActions actions;
	const PressureTiming timing;
	MemoryPressureResponse response{actions, timing};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	actions.gradual_outcome = TrimOutcome::busy;
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(response.retry_delay_ns() == timing.retry_interval_ns,
		"the end of the trimmer is pending");
	/* The pressure is back inside the interval: the trimmer is wanted on, and
	 * the end that was never sent is no longer wanted. */
	actions.advance(second);
	require(response.on_level(PressureLevel::warn) == Outcome::limited);
	require(response.retry_delay_ns() == 0, "the end outlived the pressure");
	require(response.on_retry() == Outcome::nothing);
	require((actions.calls_taken() ==
		std::vector<std::string>{"trim-on", "trim-off"}));

	/* A critical level takes it too, and asks for the trimmer on itself. */
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(response.retry_delay_ns() != 0);
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(actions.calls_named("trim-off") == 2);
	actions.gradual_outcome = TrimOutcome::done;
	/* What is pending now is the trimmer's request to be on, not its end. */
	require(response.retry_delay_ns() != 0);
	require(response.on_retry() == Outcome::retried);
	require(actions.calls_named("trim-off") == 2);
	require(actions.calls_taken().back() == "trim-on");
}

void aBusyEndOfTheTrimmerIsTriedAgainAtMostFourTimes()
{
	FakeActions actions;
	const PressureTiming timing;
	MemoryPressureResponse response{actions, timing};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	actions.gradual_outcome = TrimOutcome::busy;
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.calls_named("trim-off") == 1);
	for (unsigned retry = 1; retry <= 4; ++retry) {
		require(response.retry_delay_ns() == timing.retry_interval_ns,
			std::to_string(retry));
		require(response.on_retry() == Outcome::retried);
		/* One request for each call of the timer, and no second set of
		 * attempts behind it. */
		require(actions.calls_named("trim-off") == 1U + retry,
			std::to_string(retry));
	}
	require(response.retry_delay_ns() == 0);
	require(response.on_retry() == Outcome::nothing);
	require(actions.calls_named("trim-off") == 5);
	require(response.counts().retries == 4);
	require(contains(actions.lines_taken().back(), "(retry 4 of 4)") &&
		contains(actions.lines_taken().back(), "trimmer off (busy)"),
		actions.lines_taken().back());
}

void aCriticalRetryThatTrimmedKeepsABusyTrimmerRequestWithoutTrimmingAgain()
{
	FakeActions actions;
	actions.outcome = TrimOutcome::busy;
	actions.gradual_outcome = TrimOutcome::busy;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require((actions.calls_taken() == std::vector<std::string>{"gpu", "trim"}));
	/* The retry's trim goes through; the trimmer's request behind it finds the
	 * guest busy and is kept with attempts of its own. */
	actions.outcome = TrimOutcome::done;
	require(response.on_retry() == Outcome::retried);
	require(response.retry_delay_ns() != 0, "the trimmer's request was dropped");
	require(contains(actions.lines_taken().back(), "page cache trim done"),
		actions.lines_taken().back());
	for (unsigned attempt = 1; attempt <= 3; ++attempt)
		require(response.on_retry() == Outcome::retried);
	/* The guest is free for the last of them. */
	actions.gradual_outcome = TrimOutcome::done;
	require(response.on_retry() == Outcome::retried);
	require(response.retry_delay_ns() == 0);
	require(response.on_retry() == Outcome::nothing);
	require(actions.trims() == 2, "the trim of all the cache was repeated");
	require(actions.calls_named("trim-on") == 5);

	/* A trimmer that stays busy is asked four times more and no more. */
	FakeActions busy;
	busy.outcome = TrimOutcome::busy;
	busy.gradual_outcome = TrimOutcome::busy;
	MemoryPressureResponse waiting{busy};
	require(waiting.on_level(PressureLevel::critical) == Outcome::responded);
	busy.outcome = TrimOutcome::done;
	for (unsigned attempt = 0; attempt <= 4; ++attempt)
		require(waiting.on_retry() == Outcome::retried);
	require(waiting.retry_delay_ns() == 0);
	require(waiting.on_retry() == Outcome::nothing);
	require(busy.trims() == 2 && busy.calls_named("trim-on") == 5);
}

void aGradualRequestThatCouldNotBeSentIsTriedAgainWithinItsAllowance()
{
	FakeActions actions;
	actions.gradual_outcome = TrimOutcome::unreachable;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	require(response.retry_delay_ns() != 0);
	for (unsigned retry = 1; retry <= 4; ++retry)
		require(response.on_retry() == Outcome::retried);
	require(response.retry_delay_ns() == 0);
	require(response.on_retry() == Outcome::nothing);
	require(actions.calls_named("trim-on") == 5);

	/* The trim of all the cache that cannot be sent is not tried again. */
	FakeActions once;
	once.outcome = TrimOutcome::unreachable;
	MemoryPressureResponse single{once};
	require(single.on_level(PressureLevel::critical) == Outcome::responded);
	require(single.retry_delay_ns() == 0);
}

void anEndOfTheTrimmerThatCouldNotBeSentStaysOwedAndARefusedOneDoesNot()
{
	FakeActions actions;
	actions.off_outcome = TrimOutcome::unreachable;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	for (unsigned retry = 1; retry <= 4; ++retry)
		require(response.on_retry() == Outcome::retried);
	require(response.on_retry() == Outcome::nothing);
	require(actions.calls_named("trim-off") == 5);
	/* The guest was never told: the next normal level asks again. */
	actions.off_outcome = TrimOutcome::done;
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.calls_named("trim-off") == 6);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.calls_named("trim-off") == 6, "asked again after it was taken");

	/* A kernel without the command has nothing to turn off. */
	FakeActions refused;
	refused.off_outcome = TrimOutcome::refused;
	MemoryPressureResponse without{refused};
	require(without.on_level(PressureLevel::warn) == Outcome::responded);
	refused.advance(second);
	require(without.on_level(PressureLevel::normal) == Outcome::nothing);
	require(without.retry_delay_ns() == 0);
	require(without.on_level(PressureLevel::normal) == Outcome::nothing);
	require(refused.calls_named("trim-off") == 1);
}

void aWarningThatMeetsTheRetriedEndOfTheTrimmerTurnsItOnAgain()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	actions.gradual_outcome = TrimOutcome::busy;
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	actions.gradual_outcome = TrimOutcome::done;
	Outcome met = Outcome::nothing;
	/* The pressure comes back while the retried end is in the guest's hands:
	 * the event finds a retry running and starts nothing. */
	actions.inside_trim = [&] {
		actions.inside_trim = nullptr;
		met = response.on_level(PressureLevel::warn);
	};
	require(response.on_retry() == Outcome::retried);
	require(met == Outcome::busy);
	require((actions.calls_taken() == std::vector<std::string>{
		"trim-on", "trim-off", "trim-off", "trim-on"}));
	require(response.retry_delay_ns() == 0);
}

/* ---- What the host reads of the system's level. ---- */

void aMergedDeliveryIsActedOnAtTheLevelTheSystemReports()
{
	const std::uint64_t normal = DISPATCH_MEMORYPRESSURE_NORMAL;
	const std::uint64_t warn = DISPATCH_MEMORYPRESSURE_WARN;
	const std::uint64_t critical = DISPATCH_MEMORYPRESSURE_CRITICAL;
	FakeActions actions;
	actions.system = {true, PressureLevel::warn, 0};
	MemoryPressureResponse response{actions};
	/* The critical level merged with the normal one, and the system is at
	 * warn now: the warning's response, not the critical level's. */
	require(response.on_delivery(normal | critical) == Outcome::responded);
	require((actions.calls_taken() == std::vector<std::string>{"trim-on"}));
	require(contains(actions.lines_taken().front(),
		"uml: memory pressure warn (was normal): footprint 2026 MiB, 16 guest "
		"RAM wire failures; system level warn"), actions.lines_taken().front());
	/* A delivery with no normal flag is the level it says, whatever the system
	 * says: the system is only asked when the order of events is lost. */
	actions.system = {true, PressureLevel::normal, 0};
	actions.advance(second);
	require(response.on_delivery(critical) == Outcome::responded);
	require(actions.trims() == 1 && actions.calls_named("gpu") == 1);
	/* The pressure eased behind a critical level: the system is at normal, and
	 * the trimmer is ended. */
	actions.advance(second);
	require(response.on_delivery(normal | critical) == Outcome::nothing);
	require(actions.calls_taken().back() == "trim-off");
	require(actions.trims() == 1, "a merged delivery was acted on as critical");
	require(reportsOf(actions).back().level == PressureLevel::normal);
	/* A system that cannot be read leaves the most serious flag. */
	actions.system = {false, PressureLevel::normal, 1};
	actions.advance(MemoryPressureResponse::warn_interval_ns);
	require(response.on_delivery(normal | warn) == Outcome::responded);
	require(actions.calls_taken().back() == "trim-on");
	actions.advance(MemoryPressureResponse::warn_interval_ns);
	require(response.on_delivery(normal | critical) == Outcome::responded);
	require(actions.trims() == 2 && actions.calls_named("gpu") == 2);
	/* The normal level alone is normal, and the system is not asked. */
	actions.system = {true, PressureLevel::critical, 0};
	require(response.on_delivery(normal) == Outcome::nothing);
	require(reportsOf(actions).back().level == PressureLevel::normal);
}

void theWatchSaysWhatItReadsOfTheSystemLevel()
{
	FakeActions actions;
	actions.system = {true, PressureLevel::critical, 0};
	MemoryPressureResponse response{actions};
	response.announce_start();
	require((actions.lines_taken() == std::vector<std::string>{
		"uml: memory pressure watch started: system level critical"}));
	FakeActions unreadable;
	unreadable.system = {false, PressureLevel::normal, 1};
	MemoryPressureResponse other{unreadable};
	other.announce_start();
	(void)other.on_level(PressureLevel::warn);
	require(unreadable.lines_taken().front() ==
		"uml: memory pressure watch started: system level unreadable "
		"(error 1)", unreadable.lines_taken().front());
	/* The level line says nothing of a level that cannot be read. */
	require(!contains(unreadable.lines_taken()[1], "system level"),
		unreadable.lines_taken()[1]);
}

/* A clock that holds its first reader, as a thread the scheduler stops between
 * reading the time and using it would. */
class HeldClock final : public PressureActions {
public:
	std::uint64_t now_ns() noexcept override
	{
		std::unique_lock lock{mutex};
		const unsigned reader = ++readers;
		++inside;
		widest = std::max(widest, inside);
		changed.notify_all();
		if (reader == 1U)
			(void)changed.wait_for(lock, std::chrono::seconds{20},
				[&] { return released; });
		--inside;
		return now += reader == 1U ? 0U : 2 * second;
	}

	PressureFigures figures() noexcept override { return {}; }
	std::uint64_t reclaim_gpu() noexcept override { return 0; }
	TrimOutcome trim_page_cache(TrimKind) noexcept override
	{
		trims.fetch_add(1);
		return TrimOutcome::done;
	}
	void log(std::string_view) noexcept override {}
	void report(const PressureReport &) noexcept override {}

	std::mutex mutex;
	std::condition_variable changed;
	std::uint64_t now = 100 * second;
	unsigned readers = 0;
	unsigned inside = 0;
	unsigned widest = 0;
	bool released = false;
	std::atomic<unsigned> trims{0};
};

void theClockIsReadInTheOrderOfTheMutex()
{
	HeldClock clock;
	MemoryPressureResponse response{clock};
	std::thread first{[&] {
		require(response.on_level(PressureLevel::warn) == Outcome::responded);
	}};
	{
		std::unique_lock lock{clock.mutex};
		require(clock.changed.wait_for(lock, std::chrono::seconds{20},
			[&] { return clock.inside == 1U; }), "the first call never read");
	}
	/* A second event meets the first one's reading of the clock: it waits for
	 * the policy's mutex instead of reading an older or newer time, and finds
	 * the first one's response started when it gets the mutex. */
	Outcome second_outcome = Outcome::nothing;
	std::thread second_call{[&] {
		second_outcome = response.on_level(PressureLevel::warn);
	}};
	require(!waitUntil([&] {
		const std::lock_guard lock{clock.mutex};
		return clock.readers > 1U;
	}, std::chrono::milliseconds{300}),
		"a second thread read the clock while the first held the mutex");
	{
		const std::lock_guard lock{clock.mutex};
		clock.released = true;
	}
	clock.changed.notify_all();
	first.join();
	second_call.join();
	require(clock.widest == 1U, "two threads read the clock at once");
	/* Limited, or busy when it got the mutex while the response ran: the
	 * second never starts one. */
	require(second_outcome == Outcome::limited ||
		second_outcome == Outcome::busy);
	require(clock.trims == 1);
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

/* ---- The application hears each event, before the response's work. ---- */

[[nodiscard]] std::vector<PressureReport> reportsOf(FakeActions &actions)
{
	std::vector<PressureReport> reports;
	for (const auto &entry : actions.reports_taken())
		reports.push_back(entry.report);
	return reports;
}

[[nodiscard]] bool sameReports(const std::vector<PressureReport> &got,
	const std::vector<PressureReport> &wanted)
{
	if (got.size() != wanted.size())
		return false;
	for (std::size_t index = 0; index < got.size(); ++index) {
		if (got[index].level != wanted[index].level ||
		    got[index].reaction != wanted[index].reaction)
			return false;
	}
	return true;
}

void everyLevelIsReportedTheNormalLevelToo()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	/* The way back is the report that ends the pressure, with nothing done
	 * for it; a normal level that changes nothing is reported as well, and
	 * the application keeps what it needs from it. */
	require(sameReports(reportsOf(actions), {
		{PressureLevel::normal, PressureReaction::none},
		{PressureLevel::warn, PressureReaction::responding},
		{PressureLevel::critical, PressureReaction::responding},
		{PressureLevel::normal, PressureReaction::none},
	}));
}

void aReportComesBeforeTheWorkOfTheResponse()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	require((actions.calls_taken() == std::vector<std::string>{"trim-on"}));
	const auto reports = actions.reports_taken();
	require(reports.size() == 1);
	/* Nothing was freed, nothing was sent to the guest and no line was
	 * written when the application heard it: a trim can take seconds. */
	require(reports[0].calls_before == 0, "the report waited for the work");
	require(reports[0].lines_before == 0, "the report waited for a line");
	require(reports[0].report.level == PressureLevel::warn);
	require(reports[0].report.reaction == PressureReaction::responding);

	/* The same for the critical level, whose work is the larger. */
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	const auto both = actions.reports_taken();
	require(both.size() == 2);
	require(both[1].calls_before == 1, "the report waited for the work");
	require(both[1].lines_before == both[0].lines_before + 2,
		"the report waited for a line");
	require(both[1].report.level == PressureLevel::critical);
	require(both[1].report.reaction == PressureReaction::responding);
	require((actions.calls_taken() ==
		std::vector<std::string>{"trim-on", "gpu", "trim", "trim-on"}));
}

void aWarningInsideTheIntervalIsReportedAsWithheld()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	actions.advance(second);
	require(response.on_level(PressureLevel::warn) == Outcome::limited);
	/* A critical level is never withheld. */
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	actions.advance(MemoryPressureResponse::warn_interval_ns);
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	require(sameReports(reportsOf(actions), {
		{PressureLevel::warn, PressureReaction::responding},
		{PressureLevel::warn, PressureReaction::withheld},
		{PressureLevel::critical, PressureReaction::responding},
		{PressureLevel::warn, PressureReaction::responding},
	}));
	/* The warnings' two requests, and the critical level's trim and the
	 * trimmer: a withheld warning that finds the trimmer on asks nothing. */
	require(actions.trims() == 1 && actions.requests() == 4,
		"a withheld warning asked the guest");
}

void aLevelMetWhileTheResponseRunsIsReportedAsUnderway()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	std::vector<Outcome> met;
	std::vector<PressureReport> during;
	actions.inside_trim = [&] {
		/* The policy is concurrent: a level met on another thread while the
		 * trim runs is reported as underway at once. (The system delivers its
		 * events one at a time on one queue, so this is the policy's path and
		 * not what the system's source produces: see
		 * aLevelRaisedWhileATrimRunsIsReportedWithItsNotification.) */
		met.push_back(response.on_level(PressureLevel::critical));
		during = reportsOf(actions);
	};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	actions.inside_trim = nullptr;
	require(met == (std::vector<Outcome>{Outcome::busy}));
	require(sameReports(during, {
		{PressureLevel::warn, PressureReaction::responding},
		{PressureLevel::critical, PressureReaction::underway},
	}));
	require(actions.requests() == 1);
}

void aRetryReportsNothing()
{
	FakeActions actions;
	actions.gradual_outcome = TrimOutcome::busy;
	PressureTiming timing;
	timing.retry_attempts = 2;
	MemoryPressureResponse response{actions, timing};
	require(response.on_level(PressureLevel::warn) == Outcome::responded);
	require(response.on_retry() == Outcome::retried);
	require(response.on_retry() == Outcome::retried);
	require(actions.requests() == 3);
	require(sameReports(reportsOf(actions), {
		{PressureLevel::warn, PressureReaction::responding},
	}), "a retry is the same pressure, heard once");
}

void theNamesOfTheReactionsAreTheLinesWords()
{
	using uml::runtime::pressure_reaction_name;
	require(pressure_reaction_name(PressureReaction::none) == "none");
	require(pressure_reaction_name(PressureReaction::responding) == "responding");
	require(pressure_reaction_name(PressureReaction::withheld) == "withheld");
	require(pressure_reaction_name(PressureReaction::underway) == "underway");
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
std::atomic<int> gradual_ons{0};
std::atomic<int> gradual_offs{0};

std::uint64_t countedGpuReclaim() noexcept
{
	gpu_reclaims.fetch_add(1);
	return 5;
}

uml::runtime::TrimOutcome countedGuestTrim(void *context,
	TrimKind kind) noexcept
{
	switch (kind) {
	case TrimKind::once:
		guest_trims.fetch_add(1);
		*static_cast<int *>(context) += 1;
		break;
	case TrimKind::gradual_on:
		gradual_ons.fetch_add(1);
		break;
	case TrimKind::gradual_off:
		gradual_offs.fetch_add(1);
		break;
	}
	return TrimOutcome::done;
}

void theSystemSourceReachesTheGpuAndTheGuest()
{
	gpu_reclaims = 0;
	guest_trims = 0;
	gradual_ons = 0;
	gradual_offs = 0;
	uml::platform::publishGpuCacheReclaimer(&countedGpuReclaim);
	int context = 0;
	StderrCapture capture;
	{
		const auto watch =
			uml::runtime::start_memory_pressure_response(&countedGuestTrim,
				&context);
		require(watch != nullptr);
		/* The watch says at its start what the host reads of the system's level,
		 * and nothing else. */
		const std::string started = capture.take();
		require(contains(started, "uml: memory pressure watch started: system "
			"level "), started);

		/* Normal: nothing. */
		raiseAndWait(DISPATCH_MEMORYPRESSURE_NORMAL);
		require(capture.take() == started, capture.take());
		require(gpu_reclaims == 0 && guest_trims == 0 && gradual_ons == 0 &&
			gradual_offs == 0);

		/* A warning turns the gradual trimmer on, takes nothing else, and
		 * says so on stderr. */
		raiseAndWait(DISPATCH_MEMORYPRESSURE_WARN);
		require(gpu_reclaims == 0 && guest_trims == 0 && context == 0,
			"a warning took the GPU's caches or trimmed all the cache");
		require(gradual_ons == 1);
		const std::string text = capture.take();
		require(contains(text, "memory pressure warn (was normal): footprint "),
			text);
		require(contains(text, "memory pressure warn: footprint "), text);
		/* The figures are the process's own. */
		const auto footprint = text.find("footprint ");
		require(footprint != std::string::npos &&
			std::stoull(text.substr(footprint + 10)) > 0, text);
		require(contains(text, " -> "), text);
		require(contains(text, "guest page cache trimmer on (done)"), text);
		require(!contains(text, "cached allocations"), text);
		require(contains(text, "guest RAM wire failures"), text);

		/* A second warning inside the interval does nothing. */
		raiseAndWait(DISPATCH_MEMORYPRESSURE_WARN);
		require(gpu_reclaims == 0 && gradual_ons == 1,
			"a second warning inside the interval acted");

		/* A critical level always acts: the GPU's caches, one trim of all
		 * the cache, and the gradual trimmer, asked for again. */
		raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
		require(gpu_reclaims == 1 && guest_trims == 1 && context == 1,
			"the critical level did not act");
		require(gradual_ons == 2);
		const std::string critical = capture.take();
		require(contains(critical, "(5 cached allocations)"), critical);
		require(contains(critical, "page cache trim done"), critical);

		/* And the way back to normal ends the gradual trimmer. */
		raiseAndWait(DISPATCH_MEMORYPRESSURE_NORMAL);
		require(gradual_offs == 1);
	}
	/* The watch has ended; the source stays with the process and answers
	 * nothing. */
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	require(gpu_reclaims == 1 && guest_trims == 1 && gradual_ons == 2 &&
		gradual_offs == 1, "a source with no watch acted");
	capture.restore();
	uml::platform::publishGpuCacheReclaimer(nullptr);
}

void aMergedDeliveryFromTheSystemSourceIsActedOnAtTheSystemsLevel()
{
	gpu_reclaims = 0;
	guest_trims = 0;
	gradual_ons = 0;
	gradual_offs = 0;
	uml::platform::publishGpuCacheReclaimer(&countedGpuReclaim);
	int context = 0;
	StderrCapture capture;
	{
		/* The critical level's notification came merged with the normal one
		 * because the pressure eased while it queued: the system is at
		 * normal. */
		const SystemLevelIs system{1};
		const auto watch = uml::runtime::start_memory_pressure_response(
			&countedGuestTrim, &context);
		require(watch != nullptr);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_WARN);
		require(gradual_ons == 1);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_NORMAL |
			DISPATCH_MEMORYPRESSURE_CRITICAL);
		require(gradual_offs == 1 && gpu_reclaims == 0 && guest_trims == 0,
			"the merged delivery was acted on as critical");
		const std::string text = capture.take();
		require(contains(text, "watch started: system level normal"), text);
		require(contains(text, "memory pressure normal (was warn): footprint "),
			text);
		require(contains(text, "; system level normal"), text);
	}
	{
		/* Merged the other way round: the system is at critical. */
		const SystemLevelIs system{4};
		const auto watch = uml::runtime::start_memory_pressure_response(
			&countedGuestTrim, &context);
		require(watch != nullptr);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_NORMAL |
			DISPATCH_MEMORYPRESSURE_WARN);
		require(gpu_reclaims == 1 && guest_trims == 1,
			"the merged delivery was acted on as a warning");
		require(contains(capture.take(), "; system level critical"),
			capture.take());
	}
	{
		/* A system the sandbox does not let the process ask: the delivery's
		 * most serious flag. */
		const SystemLevelIs system{0, EPERM};
		const auto watch = uml::runtime::start_memory_pressure_response(
			&countedGuestTrim, &context);
		require(watch != nullptr);
		const int before = gradual_ons.load();
		raiseAndWait(DISPATCH_MEMORYPRESSURE_NORMAL |
			DISPATCH_MEMORYPRESSURE_WARN);
		require(gradual_ons == before + 1 && gpu_reclaims == 1,
			"the unreadable system changed the level");
		raiseAndWait(DISPATCH_MEMORYPRESSURE_NORMAL |
			DISPATCH_MEMORYPRESSURE_CRITICAL);
		require(gpu_reclaims == 2 && guest_trims == 2);
		const std::string text = capture.take();
		require(contains(text, "watch started: system level unreadable "
			"(error 1)"), text);
	}
	capture.restore();
	uml::platform::publishGpuCacheReclaimer(nullptr);
}

void aWatchWithNoGpuAndNoGuestStillAnswers()
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	StderrCapture capture;
	const auto watch =
		uml::runtime::start_memory_pressure_response(nullptr, nullptr);
	require(watch != nullptr);
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	const std::string text = capture.take();
	require(contains(text, "page cache trim no console"), text);
	require(contains(text, "(0 cached allocations)"), text);
}

void aWatchEndsWhileItsResponseIsRunning()
{
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
		[](void *context, TrimKind kind) noexcept {
			if (kind != TrimKind::once)
				return TrimOutcome::done;
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
	dispatch_source_merge_data(raisable(), DISPATCH_MEMORYPRESSURE_CRITICAL);
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

/* What a guest trim of all the cache that is busy a few times counts; the
 * gradual trimmer's requests are always taken and not counted. */
struct BusyThenFree final {
	std::atomic<int> calls{0};
	int busy_calls;
};

TrimOutcome busyThenFree(void *context, TrimKind kind) noexcept
{
	if (kind != TrimKind::once)
		return TrimOutcome::done;
	auto &trim = *static_cast<BusyThenFree *>(context);
	return trim.calls.fetch_add(1) < trim.busy_calls ? TrimOutcome::busy
							: TrimOutcome::done;
}

void aBusyGuestTrimIsTriedAgainByTheWatch()
{
	BusyThenFree trim;
	trim.busy_calls = 2;
	StderrCapture capture;
	PressureTiming timing;
	timing.retry_interval_ns = 20 * millisecond;
	{
		const auto watch = uml::runtime::start_memory_pressure_response(
			&busyThenFree, &trim, timing);
		require(watch != nullptr);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
		/* The response's own try and two retries found the guest busy or
		 * free; nothing comes after the one that was done. */
		require(waitUntil([&] { return trim.calls == 3; },
			std::chrono::seconds{20}), "the busy trim was not retried");
		require(waitUntil([&] {
			return contains(capture.take(), "(retry 2 of 4)");
		}, std::chrono::seconds{20}), capture.take());
		require(!waitUntil([&] { return trim.calls > 3; },
			std::chrono::milliseconds{300}), "a trim that was done came again");
	}
	capture.restore();
	const std::string text = capture.take();
	require(contains(text, "page cache trim busy"), text);
	require(contains(text, "(retry 2 of 4)") &&
		contains(text, "page cache trim done"), text);
	/* The first retry found it busy still and wrote nothing. */
	require(!contains(text, "(retry 1 of 4)"), text);
}

/* The guest is busy at the first try, and until the retry the process cannot
 * allocate. */
TrimOutcome busyWhileAllocationFails(void *context, TrimKind kind) noexcept
{
	if (kind != TrimKind::once)
		return TrimOutcome::done;
	auto &calls = *static_cast<std::atomic<int> *>(context);
	if (calls.fetch_add(1) == 0) {
		nothrow_allocation_fails.store(true);
		return TrimOutcome::busy;
	}
	nothrow_allocation_fails.store(false);
	return TrimOutcome::done;
}

void aRetryNeedsNoAllocationToBeArmed()
{
	std::atomic<int> calls{0};
	StderrCapture capture;
	PressureTiming timing;
	timing.retry_interval_ns = 20 * millisecond;
	{
		const auto watch = uml::runtime::start_memory_pressure_response(
			&busyWhileAllocationFails, &calls, timing);
		require(watch != nullptr);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
		const bool retried = waitUntil([&] { return calls == 2; },
			std::chrono::seconds{20});
		nothrow_allocation_fails.store(false);
		require(retried, "a retry was lost for want of memory to arm it");
	}
	capture.restore();
	require(contains(capture.take(), "(retry 1 of 4)"));
}

void aPendingRetryEndsWithTheWatch()
{
	BusyThenFree trim;
	trim.busy_calls = 1000;
	BusyThenFree later;
	later.busy_calls = 0;
	StderrCapture capture;
	PressureTiming timing;
	timing.retry_interval_ns = 150 * millisecond;
	{
		auto watch = uml::runtime::start_memory_pressure_response(
			&busyThenFree, &trim, timing);
		require(watch != nullptr);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
		require(trim.calls == 1);
		/* The watch ends with a retry pending, and another watch of the same
		 * process begins before the timer would have fired. */
		watch.reset();
		const auto next = uml::runtime::start_memory_pressure_response(
			&busyThenFree, &later, timing);
		require(next != nullptr);
		std::this_thread::sleep_for(std::chrono::milliseconds{500});
		require(trim.calls == 1, "a retry came after its watch ended");
		require(later.calls == 0, "a retry of an earlier watch reached the next");
	}
	/* The process goes on, and a watch with no retry pending sees none. */
	std::this_thread::sleep_for(std::chrono::milliseconds{200});
	require(trim.calls == 1 && later.calls == 0);
}

/* ---- The observer of the reports, against the system's source. ---- */

/* What an observer heard, in order, and the guest trims counted by the time it
 * heard each. */
struct Heard final {
	std::mutex mutex;
	std::vector<PressureReport> reports;
	std::vector<int> trims_before;
	std::atomic<int> *trims{};
};

void observeInto(void *context, const PressureReport &report) noexcept
{
	auto &heard = *static_cast<Heard *>(context);
	const std::lock_guard lock{heard.mutex};
	heard.reports.push_back(report);
	heard.trims_before.push_back(heard.trims == nullptr ? 0 : heard.trims->load());
}

std::atomic<int> observed_trims{0};

TrimOutcome countObservedTrim(void *, TrimKind) noexcept
{
	observed_trims.fetch_add(1);
	return TrimOutcome::done;
}

void theObserverHearsTheLevelsTheSystemRaises()
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	observed_trims = 0;
	Heard heard;
	heard.trims = &observed_trims;
	StderrCapture capture;
	uml::runtime::set_pressure_observer(&observeInto, &heard);
	{
		const auto watch = uml::runtime::start_memory_pressure_response(
			&countObservedTrim, nullptr);
		require(watch != nullptr);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_WARN);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_WARN);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_NORMAL);
	}
	/* A watch that ended drops what the source delivers, and so reports none. */
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	uml::runtime::set_pressure_observer(nullptr, nullptr);
	const std::lock_guard lock{heard.mutex};
	require(sameReports(heard.reports, {
		{PressureLevel::warn, PressureReaction::responding},
		{PressureLevel::warn, PressureReaction::withheld},
		{PressureLevel::critical, PressureReaction::responding},
		{PressureLevel::normal, PressureReaction::none},
	}));
	/* Each was heard before its own request, and a withheld warning that found
	 * the trimmer on had none: the warning's trimmer on, the critical level's
	 * one trim and trimmer on, the normal level's trimmer off. */
	require((heard.trims_before == std::vector<int>{0, 1, 1, 3}),
		"a report followed the request it announced");
	require(observed_trims == 4);
	capture.restore();
}

void aClearedObserverHearsNothingMore()
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	Heard first;
	Heard second_observer;
	StderrCapture capture;
	const auto watch = uml::runtime::start_memory_pressure_response(
		nullptr, nullptr);
	require(watch != nullptr);
	uml::runtime::set_pressure_observer(&observeInto, &first);
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	uml::runtime::set_pressure_observer(&observeInto, &second_observer);
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	uml::runtime::set_pressure_observer(nullptr, nullptr);
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	capture.restore();
	require(first.reports.size() == 1, "the replaced observer heard on");
	require(second_observer.reports.size() == 1,
		"the cleared observer heard on");
}

/* An observer that records into storage of its own: it allocates nothing. */
struct FixedHeard final {
	std::array<PressureReport, 8> reports{};
	std::atomic<int> count{0};
};

void observeFixed(void *context, const PressureReport &report) noexcept
{
	auto &heard = *static_cast<FixedHeard *>(context);
	const int index = heard.count.load();
	if (index < static_cast<int>(heard.reports.size()))
		heard.reports[static_cast<std::size_t>(index)] = report;
	heard.count.store(index + 1);
}

/* A report is made when the system is short of memory, so it allocates
 * nothing: the same event is handled with and without an observer, and the
 * ordinary allocations of its whole handling on the pressure queue's thread are
 * the same count (an allocation the report added would show in the difference),
 * and an event still reaches the observer while the nothrow allocations fail. */
void aReportNeedsNoAllocation()
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	StderrCapture capture;
	const auto watch = uml::runtime::start_memory_pressure_response(
		nullptr, nullptr);
	require(watch != nullptr);
	require(pressure_handler.load() != nullptr, "the handler was not counted");
	/* The first events make what the handling makes once. */
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	const std::size_t without_observer = last_event_allocations.load();

	FixedHeard heard;
	uml::runtime::set_pressure_observer(&observeFixed, &heard);
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	const std::size_t with_observer = last_event_allocations.load();
	nothrow_allocation_fails.store(true);
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	nothrow_allocation_fails.store(false);
	uml::runtime::set_pressure_observer(nullptr, nullptr);
	capture.restore();
	require(heard.count.load() == 2, "a report was lost for want of memory");
	require(heard.reports[0].level == PressureLevel::critical);
	require(with_observer == without_observer,
		"the report allocated: " + std::to_string(with_observer) + " ordinary allocations with the observer, "
			+ std::to_string(without_observer) + " without");
}

/* The source delivers an event after the handler of the one before it ends, and
 * a level that arrives during a trim is merged into the source: while a trim
 * runs, nothing more is reported, and the levels raised meanwhile are reported
 * as one when their notification is delivered, before that notification's work. */
void aLevelRaisedWhileATrimRunsIsReportedWithItsNotification()
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	/* The levels merged here are the critical and the normal one; the system
	 * is at critical. */
	const SystemLevelIs system{4};
	struct Gate final {
		std::mutex mutex;
		std::condition_variable changed;
		bool inside = false;
		bool release = false;
	};
	static Gate gate;
	static std::atomic<int> trims{0};
	gate.inside = false;
	gate.release = false;
	trims.store(0);
	StderrCapture capture;
	FixedHeard heard;
	uml::runtime::set_pressure_observer(&observeFixed, &heard);
	const auto watch = uml::runtime::start_memory_pressure_response(
		[](void *, TrimKind) noexcept {
			if (trims.fetch_add(1) == 0) {
				std::unique_lock lock{gate.mutex};
				gate.inside = true;
				gate.changed.notify_all();
				(void)gate.changed.wait_for(lock, std::chrono::seconds{20},
					[] { return gate.release; });
			}
			return TrimOutcome::done;
		}, nullptr);
	require(watch != nullptr);
	dispatch_source_merge_data(raisable(), DISPATCH_MEMORYPRESSURE_WARN);
	{
		std::unique_lock lock{gate.mutex};
		require(gate.changed.wait_for(lock, std::chrono::seconds{20},
			[] { return gate.inside; }), "the trim never began");
	}
	dispatch_source_merge_data(raisable(), DISPATCH_MEMORYPRESSURE_CRITICAL);
	dispatch_source_merge_data(raisable(), DISPATCH_MEMORYPRESSURE_NORMAL);
	require(!waitUntil([&] { return heard.count.load() > 1; },
		std::chrono::milliseconds{200}),
		"a level raised during the trim was reported before the trim ended");
	{
		const std::lock_guard lock{gate.mutex};
		gate.release = true;
	}
	gate.changed.notify_all();
	/* Two calls on the serial queue: the notification the trim held back was
	 * queued behind the first. */
	dispatch_sync_f(pressure_queue.load(), nullptr, [](void *) {});
	dispatch_sync_f(pressure_queue.load(), nullptr, [](void *) {});
	uml::runtime::set_pressure_observer(nullptr, nullptr);
	capture.restore();
	require(heard.count.load() == 2,
		"the levels raised during the trim were not reported once");
	require(heard.reports[0].level == PressureLevel::warn
		&& heard.reports[0].reaction == PressureReaction::responding);
	require(heard.reports[1].level == PressureLevel::critical
		&& heard.reports[1].reaction == PressureReaction::responding,
		"the merged levels were not reported as the most serious, with a response");
	/* The warning's trimmer on, the critical level's trim and trimmer on. */
	require(trims.load() == 3);
}

void clearingTheObserverWaitsForACallInProgress()
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	struct Slow final {
		std::mutex gate;
		std::condition_variable changed;
		bool inside = false;
		bool release = false;
		std::atomic<bool> finished{false};
	} slow;
	StderrCapture capture;
	uml::runtime::set_pressure_observer([](void *context,
		const PressureReport &) noexcept {
		auto &slow = *static_cast<Slow *>(context);
		std::unique_lock lock{slow.gate};
		slow.inside = true;
		slow.changed.notify_all();
		(void)slow.changed.wait_for(lock, std::chrono::seconds{20},
			[&] { return slow.release; });
		lock.unlock();
		slow.finished.store(true);
	}, &slow);
	const auto watch = uml::runtime::start_memory_pressure_response(
		nullptr, nullptr);
	require(watch != nullptr);
	dispatch_source_merge_data(raisable(), DISPATCH_MEMORYPRESSURE_CRITICAL);
	{
		std::unique_lock lock{slow.gate};
		require(slow.changed.wait_for(lock, std::chrono::seconds{20},
			[&] { return slow.inside; }), "the observer was never called");
	}
	std::atomic<bool> cleared{false};
	std::thread clearing{[&] {
		uml::runtime::set_pressure_observer(nullptr, nullptr);
		/* The call had ended when this returned, so its context could go. */
		require(slow.finished.load(), "the clear returned inside the call");
		cleared.store(true);
	}};
	require(!waitUntil([&] { return cleared.load(); },
		std::chrono::milliseconds{300}), "the clear did not wait");
	{
		const std::lock_guard lock{slow.gate};
		slow.release = true;
	}
	slow.changed.notify_all();
	clearing.join();
	require(cleared.load());
	/* Let the response behind the call end before the watch goes. */
	dispatch_sync_f(pressure_queue.load(), nullptr, [](void *) {});
	capture.restore();
}

} // namespace

int main()
{
	static constexpr uml::testing::NamedTest tests[] {
		{"the-levels-of-the-source-are-the-most-serious-flag",
			theLevelsOfTheSourceAreTheMostSeriousFlag},
		{"the-normal-level-takes-nothing", theNormalLevelTakesNothing},
		{"a-warning-turns-the-gradual-trimmer-on-and-takes-nothing-else",
			aWarningTurnsTheGradualTrimmerOnAndTakesNothingElse},
		{"a-warning-says-what-became-of-the-gradual-trimmer",
			aWarningSaysWhatBecameOfTheGradualTrimmer},
		{"a-critical-level-frees-the-gpu-caches-trims-once-and-leaves-the-"
			"gradual-trimmer-on",
			aCriticalLevelFreesTheGpuCachesTrimsOnceAndLeavesTheGradualTrimmerOn},
		{"a-trim-that-could-not-be-sent-or-answered-is-said-so",
			aTrimThatCouldNotBeSentOrAnsweredIsSaidSo},
		{"a-busy-trim-is-tried-again", aBusyTrimIsTriedAgain},
		{"a-retry-that-is-still-busy-stops-after-its-attempts",
			aRetryThatIsStillBusyStopsAfterItsAttempts},
		{"a-retry-is-not-for-a-guest-the-host-has-quiesced",
			aRetryIsNotForAGuestTheHostHasQuiesced},
		{"a-retry-does-not-move-the-warn-spacing",
			aRetryDoesNotMoveTheWarnSpacing},
		{"a-later-response-or-the-normal-level-takes-the-pending-trim",
			aLaterResponseOrTheNormalLevelTakesThePendingTrim},
		{"the-normal-level-during-a-response-leaves-no-trim-pending",
			theNormalLevelDuringAResponseLeavesNoTrimPending},
		{"the-normal-level-during-a-warning-ends-the-gradual-trimmer-when-it-"
			"returns",
			theNormalLevelDuringAWarningEndsTheGradualTrimmerWhenItReturns},
		{"a-busy-guest-keeps-the-end-of-the-gradual-trimmer-pending",
			aBusyGuestKeepsTheEndOfTheGradualTrimmerPending},
		{"a-retry-is-not-joined-to-a-response-that-is-running",
			aRetryIsNotJoinedToAResponseThatIsRunning},
		{"the-clock-is-read-in-the-order-of-the-mutex",
			theClockIsReadInTheOrderOfTheMutex},
		{"every-level-is-reported-the-normal-level-too",
			everyLevelIsReportedTheNormalLevelToo},
		{"a-report-comes-before-the-work-of-the-response",
			aReportComesBeforeTheWorkOfTheResponse},
		{"a-warning-inside-the-interval-is-reported-as-withheld",
			aWarningInsideTheIntervalIsReportedAsWithheld},
		{"a-level-met-while-the-response-runs-is-reported-as-underway",
			aLevelMetWhileTheResponseRunsIsReportedAsUnderway},
		{"a-retry-reports-nothing", aRetryReportsNothing},
		{"the-names-of-the-reactions-are-the-lines-words",
			theNamesOfTheReactionsAreTheLinesWords},
		{"a-warning-starts-one-response-per-interval",
			aWarningStartsOneResponsePerInterval},
		{"a-warning-the-interval-holds-back-still-turns-the-trimmer-on",
			aWarningTheIntervalHoldsBackStillTurnsTheTrimmerOn},
		{"a-warning-or-a-critical-level-cancels-a-pending-end-of-the-trimmer",
			aWarningOrACriticalLevelCancelsAPendingEndOfTheTrimmer},
		{"a-busy-end-of-the-trimmer-is-tried-again-at-most-four-times",
			aBusyEndOfTheTrimmerIsTriedAgainAtMostFourTimes},
		{"a-critical-retry-that-trimmed-keeps-a-busy-trimmer-request-without-"
			"trimming-again",
			aCriticalRetryThatTrimmedKeepsABusyTrimmerRequestWithoutTrimmingAgain},
		{"a-gradual-request-that-could-not-be-sent-is-tried-again-within-its-"
			"allowance",
			aGradualRequestThatCouldNotBeSentIsTriedAgainWithinItsAllowance},
		{"an-end-of-the-trimmer-that-could-not-be-sent-stays-owed-and-a-refused-"
			"one-does-not",
			anEndOfTheTrimmerThatCouldNotBeSentStaysOwedAndARefusedOneDoesNot},
		{"a-warning-that-meets-the-retried-end-of-the-trimmer-turns-it-on-again",
			aWarningThatMeetsTheRetriedEndOfTheTrimmerTurnsItOnAgain},
		{"a-merged-delivery-is-acted-on-at-the-level-the-system-reports",
			aMergedDeliveryIsActedOnAtTheLevelTheSystemReports},
		{"the-watch-says-what-it-reads-of-the-system-level",
			theWatchSaysWhatItReadsOfTheSystemLevel},
		{"a-merged-delivery-from-the-system-source-is-acted-on-at-the-systems-"
			"level",
			aMergedDeliveryFromTheSystemSourceIsActedOnAtTheSystemsLevel},
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
		{"a-busy-guest-trim-is-tried-again-by-the-watch",
			aBusyGuestTrimIsTriedAgainByTheWatch},
		{"a-retry-needs-no-allocation-to-be-armed",
			aRetryNeedsNoAllocationToBeArmed},
		{"a-pending-retry-ends-with-the-watch", aPendingRetryEndsWithTheWatch},
		{"the-observer-hears-the-levels-the-system-raises",
			theObserverHearsTheLevelsTheSystemRaises},
		{"a-cleared-observer-hears-nothing-more",
			aClearedObserverHearsNothingMore},
		{"a-report-needs-no-allocation", aReportNeedsNoAllocation},
		{"a-level-raised-while-a-trim-runs-is-reported-with-its-notification",
			aLevelRaisedWhileATrimRunsIsReportedWithItsNotification},
		{"clearing-the-observer-waits-for-a-call-in-progress",
			clearingTheObserverWaitsForACallInProgress},
	};
	return uml::testing::runNamedTests(tests);
}
