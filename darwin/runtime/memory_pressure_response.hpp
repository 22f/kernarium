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
 * The system's memory pressure source reports three levels. A warning takes
 * nothing a person could notice, and a critical level takes what it can get
 * back at once:
 *
 *   - a warning turns the guest's own page-cache trimmer on (TrimKind::
 *     gradual_on): it trims the cold file cache in small passes a few seconds
 *     apart and backs off while the pages it took are read back. The host sends
 *     the request and does not wait for the guest's answer. The normal level
 *     turns the trimmer off, unless the application asked for it
 *     (uml_emulator_set_page_cache_trimming).
 *   - a critical level frees the host GPU's cached driver allocations
 *     (platform/gpu_cache_reclaim.hpp) and has the guest trim all of its cold
 *     page cache once (TrimKind::once), which unwires those pages and takes
 *     them out of the process's footprint, the only memory the iOS compressor
 *     cannot shrink (guest RAM is wired while committed), and leaves the
 *     gradual trimmer on while the pressure lasts.
 *
 * The gradual trimmer is on exactly while the last level is not normal. The
 * interval below limits the responses and their report, not that state: a
 * warning it withholds still asks for the trimmer to be on (the request is
 * free where the guest runs as asked), and any level but normal cancels a
 * pending request to turn it off.
 *
 * The system need not say when the pressure ends (an iPad's source never
 * delivered the normal level after a critical one). The level the response
 * holds therefore returns to normal by itself when the source has delivered
 * no level other than normal for `PressureTiming::quiet_period_ns`, on the
 * clock of PressureActions::now_ns (which does not run while the process is
 * suspended, as the caller's timer does not): on_retry() makes the return, the
 * same way the normal level does, and says so on its level line. A delivered
 * normal level ends it at once.
 *
 * A warning starts at most one response per `warn_interval_ns`, counted from
 * the start of the last response of either level, on the clock of
 * PressureActions::now_ns. A critical level always starts one. No response
 * starts while one is running: the event is dropped and counted, a higher level
 * is not queued and a running warning's response is not upgraded. The
 * translation caches, the guest's RAM and
 * every camera or other session are not touched.
 *
 * The guest requests are skipped, with no retry, for a guest the host has
 * quiesced (it trimmed the guest when it did, and a request would wake it; the
 * gradual trimmer's state is kept for the resume) and for one with no console
 * (TrimOutcome::unreachable), except that a gradual request that could not be
 * written is tried again. When another lifecycle call of the guest is in
 * progress (TrimOutcome::busy) the response keeps one request pending and tries
 * it again after `PressureTiming::retry_interval_ns`, up to `retry_attempts`
 * times: on_retry() runs it, and the next response (which tries for itself) or
 * the normal level replaces or ends it. A trim of all the cache that was tried
 * again and went through is followed by the gradual trimmer's request, with
 * attempts of its own and without repeating the trim. A retry never changes
 * when a warning may start.
 *
 * Each event the system raises is also reported to the application, once and
 * as soon as the response has decided what it does with it (PressureReport):
 * the level, and whether a response starts, is withheld by the interval or is
 * already running. The report is made by the call that handles the event,
 * before that response's work, and the system's watch makes those calls one at
 * a time in the order the events arrived. The normal level is reported too, so
 * that an application that shows the pressure can show its end. It carries no
 * figures.
 *
 * Each change of the level and each response (or its last retry) is one line
 * on stderr (the console of both applications). A level line carries the level
 * and the footprint; a critical response line the footprint before and after,
 * the bytes each part released and the trim's duration, and a warning's the
 * state of the gradual trimmer. Every line carries the count of guest RAM
 * wiring operations that failed (the first sign of a system that is short).
 * Each kind of line is rate limited, so that a storm of events cannot flood the
 * console; the next line that passes says how many were left out.
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

/* What the response does about a level the system has just reported, decided
 * before it does it. */
enum class PressureReaction : std::uint8_t {
	/* The normal level: no response starts (the gradual trimmer is turned
	 * off). */
	none = 0,
	/* A response starts: a warning turns the guest's gradual trimmer on, a
	 * critical level frees the graphics caches and has the guest trim all of
	 * its page cache. */
	responding = 1,
	/* A warning, and the last response began less than `warn_interval_ns`
	 * ago: no response starts for it. The gradual trimmer is still asked to
	 * be on. */
	withheld = 2,
	/* A response is running (or being tried again) and this event starts none
	 * of its own: a critical level that meets a running warning's response is
	 * not upgraded. The system delivers its notifications one at a time, each
	 * after the response of the one before, so this does not arise from it. */
	underway = 3,
};

/* The name the lines use. */
[[nodiscard]] std::string_view pressure_reaction_name(
	PressureReaction reaction) noexcept;

/* One event of the system's memory pressure source, as the application hears
 * it. */
struct PressureReport final {
	PressureLevel level{PressureLevel::normal};
	PressureReaction reaction{PressureReaction::none};
};

/* How the guest's page-cache trim ended. */
enum class TrimOutcome : std::uint8_t {
	/* The guest trimmed, or already ran as asked. */
	done,
	/* Sent, and its answer was not waited for (the gradual trimmer's two
	 * short requests). */
	sent,
	/* Sent, and the guest had not finished within the bound. */
	unanswered,
	/* Sent, and the guest refused it, as a kernel without the command does. */
	refused,
	/* Not sent: there is no console to send it to. */
	unreachable,
	/* Not sent: the host has quiesced the guest, so the guest was trimmed
	 * when it was suspended and a request would wake it. */
	quiesced,
	/* Not sent: another lifecycle call of the guest was in progress. The
	 * only outcome the response tries again. */
	busy,
};

[[nodiscard]] std::string_view trim_outcome_name(TrimOutcome outcome) noexcept;

/* What a guest trim asks of the guest. */
enum class TrimKind : std::uint8_t {
	/* All of the cold page cache and the free memory, now, and the reply is
	 * waited for, bounded ("trim once"). */
	once,
	/* The guest's periodic trimmer on ("trim on"): the request is one short
	 * message that is not waited for, and the trimming is the guest's, in small
	 * passes that back off while what they took is read back. */
	gradual_on,
	/* The periodic trimmer off ("trim off"). */
	gradual_off,
};

/* What the response reads of the process at one moment. */
struct PressureFigures final {
	/* What the system holds the process to (phys_footprint). */
	std::uint64_t footprint{};
	/* The part of the guest's RAM the host holds; 0 without a guest. */
	std::uint64_t guest_held{};
	/* The footprint's graphics ledger, compressed pages included. */
	std::uint64_t graphics{};
	/* Guest RAM wiring operations that failed, since the guest began (the
	 * count is of operations, each over a range of pages). */
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
	/* Asks the guest for `kind` of trim. "Once" waits for the trim, bounded;
	 * the gradual trimmer's requests wait for nothing. */
	[[nodiscard]] virtual TrimOutcome trim_page_cache(TrimKind kind) noexcept = 0;
	/* One line, without its newline. */
	virtual void log(std::string_view line) noexcept = 0;
	/* Tells the application what the system reported and what the host does
	 * about it. Called once for each event, as soon as that is decided and
	 * before the response's work, on the thread that handles the event; it
	 * returns promptly. */
	virtual void report(const PressureReport &report) noexcept = 0;
};

/* The response's pacing; a test shortens the retry. */
struct PressureTiming final {
	/* How long after a trim that found the guest busy the next try is due. */
	std::uint64_t retry_interval_ns = 1'000'000'000ULL;
	/* How many times a busy trim is tried again; 0 for never. */
	unsigned retry_attempts = 4;
	/* How long the source must deliver no level but the normal one for the
	 * response to return to normal itself; 0 for never. Three times the longest
	 * gap seen between two critical levels of one episode (78 s, on an iPad,
	 * where the normal level never came). */
	std::uint64_t quiet_period_ns = 240'000'000'000ULL;
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
		/* on_retry() ran the pending guest trim again. */
		retried,
		/* on_retry() returned the level to normal after the quiet period. */
		quiet,
	};

	explicit MemoryPressureResponse(PressureActions &actions,
		PressureTiming timing = {}) noexcept
		: actions_{actions}, timing_{timing}
	{
	}

	MemoryPressureResponse(const MemoryPressureResponse &) = delete;
	MemoryPressureResponse &operator=(const MemoryPressureResponse &) = delete;

	/* The system reported `level`. Runs the response, when there is one, on
	 * the calling thread and returns once it has ended; any thread, and a call
	 * that meets a response running returns at once. */
	Outcome on_level(PressureLevel level) noexcept;

	/* How long from now the pending guest trim is due, 0 when none is. The
	 * caller runs on_retry() that long after each call that may leave one. */
	[[nodiscard]] std::uint64_t retry_delay_ns() const noexcept;

	/* How long from now the quiet period ends, 0 when the level held is
	 * normal (or the period is off). The caller runs on_retry() that long after
	 * each call that may have moved it. */
	[[nodiscard]] std::uint64_t quiet_delay_ns() const noexcept;

	/* The pending guest trim is tried again, on the calling thread. Nothing
	 * happens when none is pending (a later response or the normal level
	 * took it), and a call that meets a response running returns busy and
	 * leaves it pending. When the quiet period has ended it returns the level
	 * to normal instead, whatever is pending (the normal level takes it). */
	Outcome on_retry() noexcept;

	/* The response's counters, for a test or a report. */
	struct Counts final {
		std::uint64_t responses{};
		std::uint64_t limited{};
		std::uint64_t busy{};
		std::uint64_t retries{};
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

	/* Whether a line of `budget` may be written now (the clock is read under
	 * the mutex, so that calls from several threads see it in order);
	 * `dropped` receives the lines of that kind left out since the last
	 * one. */
	[[nodiscard]] bool admit_line(LineBudget &budget,
		std::uint64_t &dropped) noexcept;
	void log_level_change(PressureLevel from, PressureLevel to,
		bool quiet) noexcept;
	/* on_level(), and on_retry()'s return to normal (`quiet`), which does
	 * nothing unless the period has ended. */
	Outcome handle_level(PressureLevel level, bool quiet) noexcept;
	/* Whether the period has ended. Under the mutex, with the clock read
	 * under it. */
	[[nodiscard]] bool quiet_due(std::uint64_t now) const noexcept;
	void respond(PressureLevel level, std::uint64_t events_skipped) noexcept;
	/* Asks for the gradual trimmer off, after the normal level, and keeps the
	 * request pending when it could not be sent. */
	void end_gradual_trim() noexcept;
	/* Asks for the gradual trimmer on, for a warning that starts no response,
	 * and keeps the request pending when it could not be sent. */
	void arm_gradual_trim() noexcept;
	/* A gradual request of `kind`, noted. */
	[[nodiscard]] TrimOutcome ask_gradual(TrimKind kind) noexcept;
	/* Records what became of a request of `kind`: an ON that was asked is
	 * owed an OFF until one has been taken. Under the mutex. */
	void note_request(TrimKind kind, TrimOutcome outcome) noexcept;
	/* Leaves the request pending after a request that did not go through, for
	 * `level`, with the full attempts. Under the mutex. */
	void keep_pending(PressureLevel level, TrimKind kind) noexcept;

	PressureActions &actions_;
	const PressureTiming timing_;
	mutable std::mutex mutex_;
	PressureLevel level_{PressureLevel::normal};
	/* When the source last delivered a level other than normal. */
	std::uint64_t last_level_ns_{};
	bool running_{};
	bool started_{};
	std::uint64_t last_start_ns_{};
	std::uint64_t skipped_{};
	/* The one pending guest request: the level of the response that left it,
	 * what it asks and the tries it has left. A pending request for a trim of
	 * all the cache is followed by the gradual trimmer's, which has attempts of
	 * its own. */
	bool retry_pending_{};
	PressureLevel retry_level_{PressureLevel::normal};
	TrimKind retry_kind_{TrimKind::once};
	unsigned retry_attempts_left_{};
	/* The gradual trimmer was asked on and no request to turn it off has been
	 * taken since: the normal level owes it an off. */
	bool gradual_on_{};
	Counts counts_{};
	LineBudget level_lines_;
	LineBudget response_lines_;
};

/* The step the runtime cannot take itself: the host's requests to the guest
 * (the kinds of TrimKind), which the emulator layer owns. Called on the
 * response's thread, never with a lock the caller holds. */
using GuestTrim = TrimOutcome (*)(void *context, TrimKind kind) noexcept;

/* The response of a run, listening to the system's memory pressure source until
 * it is destroyed. The destructor returns once a response that is running has
 * ended, and no response, retry or call of the run's `GuestTrim` starts after
 * it. The source, its queue and the retry timer are the process's, made on the
 * first watch and kept: cancelling a source leaves a libdispatch worker thread
 * behind, and a run leaves no thread of its own. The timer starts, and takes
 * a thread of libdispatch's, with the first retry. */
class MemoryPressureWatch final {
public:
	~MemoryPressureWatch();

	MemoryPressureWatch(const MemoryPressureWatch &) = delete;
	MemoryPressureWatch &operator=(const MemoryPressureWatch &) = delete;

	struct State;

private:
	friend std::unique_ptr<MemoryPressureWatch> start_memory_pressure_response(
		GuestTrim, void *, PressureTiming);
	explicit MemoryPressureWatch(std::unique_ptr<State> state) noexcept;

	std::unique_ptr<State> state_;
};

/* Receives the reports of the run's response (PressureReport), on the thread
 * that handles the system's event, so it returns promptly. */
using PressureObserver = void (*)(void *context,
	const PressureReport &report) noexcept;

/* Sets, or with a null observer clears, the process's one receiver of the
 * reports. It outlives a run and hears the reports of every run after it was
 * set, none from before. Once this returns, no call to the previous observer
 * is running or will start, so its context may be released; the observer must
 * not call this. */
void set_pressure_observer(PressureObserver observer, void *context) noexcept;

/* Starts the response of the run that is beginning. `trim` and `context` are
 * the guest's page-cache trim. Null when the system's source cannot be made;
 * the run goes on without the response. */
[[nodiscard]] std::unique_ptr<MemoryPressureWatch> start_memory_pressure_response(
	GuestTrim trim, void *context, PressureTiming timing = {});

} // namespace uml::runtime
