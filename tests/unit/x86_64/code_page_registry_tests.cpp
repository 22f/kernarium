/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/x86_64/code_page_registry.hpp"
#include "cpu/x86_64/software_tlb.hpp"
#include "require.hpp"

#include <malloc/malloc.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#include <sanitizer/allocator_interface.h>
#define UML_TEST_SANITIZER_ALLOCATOR 1
#endif
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using uml::cpu::x86_64::CodePageRegistry;
using uml::cpu::x86_64::NativeTlbEntry;
using uml::cpu::x86_64::native_tlb_entry_count;
using uml::cpu::x86_64::native_tlb_index;
using uml::testing::require;

constexpr std::uint64_t page_at(std::uint64_t number) noexcept
{
	return 0x400000U + number * 0x1000U;
}

void test_membership_is_counted()
{
	CodePageRegistry registry;
	const auto page = page_at(3);
	require(!registry.holds(page), "an empty registry holds a page");
	const auto before = registry.changes();
	(void)registry.add(page, nullptr);
	require(registry.holds(page), "an added page is not held");
	require(registry.changes() != before,
		"a page that gained code did not change the registry");
	const auto counted = registry.changes();
	(void)registry.add(page, nullptr);
	require(registry.changes() == counted,
		"a second holder of a page changed the registry");
	registry.remove(page);
	require(registry.holds(page), "one of two holders released the page");
	registry.remove(page);
	require(!registry.holds(page), "the last holder did not release the page");
	registry.remove(page);
	require(!registry.holds(page), "a stray removal underflowed the count");
	(void)registry.add(page, nullptr);
	require(registry.holds(page), "a page cannot be held again");
}

/* The bit in front of the counts keeps neighbours adjacent, so a run of code
 * pages shadows a run of data pages. A shadowed page holds no code: a store to
 * it has nothing to publish, however hot the page is.
 */
void test_membership_is_exact_behind_a_shared_bit()
{
	CodePageRegistry registry;
	const std::uint64_t code = std::uint64_t{0x400} << 12U;
	/* (page ^ page >> 18) of both is 0x400. */
	const std::uint64_t shadowed = std::uint64_t{0x40401} << 12U;
	(void)registry.add(code, nullptr);
	require(registry.holds(code), "the code page is not held");
	require(!registry.holds(shadowed),
		"a data page sharing the code page's bit is held");
	const auto before = registry.changes();
	(void)registry.add(shadowed, nullptr);
	require(registry.holds(shadowed), "the second page of a bit is not held");
	require(registry.changes() != before,
		"a page gained code behind a set bit without changing the registry");
	registry.remove(code);
	require(!registry.holds(code), "a released page is held through its bit");
	require(registry.holds(shadowed),
		"releasing one page of a bit released the other");
	registry.remove(shadowed);
	require(!registry.holds(shadowed), "the bit outlived both of its pages");
}

void test_membership_retires_write_translations()
{
	CodePageRegistry registry;
	auto translations =
		std::make_unique<NativeTlbEntry[]>(native_tlb_entry_count);
	CodePageRegistry::Participant participant;
	participant.write_translations = translations.get();
	registry.join(participant);
	const auto page = page_at(9);
	auto &entry = translations[native_tlb_index(page)];
	entry.write_page = page;
	(void)registry.add(page, &participant);
	require(entry.write_page == 1,
		"a page gained code under a live write translation");
	entry.write_page = page;
	registry.remove(page);
	require(entry.write_page == 1,
		"a tagged write translation outlived the page's code");
	registry.leave(participant);
	entry.write_page = page;
	(void)registry.add(page, nullptr);
	require(entry.write_page == page,
		"a participant that left was still retired");
}

/* A participant whose software TLB has its own layout retires its own
 * entries: the registry calls it for every page that gains or loses code, and
 * never once it has left.
 */
void test_participant_retires_its_own_translations()
{
	struct OwnTlb final {
		std::vector<std::uint64_t> retired;
	};
	CodePageRegistry registry;
	OwnTlb tlb;
	CodePageRegistry::Participant participant;
	registry.join(participant, [](void *context, std::uint64_t page) noexcept {
		static_cast<OwnTlb *>(context)->retired.push_back(page);
	}, &tlb);
	const auto page = page_at(11);
	(void)registry.add(page, &participant);
	require(tlb.retired.size() == 1 && tlb.retired[0] == page,
		"a page gained code without retiring the participant's entry");
	(void)registry.add(page, nullptr);
	require(tlb.retired.size() == 1,
		"a second holder retired the participant's entry again");
	registry.remove(page);
	registry.remove(page);
	require(tlb.retired.size() == 2 && tlb.retired[1] == page,
		"a page lost its code without retiring the participant's entry");
	registry.leave(participant);
	(void)registry.add(page, nullptr);
	require(tlb.retired.size() == 2,
		"a participant that left was still retired");
}

/* Every executor of an address space counts pages in and out at once. The
 * counts stay exact across stripes, a page one holder never releases stays
 * held throughout, and nothing is left behind.
 */
void test_concurrent_holders_stay_exact()
{
	constexpr unsigned holders = 8;
	constexpr unsigned pages = 96;
	constexpr unsigned rounds = 20000;
	CodePageRegistry registry;
	const auto pinned = page_at(pages + 1);
	(void)registry.add(pinned, nullptr);

	std::atomic<bool> start{false};
	std::atomic<bool> lost{false};
	std::vector<std::thread> threads;
	for (unsigned holder = 0; holder < holders; ++holder) {
		threads.emplace_back([&, holder] {
			while (!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			std::uint64_t random = 0x9e3779b97f4a7c15ULL * (holder + 1);
			for (unsigned round = 0; round < rounds; ++round) {
				random = random * 6364136223846793005ULL +
					1442695040888963407ULL;
				const auto page = page_at((random >> 33U) % pages);
				(void)registry.add(page, nullptr);
				if (!registry.holds(page) || !registry.holds(pinned))
					lost.store(true, std::memory_order_release);
				registry.remove(page);
			}
		});
	}
	start.store(true, std::memory_order_release);
	for (auto &thread : threads)
		thread.join();
	require(!lost.load(std::memory_order_acquire),
		"a held page was not held while others came and went");
	for (unsigned page = 0; page < pages; ++page)
		require(!registry.holds(page_at(page)),
			"a page stayed held after every holder released it");
	require(registry.holds(pinned), "the pinned page was released");
}

/* The registry counts a participant that leaves it while inside the engine;
 * a row that does not do that on purpose requires the count to stay 0. */
struct NoLateLeave final {
	const CodePageRegistry &registry;

	~NoLateLeave()
	{
		require(registry.leaves_inside_engine() == 0,
			"a participant left the registry inside the engine");
	}
};

/* A member the waiter is waiting for, and the retirer that orders the test
 * with the waiter's snapshot: the waiter retires the page's write
 * translations immediately before it snapshots the members, in its own
 * thread. */
struct SnapshotSignal final {
	std::atomic<bool> retired{false};

	static void retire(void *context, std::uint64_t) noexcept
	{
		static_cast<SnapshotSignal *>(context)->retired.store(
			true, std::memory_order_release);
	}

	/* The waiter has retired, and then had time to take its snapshot and
	 * start polling. */
	void settle() const
	{
		const auto deadline = Clock::now() + std::chrono::seconds{10};
		while (!retired.load(std::memory_order_acquire)) {
			require(Clock::now() < deadline,
				"the waiter never started its wait");
			std::this_thread::yield();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds{40});
	}
};

/* Well under the registry's 200 ms bound, and well over what a woken thread
 * needs. */
constexpr auto prompt = std::chrono::milliseconds{100};

/* A waiter that is waiting for a second member must not read it once it has
 * left the registry and been freed. The sanitizer run of this row
 * (`ctest --preset unit-asan-ubsan -R '^x86-64-code-page-registry$'`) is the
 * regression row for the read of the freed member; the plain run also fails
 * it when the freed member still reads as inside the engine, because the
 * waiter then waits out its whole bound.
 */
void test_waiter_does_not_read_a_member_that_was_freed()
{
	CodePageRegistry registry;
	const NoLateLeave no_late_leave{registry};
	SnapshotSignal signal;
	CodePageRegistry::Participant first;
	CodePageRegistry::Participant self;
	auto second = std::make_unique<CodePageRegistry::Participant>();
	registry.join(first, &SnapshotSignal::retire, &signal);
	registry.join(*second);
	registry.join(self);
	first.engine_epoch.store(1, std::memory_order_release);
	second->engine_epoch.store(1, std::memory_order_release);

	Clock::time_point returned{};
	std::thread waiter{[&] {
		(void)registry.add(page_at(21), &self);
		returned = Clock::now();
	}};
	signal.settle();
	/* The second member leaves the engine, then the registry, and is freed,
	 * while the waiter is still polling the first. */
	second->engine_epoch.fetch_add(1, std::memory_order_release);
	registry.leave(*second);
	second.reset();
	const auto moved = Clock::now();
	first.engine_epoch.fetch_add(1, std::memory_order_release);
	waiter.join();
	require(returned - moved < prompt,
		"the waiter waited for a member that had left the registry");
	registry.leave(first);
	registry.leave(self);
}

/* The storage of a departed member is reused by a new participant whose
 * epoch is the odd one the waiter recorded. The waiter does not wait for the
 * newcomer, joined or not: it is not the member that was waited for. Main
 * returns only after its bound, because it keeps waiting while the epoch at
 * the old address equals the recorded one. The storage is kept alive, so the
 * row has defined behaviour on every build.
 */
void test_waiter_ignores_a_new_participant_at_a_departed_address(bool join)
{
	CodePageRegistry registry;
	const NoLateLeave no_late_leave{registry};
	SnapshotSignal signal;
	CodePageRegistry::Participant first;
	CodePageRegistry::Participant self;
	alignas(CodePageRegistry::Participant)
		std::byte storage[sizeof(CodePageRegistry::Participant)];
	auto *held = std::construct_at(
		reinterpret_cast<CodePageRegistry::Participant *>(storage));
	registry.join(first, &SnapshotSignal::retire, &signal);
	registry.join(*held);
	registry.join(self);
	constexpr std::uint64_t recorded = 1;
	first.engine_epoch.store(recorded, std::memory_order_release);
	held->engine_epoch.store(recorded, std::memory_order_release);

	Clock::time_point returned{};
	std::thread waiter{[&] {
		(void)registry.add(page_at(22), &self);
		returned = Clock::now();
	}};
	signal.settle();
	held->engine_epoch.fetch_add(1, std::memory_order_release);
	registry.leave(*held);
	std::destroy_at(held);
	held = std::construct_at(
		reinterpret_cast<CodePageRegistry::Participant *>(storage));
	held->engine_epoch.store(recorded, std::memory_order_release);
	if (join)
		registry.join(*held);
	const auto moved = Clock::now();
	first.engine_epoch.fetch_add(1, std::memory_order_release);
	waiter.join();
	require(returned - moved < prompt,
		"the waiter waited for a participant that took a departed one's place");
	/* The newcomer leaves the engine, as a member must before it leaves. */
	held->engine_epoch.fetch_add(1, std::memory_order_release);
	if (join)
		registry.leave(*held);
	std::destroy_at(held);
	registry.leave(first);
	registry.leave(self);
}

/* A registry found by generation stays valid for as long as the finder holds
 * it, and a generation nobody holds is not found and not made. */
void test_find_shares_the_registry_without_making_one()
{
	constexpr std::uint64_t generation = (std::uint64_t{1} << 41U) + 7;
	require(CodePageRegistry::find(generation) == nullptr,
		"a registry nobody holds was found");
	require(CodePageRegistry::find(generation) == nullptr,
		"finding a registry made it");
	auto held = CodePageRegistry::acquire(generation);
	auto found = CodePageRegistry::find(generation);
	require(found == held, "a held registry was not the one found");
	held.reset();
	require(CodePageRegistry::find(generation) == found,
		"a registry a finder still holds was lost");
	(void)found->add(page_at(23), nullptr);
	require(found->holds(page_at(23)),
		"a registry outlived by its acquirer is unusable");
	found.reset();
	require(CodePageRegistry::find(generation) == nullptr,
		"a released registry was found");
}

/* A participant is out of the engine when it leaves the registry. A caller
 * that is not is counted, and the registry stays usable: the member is gone
 * and nothing waits for it. */
void test_leave_inside_the_engine_is_counted()
{
	CodePageRegistry registry;
	CodePageRegistry::Participant participant;
	registry.join(participant);
	participant.engine_epoch.store(1, std::memory_order_release);
	require(registry.leaves_inside_engine() == 0,
		"a participant that has not left was counted");
	registry.leave(participant);
	require(registry.leaves_inside_engine() == 1,
		"leaving the registry inside the engine was not counted");
	const auto began = Clock::now();
	(void)registry.add(page_at(40), nullptr);
	require(Clock::now() - began < prompt,
		"a wait waited for a participant that had left the registry");
}

/* A second translator of a page whose gain is still being settled waits
 * itself. On main it returned at once, because the page was already counted,
 * while the first adder was still waiting, and read the page before the
 * stores in flight had landed.
 */
void test_a_second_registration_waits_for_the_first()
{
	CodePageRegistry registry;
	const NoLateLeave no_late_leave{registry};
	SnapshotSignal signal;
	CodePageRegistry::Participant inside;
	CodePageRegistry::Participant first;
	CodePageRegistry::Participant second;
	registry.join(inside, &SnapshotSignal::retire, &signal);
	inside.engine_epoch.store(1, std::memory_order_release);
	const auto page = page_at(24);

	std::atomic<bool> second_returned{false};
	CodePageRegistry::Settlement first_result;
	CodePageRegistry::Settlement second_result;
	std::thread first_adder{[&] { first_result = registry.add(page, &first); }};
	signal.settle();
	std::thread second_adder{[&] {
		second_result = registry.add(page, &second);
		second_returned.store(true, std::memory_order_release);
	}};
	std::this_thread::sleep_for(std::chrono::milliseconds{60});
	require(!second_returned.load(std::memory_order_acquire),
		"a second registration returned while the page was unsettled");
	inside.engine_epoch.fetch_add(1, std::memory_order_release);
	first_adder.join();
	second_adder.join();
	require(first_result.settled && second_result.settled,
		"a wait that completed did not settle the page");
	/* Settled: the next holder has nothing to wait for. */
	inside.engine_epoch.store(3, std::memory_order_release);
	const auto before = Clock::now();
	const auto third = registry.add(page, nullptr);
	require(third.settled && Clock::now() - before < std::chrono::milliseconds{30},
		"a registration of a settled page waited");
	inside.engine_epoch.store(4, std::memory_order_release);
	registry.leave(inside);
}

/* A participant still inside the engine after the wait is stuck. The wait
 * says so once, reports the page unsettled, and later waits do not wait for
 * the same stuck participant again; the page settles by a wait that
 * completes. */
void test_a_stuck_participant_is_waited_out_once()
{
	using namespace std::chrono_literals;
	CodePageRegistry registry{{.slice = 20ms, .slices = 3}};
	const NoLateLeave no_late_leave{registry};
	CodePageRegistry::Participant stuck;
	registry.join(stuck);
	stuck.engine_epoch.store(1, std::memory_order_release);

	auto began = Clock::now();
	const auto first = registry.add(page_at(30), nullptr);
	require(!first.settled && first.stuck.size() == 1 &&
			first.stuck[0].epoch == 1,
		"a stuck participant did not leave the page unsettled");
	require(Clock::now() - began >= 60ms,
		"the wait ended before its slices did");
	require(registry.expiry_records() == 1,
		"the stuck participant was not remembered");
	const auto serial = first.stuck[0].serial;
	require(!registry.left_or_moved(serial, 1),
		"a participant still inside the engine has moved");

	/* Another page, and the same one again, do not wait for it. */
	began = Clock::now();
	const auto other = registry.add(page_at(31), nullptr);
	const auto again = registry.add(page_at(30), nullptr);
	require(Clock::now() - began < 40ms,
		"a later registration waited for a participant already waited out");
	require(!other.settled && !again.settled &&
			other.stuck.size() == 1 && again.stuck.size() == 1 &&
			other.stuck[0].serial == serial,
		"a later registration did not report the stuck participant");
	require(registry.expiry_records() == 1,
		"the same stuck participant was remembered twice");

	/* It leaves the engine: the next registration completes and settles. */
	stuck.engine_epoch.store(2, std::memory_order_release);
	require(registry.left_or_moved(serial, 1),
		"a participant that left the engine has not moved");
	const auto settled = registry.add(page_at(30), nullptr);
	require(settled.settled && settled.stuck.empty(),
		"a registration after the participant left was not settled");
	require(registry.expiry_records() == 0,
		"the record of a participant that moved was kept");
	began = Clock::now();
	const auto last = registry.add(page_at(31), nullptr);
	require(last.settled && Clock::now() - began < 40ms,
		"the other page did not settle once the participant left");
	registry.leave(stuck);
}

/* Two registrations that wait out the same participant at once record it
 * once. */
void test_concurrent_waiters_record_a_stuck_participant_once()
{
	using namespace std::chrono_literals;
	CodePageRegistry registry{{.slice = 20ms, .slices = 3}};
	const NoLateLeave no_late_leave{registry};
	CodePageRegistry::Participant stuck;
	registry.join(stuck);
	stuck.engine_epoch.store(1, std::memory_order_release);
	std::atomic<unsigned> ready{0};
	const auto wait = [&](std::uint64_t number) {
		ready.fetch_add(1);
		while (ready.load() != 2)
			std::this_thread::yield();
		const auto result = registry.add(page_at(number), nullptr);
		require(!result.settled && result.stuck.size() == 1,
			"a stuck participant did not leave the page unsettled");
	};
	std::thread first{wait, 40};
	std::thread second{wait, 41};
	first.join();
	second.join();
	require(registry.expiry_records() == 1,
		"two waiters recorded one participant at one epoch twice");
	stuck.engine_epoch.store(2, std::memory_order_release);
	registry.leave(stuck);
}

/* A participant that leaves the registry takes its record with it. */
void test_leave_drops_the_record_of_a_stuck_participant()
{
	using namespace std::chrono_literals;
	CodePageRegistry registry{{.slice = 10ms, .slices = 2}};
	const NoLateLeave no_late_leave{registry};
	CodePageRegistry::Participant stuck;
	registry.join(stuck);
	stuck.engine_epoch.store(1, std::memory_order_release);
	const auto result = registry.add(page_at(32), nullptr);
	require(!result.settled && registry.expiry_records() == 1,
		"the stuck participant was not remembered");
	stuck.engine_epoch.store(2, std::memory_order_release);
	registry.leave(stuck);
	require(registry.expiry_records() == 0,
		"a participant that left the registry kept its record");
}

/* The bytes the process's live allocations hold. A sanitizer replaces the
 * allocator, and the malloc zone statistics it reports then measure something
 * else: AddressSanitizer's count every byte ever allocated, freed or not, and
 * ThreadSanitizer's do not move at all. Its own count of live bytes answers
 * instead. */
std::size_t heap_in_use() noexcept
{
#if defined(UML_TEST_SANITIZER_ALLOCATOR)
	return __sanitizer_get_current_allocated_bytes();
#else
	malloc_statistics_t statistics{};
	malloc_zone_statistics(nullptr, &statistics);
	return statistics.size_in_use;
#endif
}

/* Executors of one address space share its registry, and a guest process
 * never reuses a generation. The registry of an address space nobody runs any
 * more must be released in full, or a guest that runs many short processes
 * grows the host without bound.
 */
void test_acquired_registries_are_released()
{
	constexpr std::uint64_t first = std::uint64_t{1} << 40U;
	constexpr unsigned generations = 4096;
	{
		const auto held = CodePageRegistry::acquire(first);
		require(CodePageRegistry::acquire(first) == held,
			"two holders of a generation got different registries");
	}
	/* Warm the allocator's size classes before measuring. */
	for (unsigned index = 1; index <= 64; ++index)
		(void)CodePageRegistry::acquire(first + index);
	const auto before = heap_in_use();
	for (unsigned index = 0; index < generations; ++index)
		(void)CodePageRegistry::acquire(first + 65 + index);
	const auto after = heap_in_use();
	/* A retained registry costs more than 4 KiB. */
	require(after < before + (std::size_t{1} << 20U),
		"registries of released generations stayed allocated");
}

} // namespace

int main()
{
	test_membership_is_counted();
	test_membership_is_exact_behind_a_shared_bit();
	test_membership_retires_write_translations();
	test_participant_retires_its_own_translations();
	test_concurrent_holders_stay_exact();
	test_acquired_registries_are_released();
	test_find_shares_the_registry_without_making_one();
	test_leave_inside_the_engine_is_counted();
	test_waiter_does_not_read_a_member_that_was_freed();
	test_waiter_ignores_a_new_participant_at_a_departed_address(false);
	test_waiter_ignores_a_new_participant_at_a_departed_address(true);
	test_a_second_registration_waits_for_the_first();
	test_a_stuck_participant_is_waited_out_once();
	test_concurrent_waiters_record_a_stuck_participant_once();
	test_leave_drops_the_record_of_a_stuck_participant();
	return 0;
}
