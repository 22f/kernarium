// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

#ifndef UML_MEMORY_ALLOCATION_TESTING
#define UML_MEMORY_ALLOCATION_TESTING 0
#endif
#include "../count_audit.hpp"

namespace uml::memory {

/* The executable-mapping accounting found a bug: a departure with nothing to
 * count out, or a count that names no record. Counts are not exact any more,
 * and a verdict kept on their strength may be wrong, so from then on the
 * process shares none: every stamp the memory layer reports says
 * never-share. The latch is one relaxed flag, set in every build and never
 * cleared (but by a test). */
void latch_sharing_off() noexcept;
[[nodiscard]] bool sharing_latched_off() noexcept;

} // namespace uml::memory

#if UML_MEMORY_ALLOCATION_TESTING
namespace uml::memory::testing {

enum class AllocationSite : std::uint8_t {
	none,
	backing,
	batch,
	batch_commit,
	batch_index,
	code_record,
	destroy_cleared,
	generation,
	history,
	immediate_change,
	reserve,
	software_domain_registry,
	native_after_mmap,
	native_backing_index,
	native_direct_record,
	native_domain_registry,
	native_mapping,
	native_operation,
	native_owner_record,
	native_owner_registry,
	native_slot,
	count,
};

void fail_next_allocation(AllocationSite site) noexcept;
void fail_allocation_at(AllocationSite site, std::size_t occurrence) noexcept;
void clear_allocation_failure() noexcept;
void allocation_checkpoint(AllocationSite site);
void reset_allocation_attempt_counts() noexcept;
[[nodiscard]] std::size_t allocation_attempt_count(
	AllocationSite site) noexcept;

/* Runs at every checkpoint, before any injected failure, on the thread that
 * reached it and under whatever locks that thread holds. It lets a test see
 * the state an operation has at that point, so it must not call back into the
 * memory domain. */
using AllocationObserver = void (*)(AllocationSite site) noexcept;
void set_allocation_observer(AllocationObserver observer) noexcept;

/* The executable-mapping counts of the physical-page records are exact by
 * construction. A departure with nothing to count out, or a count that names
 * no record, is a bug in the accounting; the production code survives it (the
 * count clamps at zero and the epoch moves) and a test must see it. */
void count_anomaly() noexcept;
[[nodiscard]] std::size_t count_anomalies() noexcept;
/* Lets a test that provoked an anomaly on purpose share again. */
void clear_sharing_latch() noexcept;

void backing_mapping_created() noexcept;
void backing_mapping_destroyed() noexcept;
[[nodiscard]] std::size_t active_backing_mappings() noexcept;

} // namespace uml::memory::testing

#define UML_MEMORY_ALLOCATION_CHECKPOINT(site) \
	::uml::memory::testing::allocation_checkpoint(site)
#define UML_MEMORY_COUNT_ANOMALY() \
	do { \
		::uml::memory::testing::count_anomaly(); \
		::uml::memory::latch_sharing_off(); \
	} while (0)
#define UML_MEMORY_BACKING_MAPPING_CREATED() \
	::uml::memory::testing::backing_mapping_created()
#define UML_MEMORY_BACKING_MAPPING_DESTROYED() \
	::uml::memory::testing::backing_mapping_destroyed()
#else
/* Production expands these probes away before semantic analysis. The
 * allocation-site enum and test functions therefore create no ordinary
 * product symbol, load, branch, or call even in an unoptimized build. The
 * one that sees an accounting bug latches sharing off in every build, and the
 * diagnostics build of a guest run counts it as well (count_audit.hpp reports
 * it). */
#define UML_MEMORY_ALLOCATION_CHECKPOINT(...) ((void)0)
#if UML_MEMORY_COUNT_AUDIT
#define UML_MEMORY_COUNT_ANOMALY() \
	do { \
		::uml::memory::note_count_anomaly(); \
		::uml::memory::latch_sharing_off(); \
	} while (0)
#else
#define UML_MEMORY_COUNT_ANOMALY() ::uml::memory::latch_sharing_off()
#endif
#define UML_MEMORY_BACKING_MAPPING_CREATED() ((void)0)
#define UML_MEMORY_BACKING_MAPPING_DESTROYED() ((void)0)
#endif
