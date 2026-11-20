// SPDX-License-Identifier: GPL-2.0-or-later

#include "apple/os_address_space.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <sys/mman.h>

namespace {

using uml::testing::require;
namespace platform = uml::platform;
using platform::AddressRange;

constexpr std::uint64_t mib = 1024ULL * 1024ULL;
constexpr std::uint64_t gib = 1024ULL * mib;

/* The first mapped range that ends after `address`, from a map in address
 * order, as the walk of a process answers it. */
auto mapOf(std::vector<AddressRange> ranges)
{
	return [ranges = std::move(ranges)](std::uint64_t address)
		-> std::optional<AddressRange> {
		for (const auto &range : ranges)
			if (range.end > address)
				return range;
		return std::nullopt;
	};
}

/* The iPad mini 6 of a process signed without Extended Virtual Addressing:
 * the program's page zero, the shared region, the system's guard regions up to
 * 448 GiB, and what is left of the 454 GiB the system lets it map. */
void theRunAboveTheGuardRegionIsTheWidestFromTheFloor()
{
	const auto map = mapOf({
		{0, 4 * gib + 0x108000},
		{6 * gib, 63 * gib},
		{63 * gib, 64 * gib},
		{64 * gib, 448 * gib},
	});
	require(platform::largest_free_run(48 * gib, 454 * gib, map) == 6 * gib);
	/* From the floor of zero the run below the shared region is shorter. */
	require(platform::largest_free_run(0, 454 * gib, map) == 6 * gib);
	/* A guest's window and the rest of what it maps leave less. */
	const auto holding = mapOf({
		{0, 4 * gib},
		{6 * gib, 64 * gib},
		{64 * gib, 448 * gib},
		{448 * gib, 453 * gib + 44 * 1024},
	});
	require(platform::largest_free_run(48 * gib, 454 * gib, holding) ==
		gib - 44 * 1024);
}

void aRangeThatStartsBelowTheFloorCountsFromTheFloor()
{
	const auto map = mapOf({{0, 60 * gib}, {64 * gib, 70 * gib}});
	require(platform::largest_free_run(48 * gib, 100 * gib, map) == 30 * gib,
		"the run after the last range");
	require(platform::largest_free_run(48 * gib, 70 * gib, map) == 4 * gib,
		"the gap between two ranges, from where the first ends");
	require(platform::largest_free_run(48 * gib, 60 * gib, map) == 0,
		"every byte mapped");
}

void nothingMappedIsTheWholeSpanAndTheCeilingEndsTheLastRun()
{
	require(platform::largest_free_run(48 * gib, 454 * gib, mapOf({})) ==
		406 * gib);
	require(platform::largest_free_run(10, 10, mapOf({})) == 0);
	require(platform::largest_free_run(20, 10, mapOf({})) == 0);
	/* A range past the ceiling ends the walk with the run before it. */
	require(platform::largest_free_run(48 * gib, 50 * gib,
		mapOf({{60 * gib, 70 * gib}})) == 2 * gib);
	/* The widest run is the widest of several. */
	require(platform::largest_free_run(0, 100 * gib,
		mapOf({{10 * gib, 12 * gib}, {20 * gib, 60 * gib},
			{61 * gib, 90 * gib}})) == 10 * gib);
}

void aWalkThatMakesNoProgressEnds()
{
	/* A map that answers a range ending at or before the cursor would loop;
	 * it ends with what was found. */
	const auto stuck = [](std::uint64_t) -> std::optional<AddressRange> {
		return AddressRange{0, 1};
	};
	require(platform::largest_free_run(10, 100, stuck) == 0);
}

/* The system's own map: a reservation of the run the walk reports, or of a
 * size within it, is what mmap gives, wherever the host puts it. */
void aReservationOfTheMeasuredRunSucceeds()
{
	const auto run = platform::os_largest_free_address_run(48 * gib);
	require(run.has_value(), "the system answers");
	require(*run > 0, "a process has address space free");
	const std::uint64_t size = std::min<std::uint64_t>(*run, 4 * gib);
	void *const mapping = ::mmap(nullptr, size, PROT_NONE,
		MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	require(mapping != MAP_FAILED, "the run holds a reservation of its size");
	::munmap(mapping, size);
}

/* What the process maps never widens the run: a reservation the walk sees
 * leaves no wider run, and handing it back leaves none narrower. */
void aReservationNeverGrowsTheRunItWasTakenFrom()
{
	const auto before = platform::os_largest_free_address_run(0);
	require(before.has_value());
	/* Everything the walk calls free is free: reserve a quarter GiB, and the
	 * widest run is never larger afterwards. */
	void *const mapping = ::mmap(nullptr, 256 * mib, PROT_NONE,
		MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	require(mapping != MAP_FAILED);
	const auto after = platform::os_largest_free_address_run(0);
	require(after.has_value() && *after <= *before);
	::munmap(mapping, 256 * mib);
	const auto restored = platform::os_largest_free_address_run(0);
	require(restored.has_value() && *restored >= *after);
}

} // namespace

int main()
{
	static constexpr uml::testing::NamedTest tests[] {
		{"the-run-above-the-guard-region-is-the-widest-from-the-floor",
			theRunAboveTheGuardRegionIsTheWidestFromTheFloor},
		{"a-range-that-starts-below-the-floor-counts-from-the-floor",
			aRangeThatStartsBelowTheFloorCountsFromTheFloor},
		{"nothing-mapped-is-the-whole-span-and-the-ceiling-ends-the-last-run",
			nothingMappedIsTheWholeSpanAndTheCeilingEndsTheLastRun},
		{"a-walk-that-makes-no-progress-ends", aWalkThatMakesNoProgressEnds},
		{"a-reservation-of-the-measured-run-succeeds",
			aReservationOfTheMeasuredRunSucceeds},
		{"a-reservation-never-grows-the-run-it-was-taken-from",
			aReservationNeverGrowsTheRunItWasTakenFrom},
	};
	return uml::testing::runNamedTests(tests);
}
