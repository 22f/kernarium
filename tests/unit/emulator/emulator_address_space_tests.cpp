// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/emulator.h"
#include "emulator_test_support.hpp"
#include "memory/domain_heap.hpp"
#include "native_window_test_support.hpp"
#include "require.hpp"
#include "runtime/guest_memory_budget.hpp"
#include "runtime/native_kernel_layout.hpp"
#include "runtime/start_refusal.hpp"
#include "testing.hpp"

#include <cstdio>
#include <cstdlib>
#include <mach/mach.h>
#include <sys/mman.h>
#include <stdexcept>
#include <string_view>
#include <sys/resource.h>

/* A process with little address space (an iOS application without Extended
 * Virtual Addressing has six GiB) refuses a guest through the run's pre-start
 * checks, one at a time: its window, then the process's heap region. A refusal
 * leaves nothing of the run in the process, and what the run reserved is
 * where a host that offers the room for it put it: the heap region does not
 * take the address a window needs. Each case is a process of its own, so that
 * the heap region is still unreserved and the address space it is limited to
 * is the one these runs use. The limit is the system's own (RLIMIT_AS, which
 * counts every mapping of the process). */

namespace {

using uml::testing::require;

constexpr std::uint64_t mib = std::uint64_t{1} << 20;

unsigned int entries;

int counting_kernel(int, char *const[], char *const[])
{
	++entries;
	return 0;
}

/* The mapped size of this process: what RLIMIT_AS limits. */
std::uint64_t mapped_bytes()
{
	task_vm_info_data_t information{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	require(task_info(mach_task_self(), TASK_VM_INFO,
			reinterpret_cast<task_info_t>(&information), &count) ==
			KERN_SUCCESS,
		"could not read the size of the process's mappings");
	return information.virtual_size;
}

/* The window of a guest of `guest_mib`: the length the product derives, the
 * RAM, an allowance of the same size held between 1 and 4 GiB, and the fixed
 * span of the shared-memory aperture and the stub pages. Derived, not
 * reserved: the process holds a window it has taken for as long as it lives. */
std::uint64_t window_bytes(std::uint64_t guest_mib)
{
	return uml::runtime::guest_memory::native_window_bytes(guest_mib * mib);
}

int run_with_memory(uml_emulator *emulator, const char *memory)
{
	const char *arguments[]{"uml-test", "--execution-engine=gadgets", memory};
	return uml_emulator_run(emulator, 3, arguments, nullptr);
}

void test_a_refused_heap_or_window_leaves_the_room_a_smaller_guest_needs()
{
	const auto emulator = uml::testing::make_emulator(counting_kernel);
	require(uml::memory::HeapRegion::instance().base() == 0,
		"the process's heap region was reserved before the runs: the test "
		"would not meet its refusal");

	/* The process has room for its heap region and a window of a 1536 MiB
	 * guest and a little more, less than a window of 2176 MiB takes beside
	 * the heap region: the larger guest gets its window and is refused the
	 * heap region, and the window it took must not stay, for the smaller
	 * guest must still be started. */
	const std::uint64_t small_window = window_bytes(1536);
	const std::uint64_t large_window = window_bytes(2176);
	constexpr std::uint64_t margin = 512 * mib;
	require(large_window > small_window + margin,
		"the test's two guests do not differ by more than its margin");
	rlimit limit{};
	require(getrlimit(RLIMIT_AS, &limit) == 0,
		"could not read the address space limit");
	limit.rlim_cur = mapped_bytes() + uml::memory::HeapRegion::reservation() +
		small_window + margin;
	require(setrlimit(RLIMIT_AS, &limit) == 0,
		"could not limit the address space");

	require(run_with_memory(emulator.get(), "mem=2176M") ==
			-UML_EMULATOR_OUT_OF_MEMORY,
		"a guest the process has no room for beside its heap region was not "
		"refused with the out-of-memory result");
	require(uml::runtime::process_native_window().begin == 0,
		"a refused run kept the window it reserved");
	require(entries == 0, "a guest the process could not hold entered its "
		"kernel");
	require(uml::runtime::start_refused_for_memory(),
		"a run the host refused for want of memory carries no mark that says "
		"so");
	require(run_with_memory(emulator.get(), "mem=1536M") == 0 && entries == 1,
		"a refused run took the room a smaller guest needed");
	require(!uml::runtime::start_refused_for_memory(),
		"a run that started carries the mark of the refused run before it");
	require(run_with_memory(emulator.get(), "mem=2176M") ==
			-UML_EMULATOR_OUT_OF_MEMORY && entries == 1,
		"the larger guest was started once the smaller one had been");
	require(uml::runtime::start_refused_for_memory(),
		"the second refusal carries no mark");
	/* A run that fails for another reason clears what the refusal before it
	 * left: the number a failed allocation of the library's own returns is the
	 * same, and must not read as a refusal of the guest's size. */
	require(uml_emulator_run(emulator.get(), -1, nullptr, nullptr) ==
			-UML_EMULATOR_INVALID_ARGUMENT,
		"a run with a negative argument count was not refused");
	require(!uml::runtime::start_refused_for_memory(),
		"a run that failed for another reason kept the mark of a refusal");
}

/* A process with no room for its heap region refuses every guest, and the
 * window the run reserved for it does not stay in the process. A refusal is
 * not for good: with the room, the same guest starts. */
void test_a_refused_heap_leaves_no_window_behind()
{
	const auto emulator = uml::testing::make_emulator(counting_kernel);
	require(uml::memory::HeapRegion::instance().base() == 0,
		"the process's heap region was reserved before the runs");
	rlimit limit{};
	require(getrlimit(RLIMIT_AS, &limit) == 0,
		"could not read the address space limit");
	limit.rlim_cur = mapped_bytes() + window_bytes(1536) + 512 * mib;
	require(setrlimit(RLIMIT_AS, &limit) == 0,
		"could not limit the address space");
	require(run_with_memory(emulator.get(), "mem=1536M") ==
			-UML_EMULATOR_OUT_OF_MEMORY && entries == 0,
		"a process without room for its heap region started a guest");
	require(uml::runtime::start_refused_for_memory(),
		"a refused heap region carries no mark of a refusal for memory");
	require(uml::runtime::process_native_window().begin == 0,
		"a refused heap region left the guest's window in the process");
	require(uml::memory::HeapRegion::instance().base() == 0,
		"a refused heap region was reserved");
	limit.rlim_cur = mapped_bytes() + window_bytes(1536) +
		uml::memory::HeapRegion::reservation() + 512 * mib;
	require(setrlimit(RLIMIT_AS, &limit) == 0,
		"could not raise the address space limit");
	require(run_with_memory(emulator.get(), "mem=1536M") == 0 && entries == 1,
		"a guest refused for want of room for its heap region was not started "
		"once the process had it");
	require(!uml::runtime::start_refused_for_memory(),
		"the run that started carries the mark of the refusal before it");
}

/* The pre-start trial of a guest's RAM is part of every run: a host that maps
 * no RAM where the kernel would ends the run before its kernel, and the next
 * run, tried where the host does map it, is started. The trial is made at an
 * address no process has, for a window the run reserved. */
void test_a_refused_ram_trial_ends_the_run_before_its_kernel()
{
	constexpr std::uintptr_t beyond_the_address_space = 1ULL << 48;
	const auto emulator = uml::testing::make_emulator(counting_kernel);
	uml::emulator::testing::place_guest_ram_trial(beyond_the_address_space);
	require(run_with_memory(emulator.get(), "mem=1536M") ==
			-UML_EMULATOR_OUT_OF_MEMORY && entries == 0,
		"a host that maps no RAM for the guest started it");
	require(uml::runtime::start_refused_for_memory(),
		"a refused RAM trial carries no mark of a refusal for memory");
	uml::emulator::testing::place_guest_ram_trial(0);
	require(run_with_memory(emulator.get(), "mem=1536M") == 0 && entries == 1,
		"a refused trial spent the process's guest");
}

/* A size the product has no layout for reserves nothing: no heap region, no
 * window. The geometry is checked before anything is reserved. */
void test_a_size_without_a_layout_reserves_nothing()
{
	const auto emulator = uml::testing::make_emulator(counting_kernel);
	require(uml::memory::HeapRegion::instance().base() == 0,
		"the process's heap region was reserved before the runs");
	require(run_with_memory(emulator.get(), "mem=0M") ==
			-UML_EMULATOR_INVALID_ARGUMENT && entries == 0,
		"a guest of no memory was not refused as an invalid argument");
	require(uml::memory::HeapRegion::instance().base() == 0,
		"a run that had no layout reserved the heap region");
	require(uml::runtime::process_native_window().begin == 0,
		"a run that had no layout reserved a window");
	require(!uml::runtime::start_refused_for_memory(),
		"a run with no layout is marked as refused for want of memory");
}

/* Fills every run of free address space below `end` that holds `size` bytes
 * (the host places an anonymous mapping in the lowest one), so that the next
 * mapping of that size is placed at the start of what is free at `end` and
 * below. The blocks stay mapped. */
void fill_free_runs_below(std::uintptr_t end, std::size_t size)
{
	for (;;) {
		void *const block = mmap(nullptr, size, PROT_NONE,
			MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
		if (block == MAP_FAILED)
			return;
		if (reinterpret_cast<std::uintptr_t>(block) >= end) {
			munmap(block, size);
			return;
		}
	}
}

/* The heap region goes where the host places it, and it must not take the
 * address a window needs: where the only room for both is one stretch of
 * address space whose start the heap region would take, the window, which is
 * at an exact address on macOS, is reserved first and the heap region has the
 * rest. The host's placement is the lowest free run that holds the size, so
 * the process below the window's address is filled, and the stretch freed
 * is the window's address with a little below it and room for the heap region
 * after. */
void test_the_heap_region_does_not_take_the_windows_address()
{
	const auto emulator = uml::testing::make_emulator(counting_kernel);
	const std::uintptr_t window = uml::testing::native_test_window_start;
	const std::uint64_t window_length = window_bytes(1536);
	const std::uint64_t heap = uml::memory::HeapRegion::reservation();
	constexpr std::uint64_t gib = std::uint64_t{1} << 30;
	const std::uintptr_t begin = window - gib;
	const std::uintptr_t end = window + window_length + heap + gib;
	fill_free_runs_below(end, 512 * gib);
	fill_free_runs_below(end, 64 * gib);
	fill_free_runs_below(end, heap);
	require(munmap(reinterpret_cast<void *>(begin), end - begin) == 0,
		"could not free the stretch the test leaves");
	/* Where a heap region reserved now would go: into the stretch, at the
	 * address the window needs or below it. */
	void *const probe = mmap(nullptr, heap, PROT_NONE,
		MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	require(probe != MAP_FAILED, "the stretch holds no heap region");
	const auto placed = reinterpret_cast<std::uintptr_t>(probe);
	munmap(probe, heap);
	require(placed >= begin && placed < window + window_length,
		"the test did not leave the host one place for a heap region, which "
		"covers the address of the window");
	require(run_with_memory(emulator.get(), "mem=1536M") == 0 && entries == 1,
		"a guest whose window the address space held was refused for the "
		"heap region that the host would place on it");
	const auto held = uml::runtime::process_native_window();
	require(held.begin == window,
		"the window was not reserved at its address");
	const auto heap_base = uml::memory::HeapRegion::instance().base();
	require(heap_base >= held.end || heap_base + heap <= held.begin,
		"the heap region overlaps the window");
}

} // namespace

int main(int argc, char **argv)
{
#if defined(UML_NATIVE_TEST_WINDOW_UNDER_TSAN) || \
	defined(UML_NATIVE_TEST_WINDOW_UNDER_ASAN)
	/* A sanitizer holds shadow memory that the limit would count. */
	std::puts("skipped under a sanitizer");
	return EXIT_SUCCESS;
#else
	try {
		uml::emulator::testing::place_native_kernel_window(
			uml::testing::native_test_window_start);
		const std::string_view mode = argc > 1 ? argv[1] : "";
		if (mode == "--no-room-for-the-heap")
			test_a_refused_heap_leaves_no_window_behind();
		else if (mode == "--ram-trial")
			test_a_refused_ram_trial_ends_the_run_before_its_kernel();
		else if (mode == "--no-layout")
			test_a_size_without_a_layout_reserves_nothing();
		else if (mode == "--heap-and-window-addresses")
			test_the_heap_region_does_not_take_the_windows_address();
		else
			test_a_refused_heap_or_window_leaves_the_room_a_smaller_guest_needs();
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "emulator address space test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
#endif
}
