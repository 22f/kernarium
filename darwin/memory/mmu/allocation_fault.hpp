// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

#ifndef UML_MEMORY_ALLOCATION_TESTING
#define UML_MEMORY_ALLOCATION_TESTING 0
#endif

#if UML_MEMORY_ALLOCATION_TESTING
namespace uml::memory::testing {

enum class AllocationSite : std::uint8_t {
	none,
	backing,
	batch,
	batch_commit,
	batch_index,
	generation,
	history,
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
	writers,
	holders,
	records,
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

/* The frame accounting holds a frame's stripe lock between these two calls on
 * the thread that holds it; a checkpoint reached in between is an allocation
 * under a stripe lock, which is counted. */
void stripe_lock_entered() noexcept;
void stripe_lock_left() noexcept;
/* An allocator the frame accounting's metadata uses, or a test's replacement
 * of the global operator new, reports here; one made under a stripe lock is
 * counted. */
void allocator_probe() noexcept;
/* The bytes the frame accounting's metadata holds now (frame_heap.hpp). */
void frame_bytes_changed(std::ptrdiff_t delta) noexcept;
[[nodiscard]] std::size_t frame_metadata_bytes() noexcept;
[[nodiscard]] std::size_t allocations_under_stripe_lock() noexcept;

void backing_mapping_created() noexcept;
void backing_mapping_destroyed() noexcept;
[[nodiscard]] std::size_t active_backing_mappings() noexcept;

} // namespace uml::memory::testing

#define UML_MEMORY_ALLOCATION_CHECKPOINT(site) \
	::uml::memory::testing::allocation_checkpoint(site)
#define UML_MEMORY_ALLOCATOR_PROBE() \
	::uml::memory::testing::allocator_probe()
#define UML_MEMORY_FRAME_BYTES(delta) \
	::uml::memory::testing::frame_bytes_changed(delta)
#define UML_MEMORY_STRIPE_LOCK_ENTERED() \
	::uml::memory::testing::stripe_lock_entered()
#define UML_MEMORY_STRIPE_LOCK_LEFT() \
	::uml::memory::testing::stripe_lock_left()
#define UML_MEMORY_BACKING_MAPPING_CREATED() \
	::uml::memory::testing::backing_mapping_created()
#define UML_MEMORY_BACKING_MAPPING_DESTROYED() \
	::uml::memory::testing::backing_mapping_destroyed()
#else
/* Production expands these probes away before semantic analysis. The
 * allocation-site enum and test functions therefore create no ordinary
 * product symbol, load, branch, or call even in an unoptimized build. */
#define UML_MEMORY_ALLOCATION_CHECKPOINT(...) ((void)0)
#define UML_MEMORY_ALLOCATOR_PROBE() ((void)0)
#define UML_MEMORY_FRAME_BYTES(...) ((void)0)
#define UML_MEMORY_STRIPE_LOCK_ENTERED() ((void)0)
#define UML_MEMORY_STRIPE_LOCK_LEFT() ((void)0)
#define UML_MEMORY_BACKING_MAPPING_CREATED() ((void)0)
#define UML_MEMORY_BACKING_MAPPING_DESTROYED() ((void)0)
#endif
