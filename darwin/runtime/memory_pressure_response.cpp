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
	{
		const std::lock_guard lock{mutex_};
		/* Read under the mutex: calls from several threads then see the
		 * clock in the order the mutex gives them. */
		const std::uint64_t now = actions_.now_ns();
		previous = std::exchange(level_, level);
		if (level == PressureLevel::normal) {
			/* The pressure is over: a trim still pending is not wanted. */
			retry_pending_ = false;
			outcome = Outcome::nothing;
		} else if (running_) {
			++counts_.busy;
			++skipped_;
			outcome = Outcome::busy;
		} else if (level == PressureLevel::warn && started_ &&
			   elapsed(last_start_ns_, now) < warn_interval_ns) {
			++counts_.limited;
			++skipped_;
			outcome = Outcome::limited;
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
	if (level != previous)
		log_level_change(previous, level);
	if (outcome == Outcome::responded)
		respond(level, events_skipped);
	return outcome;
}

std::uint64_t MemoryPressureResponse::retry_delay_ns() const noexcept
{
	const std::lock_guard lock{mutex_};
	return retry_pending_ ? timing_.retry_interval_ns : 0U;
}

void MemoryPressureResponse::respond(PressureLevel level,
	std::uint64_t events_skipped) noexcept
{
	const PressureFigures before = actions_.figures();
	const std::uint64_t gpu_freed = actions_.reclaim_gpu();
	const PressureFigures after_gpu = actions_.figures();
	const std::uint64_t trim_start = actions_.now_ns();
	const TrimOutcome trim = actions_.trim_page_cache();
	const std::uint64_t trim_end = actions_.now_ns();
	const PressureFigures after = actions_.figures();
	{
		const std::lock_guard lock{mutex_};
		running_ = false;
		++counts_.responses;
		if (trim == TrimOutcome::busy && timing_.retry_attempts != 0U) {
			retry_pending_ = true;
			retry_level_ = level;
			retry_attempts_left_ = timing_.retry_attempts;
		}
	}

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
	line.text("; released ");
	size_text(line, fell_by(before.graphics, after_gpu.graphics));
	line.text(" graphics (").number(static_cast<std::int64_t>(gpu_freed))
		.text(" cached allocations), ");
	size_text(line, fell_by(after_gpu.guest_held, after.guest_held));
	line.text(" guest RAM (page cache trim ")
		.text(trim_outcome_name(trim)).text(", ")
		.number(static_cast<std::int64_t>(
			elapsed(trim_start, trim_end) / 1'000'000ULL))
		.text(" ms); ")
		.number(static_cast<std::int64_t>(after.wire_failures))
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
	unsigned attempt = 0;
	{
		const std::lock_guard lock{mutex_};
		if (!retry_pending_)
			return Outcome::nothing;
		if (running_)
			return Outcome::busy;
		running_ = true;
		level = retry_level_;
		--retry_attempts_left_;
		attempt = timing_.retry_attempts - retry_attempts_left_;
		++counts_.retries;
	}
	const PressureFigures before = actions_.figures();
	const std::uint64_t trim_start = actions_.now_ns();
	const TrimOutcome trim = actions_.trim_page_cache();
	const std::uint64_t trim_end = actions_.now_ns();
	const PressureFigures after = actions_.figures();
	bool again = false;
	{
		const std::lock_guard lock{mutex_};
		running_ = false;
		/* The normal level, or a response of its own, may have taken the
		 * pending trim meanwhile. */
		again = retry_pending_ && trim == TrimOutcome::busy &&
			retry_attempts_left_ != 0U;
		if (!again)
			retry_pending_ = false;
	}
	if (again)
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
		.text("): footprint ")
		.number(static_cast<std::int64_t>(before.footprint / mebibyte))
		.text(" -> ");
	footprint_text(line, after.footprint);
	line.text("; released ");
	size_text(line, fell_by(before.guest_held, after.guest_held));
	line.text(" guest RAM (page cache trim ")
		.text(trim_outcome_name(trim)).text(", ")
		.number(static_cast<std::int64_t>(
			elapsed(trim_start, trim_end) / 1'000'000ULL))
		.text(" ms); ")
		.number(static_cast<std::int64_t>(after.wire_failures))
		.text(" guest RAM wire failures");
	dropped_text(line, dropped);
	actions_.log({buffer, line.size()});
	return Outcome::retried;
}

} // namespace uml::runtime
