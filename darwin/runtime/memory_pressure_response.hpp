// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

namespace uml::runtime {

/*
 * What the host gives back when the system says it is short of memory.
 *
 * The system's memory pressure source (UIKit's memory warning is the same
 * event seen from an application) reports three levels. At the normal level
 * nothing is done. At a warning or a critical level the host releases what it
 * can get back cheaply, in two parts:
 *
 *   - the host GPU's cached driver allocations, the reclaim the GPU device
 *     already runs before it refuses a reservation (platform/
 *     gpu_cache_reclaim.hpp); and
 *   - one trim of the guest's page cache, which unwires those pages and takes
 *     them out of the process's footprint, the only memory the iOS compressor
 *     cannot shrink (guest RAM is wired while committed).
 *
 * A warning starts at most one response per `warn_interval`, counted from the
 * start of the last response of either level. A critical level always starts
 * one. No response starts while one is running: the event is dropped and
 * counted, since the running trim has the guest hand over all its cold page
 * cache. The translation caches, the guest's RAM and every camera or other
 * session are not touched.
 *
 * Each change of the level and each response is one line on stderr (the
 * console of both applications): the level, the footprint, the bytes each part
 * released, the trim's duration and the count of guest RAM pages the host could
 * not wire (the first sign of a system that is short). Each kind of line is
 * rate limited, so that a storm of events cannot flood the console; the next
 * line that passes says how many were left out.
 *
 * MemoryPressureResponse is the policy and holds no thread, clock or system
 * call: it reads and acts through PressureActions, so a test drives it with a
 * clock and actions of its own. start_memory_pressure_response() composes it
 * with the system's source and actions.
 */

enum class PressureLevel : std::uint8_t {
	normal = 0,
	warn = 1,
	critical = 2,
};

/* The name the lines use. */
[[nodiscard]] std::string_view pressure_level_name(PressureLevel level) noexcept;

/* The level a memory pressure source's data word states: the most serious flag
 * set (DISPATCH_MEMORYPRESSURE_CRITICAL 0x4, _WARN 0x2, _NORMAL 0x1), normal
 * for none. */
[[nodiscard]] PressureLevel pressure_level_from_dispatch(
	std::uint64_t flags) noexcept;

/* How the guest's page-cache trim ended. */
enum class TrimOutcome : std::uint8_t {
	/* The guest trimmed. */
	done,
	/* Sent, and the guest had not finished within the bound. */
	unanswered,
	/* Nothing was sent: no console, a kernel without the command, a guest the
	 * host has quiesced (which trimmed it already) or a lifecycle call in
	 * progress. */
	skipped,
};

[[nodiscard]] std::string_view trim_outcome_name(TrimOutcome outcome) noexcept;

/* What the response reads of the process at one moment. */
struct PressureFigures final {
	/* What the system holds the process to (phys_footprint). */
	std::uint64_t footprint{};
	/* The part of the guest's RAM the host holds; 0 without a guest. */
	std::uint64_t guest_held{};
	/* The footprint's graphics ledger, compressed pages included. */
	std::uint64_t graphics{};
	/* Guest RAM pages the host could not wire, since the guest began. */
	std::uint64_t wire_failures{};
};

/* Everything the policy reads and does. Implementations answer on the thread
 * that calls and never wait on one another. */
class PressureActions {
public:
	virtual ~PressureActions() = default;

	/* A clock that does not go backwards, in nanoseconds. */
	[[nodiscard]] virtual std::uint64_t now_ns() noexcept = 0;
	[[nodiscard]] virtual PressureFigures figures() noexcept = 0;
	/* Frees the host GPU's cached driver allocations; returns how many it
	 * freed, 0 with no GPU. */
	[[nodiscard]] virtual std::uint64_t reclaim_gpu() noexcept = 0;
	/* Has the guest trim its page cache and waits for it, bounded. */
	[[nodiscard]] virtual TrimOutcome trim_page_cache() noexcept = 0;
	/* One line, without its newline. */
	virtual void log(std::string_view line) noexcept = 0;
};

class MemoryPressureResponse final {
public:
	/* The least between the start of one response and a warning's start of
	 * the next. Camera sessions that restart every 30 to 40 s were seen to
	 * raise a warning each; a trim before the next of them finds the cache
	 * the last one left refilled, and a minute would skip every second one. */
	static constexpr std::uint64_t warn_interval_ns = 30'000'000'000ULL;
	/* Each kind of line: at most `log_burst` in `log_window_ns`. */
	static constexpr std::uint64_t log_window_ns = 10'000'000'000ULL;
	static constexpr unsigned log_burst = 4;

	/* What a call to on_level() did. */
	enum class Outcome : std::uint8_t {
		/* The normal level. */
		nothing,
		/* A response ran. */
		responded,
		/* A warning, and the last response began less than `warn_interval`
		 * ago. */
		limited,
		/* A response is running. */
		busy,
	};

	explicit MemoryPressureResponse(PressureActions &actions) noexcept
		: actions_{actions}
	{
	}

	MemoryPressureResponse(const MemoryPressureResponse &) = delete;
	MemoryPressureResponse &operator=(const MemoryPressureResponse &) = delete;

	/* The system reported `level`. Runs the response, when there is one, on
	 * the calling thread and returns once it has ended; any thread, and a call
	 * that meets a response running returns at once. */
	Outcome on_level(PressureLevel level) noexcept;

	/* The response's counters, for a test or a report. */
	struct Counts final {
		std::uint64_t responses{};
		std::uint64_t limited{};
		std::uint64_t busy{};
		std::uint64_t lines_dropped{};
	};
	[[nodiscard]] Counts counts() const noexcept;

private:
	/* A fixed window of lines. Called under the response's mutex. */
	class LineBudget final {
	public:
		/* Whether a line may be written at `now_ns`; the lines left out since
		 * the last one is returned through `dropped`. */
		[[nodiscard]] bool admit(std::uint64_t now_ns,
			std::uint64_t &dropped) noexcept;

	private:
		std::uint64_t window_start_{};
		unsigned used_{};
		bool started_{};
		std::uint64_t dropped_{};
	};

	/* Whether a line of `budget` may be written now; `dropped` receives the
	 * lines of that kind left out since the last one. */
	[[nodiscard]] bool admit_line(LineBudget &budget, std::uint64_t now_ns,
		std::uint64_t &dropped) noexcept;
	void log_level_change(PressureLevel from, PressureLevel to) noexcept;
	void respond(PressureLevel level, std::uint64_t events_skipped) noexcept;

	PressureActions &actions_;
	mutable std::mutex mutex_;
	PressureLevel level_{PressureLevel::normal};
	bool running_{};
	bool started_{};
	std::uint64_t last_start_ns_{};
	std::uint64_t skipped_{};
	Counts counts_{};
	LineBudget level_lines_;
	LineBudget response_lines_;
};

/* The step the runtime cannot take itself: the host's request to the guest,
 * which the emulator layer owns. Called on the response's thread, never with a
 * lock the caller holds. */
using GuestTrim = TrimOutcome (*)(void *context) noexcept;

/* The response of a run, listening to the system's memory pressure source on
 * a queue of its own until it is destroyed. The destructor stops the source
 * and returns once a response that is running has ended. */
class MemoryPressureWatch final {
public:
	~MemoryPressureWatch();

	MemoryPressureWatch(const MemoryPressureWatch &) = delete;
	MemoryPressureWatch &operator=(const MemoryPressureWatch &) = delete;

	struct State;

private:
	friend std::unique_ptr<MemoryPressureWatch> start_memory_pressure_response(
		GuestTrim, void *);
	explicit MemoryPressureWatch(std::unique_ptr<State> state) noexcept;

	std::unique_ptr<State> state_;
};

/* Starts the response of the run that is beginning. `trim` and `context` are
 * the guest's page-cache trim. Null when the system's source cannot be made;
 * the run goes on without the response. */
[[nodiscard]] std::unique_ptr<MemoryPressureWatch> start_memory_pressure_response(
	GuestTrim trim, void *context);

} // namespace uml::runtime
