// SPDX-License-Identifier: GPL-2.0-or-later
#include "memory_pressure_response.hpp"

#include "fixed_text.hpp"

#include <utility>

namespace uml::runtime {

namespace {

constexpr std::uint64_t kibibyte = 1024ULL;
constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;

/* The mebibytes of `bytes`, or its kibibytes under one. */
void size_text(FixedText &out, std::uint64_t bytes) noexcept
{
	if (bytes >= mebibyte || bytes == 0U)
		out.number(static_cast<std::int64_t>(bytes / mebibyte)).text(" MiB");
	else
		out.number(static_cast<std::int64_t>(bytes / kibibyte)).text(" KiB");
}

void footprint_text(FixedText &out, std::uint64_t bytes) noexcept
{
	out.number(static_cast<std::int64_t>(bytes / mebibyte)).text(" MiB");
}

[[nodiscard]] std::uint64_t fell_by(std::uint64_t before,
	std::uint64_t after) noexcept
{
	return before > after ? before - after : 0U;
}

/* The clock reads of two threads may cross; time does not run backwards. */
[[nodiscard]] std::uint64_t elapsed(std::uint64_t start,
	std::uint64_t end) noexcept
{
	return end > start ? end - start : 0U;
}

/* The tail every line carries when lines before it were left out. */
void dropped_text(FixedText &out, std::uint64_t dropped) noexcept
{
	if (dropped == 0U)
		return;
	out.text("; ").number(static_cast<std::int64_t>(dropped))
		.text(" earlier lines left out");
}

/* What the application is told of a call's outcome. */
[[nodiscard]] PressureReaction reaction_of(
	MemoryPressureResponse::Outcome outcome) noexcept
{
	switch (outcome) {
	case MemoryPressureResponse::Outcome::responded:
		return PressureReaction::responding;
	case MemoryPressureResponse::Outcome::limited:
		return PressureReaction::withheld;
	case MemoryPressureResponse::Outcome::busy:
		return PressureReaction::underway;
	case MemoryPressureResponse::Outcome::nothing:
	case MemoryPressureResponse::Outcome::retried:
		break;
	}
	return PressureReaction::none;
}

/* The request reached the guest, or will when the guest is resumed. */
[[nodiscard]] bool taken(TrimOutcome outcome) noexcept
{
	return outcome == TrimOutcome::done || outcome == TrimOutcome::sent ||
	       outcome == TrimOutcome::unanswered ||
	       outcome == TrimOutcome::quiesced;
}

/* A request that did not go through and may the next time: another call held
 * the guest or its console, or (for the gradual trimmer's short messages) the
 * message could not be written. The trim of all the cache is tried again only
 * when the guest was busy. */
[[nodiscard]] bool transient(TrimKind kind, TrimOutcome outcome) noexcept
{
	return outcome == TrimOutcome::busy ||
	       (kind != TrimKind::once && outcome == TrimOutcome::unreachable);
}

} // namespace

std::string_view pressure_level_name(PressureLevel level) noexcept
{
	switch (level) {
	case PressureLevel::normal:
		return "normal";
	case PressureLevel::warn:
		return "warn";
	case PressureLevel::critical:
		return "critical";
	}
	return "unknown";
}

PressureLevel pressure_level_from_dispatch(std::uint64_t flags) noexcept
{
	constexpr std::uint64_t warn = 0x2;
	constexpr std::uint64_t critical = 0x4;
	if ((flags & critical) != 0U)
		return PressureLevel::critical;
	if ((flags & warn) != 0U)
		return PressureLevel::warn;
	return PressureLevel::normal;
}

std::string_view pressure_reaction_name(PressureReaction reaction) noexcept
{
	switch (reaction) {
	case PressureReaction::none:
		return "none";
	case PressureReaction::responding:
		return "responding";
	case PressureReaction::withheld:
		return "withheld";
	case PressureReaction::underway:
		return "underway";
	}
	return "unknown";
}

std::string_view trim_outcome_name(TrimOutcome outcome) noexcept
{
	switch (outcome) {
	case TrimOutcome::done:
		return "done";
	case TrimOutcome::unanswered:
		return "unanswered";
	case TrimOutcome::refused:
		return "refused";
	case TrimOutcome::unreachable:
		return "no console";
	case TrimOutcome::quiesced:
		return "guest quiesced";
	case TrimOutcome::busy:
		return "busy";
	case TrimOutcome::sent:
		return "sent";
	}
	return "unknown";
}

bool MemoryPressureResponse::LineBudget::admit(std::uint64_t now_ns,
	std::uint64_t &dropped) noexcept
{
	if (!started_ || elapsed(window_start_, now_ns) >= log_window_ns) {
		started_ = true;
		window_start_ = now_ns;
		used_ = 0;
	}
	if (used_ >= log_burst) {
		++dropped_;
		return false;
	}
	++used_;
	dropped = std::exchange(dropped_, 0U);
	return true;
}

MemoryPressureResponse::Counts MemoryPressureResponse::counts() const noexcept
{
	const std::lock_guard lock{mutex_};
	return counts_;
}

bool MemoryPressureResponse::admit_line(LineBudget &budget,
	std::uint64_t &dropped) noexcept
{
	const std::lock_guard lock{mutex_};
	if (budget.admit(actions_.now_ns(), dropped))
		return true;
	++counts_.lines_dropped;
	return false;
}

void MemoryPressureResponse::log_level_change(PressureLevel from,
	PressureLevel to) noexcept
{
	std::uint64_t dropped = 0;
	if (!admit_line(level_lines_, dropped))
		return;
	const PressureFigures now = actions_.figures();
	char buffer[320];
	FixedText line{buffer, sizeof(buffer)};
	line.text("uml: memory pressure ").text(pressure_level_name(to))
		.text(" (was ").text(pressure_level_name(from)).text("): footprint ");
	footprint_text(line, now.footprint);
	line.text(", ").number(static_cast<std::int64_t>(now.wire_failures))
		.text(" guest RAM wire failures");
	dropped_text(line, dropped);
	actions_.log({buffer, line.size()});
}

MemoryPressureResponse::Outcome MemoryPressureResponse::on_level(
	PressureLevel level) noexcept
{
	PressureLevel previous = PressureLevel::normal;
	Outcome outcome = Outcome::nothing;
	std::uint64_t events_skipped = 0;
	bool end_gradual = false;
	bool arm_gradual = false;
	{
		const std::lock_guard lock{mutex_};
		/* Read under the mutex: calls from several threads then see the
		 * clock in the order the mutex gives them. */
		const std::uint64_t now = actions_.now_ns();
		previous = std::exchange(level_, level);
		if (level == PressureLevel::normal) {
			/* The pressure is over: a request still pending is not wanted,
			 * but the gradual trimmer is ended, unless a response that is
			 * running ends it when it returns. */
			retry_pending_ = false;
			end_gradual = gradual_on_ && !running_;
			outcome = Outcome::nothing;
		} else {
			/* The pressure is back: an end of the gradual trimmer that is
			 * still pending is no longer wanted. */
			if (retry_pending_ && retry_kind_ == TrimKind::gradual_off)
				retry_pending_ = false;
			if (running_) {
				++counts_.busy;
				++skipped_;
				outcome = Outcome::busy;
			} else if (level == PressureLevel::warn && started_ &&
				   elapsed(last_start_ns_, now) < warn_interval_ns) {
				++counts_.limited;
				++skipped_;
				outcome = Outcome::limited;
				/* The interval limits the responses, not the trimmer's
				 * state: the request for it to be on is made again, unless
				 * one that is pending already asks for it. The guest side
				 * sends nothing when the guest runs as asked. */
				arm_gradual = !retry_pending_;
			} else {
				running_ = true;
				started_ = true;
				last_start_ns_ = now;
				events_skipped = std::exchange(skipped_, 0U);
				/* This response's own trim replaces a pending one. */
				retry_pending_ = false;
				outcome = Outcome::responded;
			}
		}
	}
	/* Before anything that reads the process or waits: the application hears
	 * the event at once, and the response's work follows. */
	actions_.report({level, reaction_of(outcome)});
	if (level != previous)
		log_level_change(previous, level);
	if (end_gradual)
		end_gradual_trim();
	if (arm_gradual)
		arm_gradual_trim();
	if (outcome == Outcome::responded)
		respond(level, events_skipped);
	return outcome;
}

std::uint64_t MemoryPressureResponse::retry_delay_ns() const noexcept
{
	const std::lock_guard lock{mutex_};
	return retry_pending_ ? timing_.retry_interval_ns : 0U;
}

void MemoryPressureResponse::note_request(TrimKind kind,
	TrimOutcome outcome) noexcept
{
	/* Whatever became of an ON, the guest or the host may hold it. */
	if (kind == TrimKind::gradual_on)
		gradual_on_ = true;
	/* An OFF that was taken ends it, and so does one the guest refused: a
	 * kernel without the command has no trimmer to turn off. One that could
	 * not be sent leaves the end owed. */
	else if (kind == TrimKind::gradual_off &&
		 (taken(outcome) || outcome == TrimOutcome::refused))
		gradual_on_ = false;
}

void MemoryPressureResponse::keep_pending(PressureLevel level,
	TrimKind kind) noexcept
{
	if (timing_.retry_attempts == 0U)
		return;
	retry_pending_ = true;
	retry_level_ = level;
	retry_kind_ = kind;
	retry_attempts_left_ = timing_.retry_attempts;
}

TrimOutcome MemoryPressureResponse::ask_gradual(TrimKind kind) noexcept
{
	const TrimOutcome outcome = actions_.trim_page_cache(kind);
	const std::lock_guard lock{mutex_};
	note_request(kind, outcome);
	return outcome;
}

void MemoryPressureResponse::end_gradual_trim() noexcept
{
	const TrimOutcome outcome = ask_gradual(TrimKind::gradual_off);
	const std::lock_guard lock{mutex_};
	/* The pressure came back meanwhile: that response asks for itself. */
	if (transient(TrimKind::gradual_off, outcome) &&
	    level_ == PressureLevel::normal && !running_)
		keep_pending(PressureLevel::normal, TrimKind::gradual_off);
}

void MemoryPressureResponse::arm_gradual_trim() noexcept
{
	const TrimOutcome outcome = ask_gradual(TrimKind::gradual_on);
	bool end = false;
	{
		const std::lock_guard lock{mutex_};
		if (level_ == PressureLevel::normal)
			end = !running_;
		else if (transient(TrimKind::gradual_on, outcome) && !running_ &&
			 !retry_pending_)
			keep_pending(level_, TrimKind::gradual_on);
	}
	/* The normal level came meanwhile and found nothing to end. */
	if (end)
		end_gradual_trim();
}

void MemoryPressureResponse::respond(PressureLevel level,
	std::uint64_t events_skipped) noexcept
{
	const bool critical = level == PressureLevel::critical;
	const PressureFigures before = actions_.figures();
	/* A warning takes nothing a person could notice. The GPU's caches are the
	 * critical level's because a warning gets no footprint from them (a
	 * rendering client takes the storage back within a second), and so is the
	 * one trim of all the cache, which stalls frames. */
	const std::uint64_t gpu_freed = critical ? actions_.reclaim_gpu() : 0U;
	const PressureFigures after_gpu = actions_.figures();
	const std::uint64_t trim_start = actions_.now_ns();
	TrimOutcome once = TrimOutcome::done;
	if (critical)
		once = actions_.trim_page_cache(TrimKind::once);
	const std::uint64_t trim_end = actions_.now_ns();
	/* The gradual trimmer is on while the pressure lasts: the pressure that is
	 * critical now is a warning when it eases, and the trimmer then keeps the
	 * cache from filling again. A trim that found the guest busy is tried
	 * again first. */
	TrimOutcome gradual = TrimOutcome::done;
	const bool gradual_asked = !critical || once != TrimOutcome::busy;
	if (gradual_asked)
		gradual = ask_gradual(TrimKind::gradual_on);
	const PressureFigures after = actions_.figures();
	bool end_after = false;
	{
		const std::lock_guard lock{mutex_};
		running_ = false;
		++counts_.responses;
		/* Not for a pressure that the normal level ended during the request. */
		if (level_ != PressureLevel::normal) {
			if (critical && once == TrimOutcome::busy)
				keep_pending(level, TrimKind::once);
			else if (transient(TrimKind::gradual_on, gradual))
				keep_pending(level, TrimKind::gradual_on);
		} else {
			end_after = gradual_on_;
		}
	}
	if (end_after)
		end_gradual_trim();

	std::uint64_t dropped = 0;
	if (!admit_line(response_lines_, dropped))
		return;
	char buffer[448];
	FixedText line{buffer, sizeof(buffer)};
	line.text("uml: memory pressure ").text(pressure_level_name(level))
		.text(": footprint ")
		.number(static_cast<std::int64_t>(before.footprint / mebibyte))
		.text(" -> ");
	footprint_text(line, after.footprint);
	if (critical) {
		line.text("; released ");
		size_text(line, fell_by(before.graphics, after_gpu.graphics));
		line.text(" graphics (").number(static_cast<std::int64_t>(gpu_freed))
			.text(" cached allocations), ");
		size_text(line, fell_by(after_gpu.guest_held, after.guest_held));
		line.text(" guest RAM (page cache trim ")
			.text(trim_outcome_name(once)).text(", ")
			.number(static_cast<std::int64_t>(
				elapsed(trim_start, trim_end) / 1'000'000ULL))
			.text(" ms)");
	} else {
		line.text("; guest page cache trimmer on (")
			.text(trim_outcome_name(gradual)).text(")");
	}
	line.text("; ").number(static_cast<std::int64_t>(after.wire_failures))
		.text(" guest RAM wire failures");
	if (events_skipped != 0U)
		line.text("; ").number(static_cast<std::int64_t>(events_skipped))
			.text(" events skipped since the last response");
	dropped_text(line, dropped);
	actions_.log({buffer, line.size()});
}

MemoryPressureResponse::Outcome MemoryPressureResponse::on_retry() noexcept
{
	PressureLevel level = PressureLevel::normal;
	TrimKind kind = TrimKind::once;
	unsigned attempt = 0;
	{
		const std::lock_guard lock{mutex_};
		if (!retry_pending_)
			return Outcome::nothing;
		if (running_)
			return Outcome::busy;
		running_ = true;
		level = retry_level_;
		kind = retry_kind_;
		--retry_attempts_left_;
		attempt = timing_.retry_attempts - retry_attempts_left_;
		++counts_.retries;
	}
	const PressureFigures before = actions_.figures();
	const std::uint64_t trim_start = actions_.now_ns();
	const TrimOutcome trim = kind == TrimKind::once ?
		actions_.trim_page_cache(TrimKind::once) : ask_gradual(kind);
	const std::uint64_t trim_end = actions_.now_ns();
	/* A trim of all the cache that went through also leaves the gradual
	 * trimmer on, as a critical response does, and is not asked again. */
	const bool follow = kind == TrimKind::once && trim != TrimOutcome::busy;
	TrimOutcome gradual = TrimOutcome::done;
	if (follow)
		gradual = ask_gradual(TrimKind::gradual_on);
	const PressureFigures after = actions_.figures();
	bool again = false;
	bool end_after = false;
	bool arm_after = false;
	{
		const std::lock_guard lock{mutex_};
		running_ = false;
		/* The normal level (which takes every pending request) or the
		 * pressure coming back (which takes a pending end) may have taken the
		 * pending request meanwhile. */
		const bool pending = retry_pending_;
		retry_pending_ = false;
		if (pending) {
			if (kind == TrimKind::once && follow) {
				if (transient(TrimKind::gradual_on, gradual)) {
					keep_pending(level, TrimKind::gradual_on);
					again = retry_pending_;
				}
			} else if (transient(kind, trim) && retry_attempts_left_ != 0U) {
				retry_pending_ = true;
				again = true;
			}
		}
		if (level_ == PressureLevel::normal)
			/* The pressure ended while the request was in the guest's
			 * hands: an off is owed, but one that was itself retried is
			 * not asked a second time here. */
			end_after = kind != TrimKind::gradual_off && gradual_on_;
		else if (kind == TrimKind::gradual_off && !again)
			/* It came back while the end was being retried, and the event
			 * found a retry running. */
			arm_after = true;
	}
	if (end_after)
		end_gradual_trim();
	if (arm_after)
		arm_gradual_trim();
	/* A trim that went through says what it did even when its follow-up
	 * comes next; a retry that failed says nothing until its last. */
	if (again && !follow)
		return Outcome::retried;

	std::uint64_t dropped = 0;
	if (!admit_line(response_lines_, dropped))
		return Outcome::retried;
	char buffer[448];
	FixedText line{buffer, sizeof(buffer)};
	line.text("uml: memory pressure ").text(pressure_level_name(level))
		.text(" (retry ").number(static_cast<std::int64_t>(attempt))
		.text(" of ")
		.number(static_cast<std::int64_t>(timing_.retry_attempts))
		.text("): ");
	if (kind == TrimKind::once) {
		line.text("footprint ")
			.number(static_cast<std::int64_t>(before.footprint / mebibyte))
			.text(" -> ");
		footprint_text(line, after.footprint);
		line.text("; released ");
		size_text(line, fell_by(before.guest_held, after.guest_held));
		line.text(" guest RAM (page cache trim ")
			.text(trim_outcome_name(trim)).text(", ")
			.number(static_cast<std::int64_t>(
				elapsed(trim_start, trim_end) / 1'000'000ULL))
			.text(" ms)");
	} else {
		line.text("guest page cache trimmer ")
			.text(kind == TrimKind::gradual_on ? "on" : "off")
			.text(" (").text(trim_outcome_name(trim)).text(")");
	}
	line.text("; ").number(static_cast<std::int64_t>(after.wire_failures))
		.text(" guest RAM wire failures");
	dropped_text(line, dropped);
	actions_.log({buffer, line.size()});
	return Outcome::retried;
}

} // namespace uml::runtime
