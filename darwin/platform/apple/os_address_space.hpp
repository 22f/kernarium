// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <mach/mach.h>

#include <algorithm>
#include <cstdint>
#include <optional>

namespace uml::platform {

/* One mapped range of a process's address space, [begin, end). */
struct AddressRange final {
	std::uint64_t begin;
	std::uint64_t end;
};

/* The widest run of address space from `floor` up to `ceiling` that no mapped
 * range holds. `next_range(address)` answers the first mapped range that ends
 * after `address`, in the order of address, or nothing where none lies at or
 * above it; a range may begin before `address`. Zero where every byte is
 * mapped. */
template<class NextRange>
[[nodiscard]] constexpr std::uint64_t largest_free_run(std::uint64_t floor,
	std::uint64_t ceiling, NextRange &&next_range) noexcept
{
	std::uint64_t cursor = floor;
	std::uint64_t widest = 0;
	while (cursor < ceiling) {
		const std::optional<AddressRange> range = next_range(cursor);
		if (!range || range->begin >= ceiling) {
			widest = std::max(widest, ceiling - cursor);
			break;
		}
		if (range->end <= cursor)
			break;
		if (range->begin > cursor)
			widest = std::max(widest, range->begin - cursor);
		cursor = range->end;
	}
	return widest;
}

/* The widest run of address space this process has free from `floor` up to the
 * highest address the system lets it map (task_vm_info's max_address), which is
 * the largest reservation a native kernel window can still be given there:
 * mmap(NULL, n) succeeds for the n that fit in some run. The system sets that
 * highest address by the process's signing (iOS gives an application without
 * Extended Virtual Addressing the small span above a guard region), so it is a
 * fact of the process, not a constant of the device. Nothing is reserved or
 * changed: one task_info call and a walk of the process's mappings. Nothing
 * where the system does not answer.
 *
 * Header-only and free of the rest of the platform, so the application, which
 * does not link the runtime, reads the figure the way the runtime does. */
[[nodiscard]] inline std::optional<std::uint64_t> os_largest_free_address_run(
	std::uint64_t floor) noexcept
{
	task_vm_info_data_t info{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO,
		    reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS ||
	    count < TASK_VM_INFO_COUNT || info.max_address == 0)
		return std::nullopt;
	bool answered = true;
	const auto widest = largest_free_run(floor, info.max_address,
		[&answered](std::uint64_t address) -> std::optional<AddressRange> {
			vm_address_t region = static_cast<vm_address_t>(address);
			vm_size_t size = 0;
			vm_region_basic_info_data_64_t information{};
			mach_msg_type_number_t information_count =
				VM_REGION_BASIC_INFO_COUNT_64;
			mach_port_t object = MACH_PORT_NULL;
			const kern_return_t result = vm_region_64(mach_task_self(),
				&region, &size, VM_REGION_BASIC_INFO_64,
				reinterpret_cast<vm_region_info_t>(&information),
				&information_count, &object);
			if (object != MACH_PORT_NULL)
				(void)mach_port_deallocate(mach_task_self(), object);
			if (result == KERN_INVALID_ADDRESS)
				return std::nullopt;
			if (result != KERN_SUCCESS || size == 0) {
				answered = false;
				return std::nullopt;
			}
			return AddressRange{region, std::uint64_t{region} + size};
		});
	if (!answered)
		return std::nullopt;
	return widest;
}

} // namespace uml::platform
