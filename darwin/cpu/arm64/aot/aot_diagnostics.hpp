// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>

#if !UML_AOT_DIAGNOSTICS
#error "aot_diagnostics.hpp is for builds that set UML_AOT_DIAGNOSTICS"
#endif

/* The counters, the test switches and the report of a diagnostics build of
 * the AOT runtime. Production builds do not set UML_AOT_DIAGNOSTICS and carry
 * none of this. The counters are fixed atomics bumped by the vCPU thread; the
 * report is written by a thread of its own to DARWIN_UML_AOT_REPORT
 * (a few times a second), so no vCPU thread does file I/O or allocates. */
namespace uml::cpu::arm64::aot::diagnostics {

struct Stats final {
	std::uint64_t binds{};
	/* Full verifications (every page) and pages hashed in all. */
	std::uint64_t verifications{};
	std::uint64_t pages_hashed{};
	/* Verifications that found a page unreadable. */
	std::uint64_t unreadable{};
	/* Instances whose pages differ from the ones the code stands for. */
	std::uint64_t poisoned{};
	std::uint64_t enters{};
	std::uint64_t steps{};
	/* Budget used up by entries: instructions only in a counting build
	 * (UML_AOT_COUNT_RETIRED), otherwise the prepaid charge. */
	std::uint64_t retired{};
	std::uint64_t refills{};
	std::uint64_t refill_failures{};
	std::uint64_t cross_hits{};
	std::uint64_t cross_misses{};
	std::array<std::uint64_t, 5> refusals{};
	/* Instructions the largest single entry ran (measured by counting each
	 * one, not estimated from the budget) and nanoseconds spent verifying
	 * instances. */
	std::uint64_t max_retired{};
	std::uint64_t verify_nanos{};
	/* Instructions the engine ran for rewritten code, and the nanoseconds
	 * that took in all (the engine's side of a step, entry to return). */
	std::uint64_t step_nanos{};
	std::uint64_t steps_run{};
	/* How many times the code-change sequence moved between lookups, and
	 * how many times a lookup asked the history what changed. */
	std::uint64_t code_bumps{};
	std::uint64_t history_queries{};
	/* Memory copy and set epilogues the generated thunk left to the host
	 * routine (a size above 64 bytes, a backward copy, a page crossing or
	 * a TLB miss). */
	std::uint64_t mops_slow{};
	std::uint64_t mops_fault{};
	std::uint64_t mops_illegal{};
	/* What binder lookups answered: hit, uncovered, not_ready, poisoned,
	 * unverified, no_entry, stale. */
	std::array<std::uint64_t, 7> finds{};
};

/* Override DARWIN_UML_AOT (nullopt: read the environment again). */
void set_enabled(std::optional<bool> state) noexcept;
/* Make every refill flush the TLB first (so that the next access to any
 * other page or kind misses too) and leave every register that the refill
 * thunk must preserve scrambled when the callback returns. */
void set_refill_stress(bool on) noexcept;
/* Call `hook(context)` whenever a run of the executor reaches AOT, after
 * it read the address space's sequences (nullptr: none). The hook may
 * publish a code change, as another vCPU would at that moment. */
void set_run_hook(void (*hook)(void *), void *context) noexcept;
[[nodiscard]] Stats stats() noexcept;
void reset_stats() noexcept;

} // namespace uml::cpu::arm64::aot::diagnostics
