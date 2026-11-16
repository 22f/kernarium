// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Which generations hold a translation of a frame of guest code, and what a
 * code synchronization or a kernel write tells them. A translator registers
 * its generation in the frame's holder word before it reads the frame's
 * bytes; a publication takes the word and publishes a whole-space code change
 * in each generation the word named (mmu/code_holders.hpp).
 *
 * The rows:
 *   words        a frame's word fills two slots, then reads "every generation";
 *                a generation that is gone is dropped on the way; a generation
 *                a slot cannot hold registers as every one.
 *   interleaving a reader that has copied the frame's bytes (held there by the
 *                fetch observer) is published to by a writer that stores and
 *                synchronizes the frame meanwhile, from another process.
 *   dead holder  a holder whose generation was destroyed is skipped, and the
 *                publication does not bring its generation back.
 *   own frame    a frame held by its writer alone, or by nobody, is published
 *                with no exchange of its word; one held by another is taken
 *                once, and a second publication finds it empty.
 *   no leaf      a backing that cannot make a leaf of its table registers
 *                nothing, and a publication on it goes to every generation.
 *   overlap      a publisher has taken a frame's word and not yet published;
 *                a second synchronization of the frame meets an empty word
 *                (or a word of its own) and must still reach the first one's
 *                holders, with and without a holder that registered between.
 */
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"
#include "allocation_fault.hpp"
#include "code_holders.hpp"
#include "memory/static_guest_memory.hpp"
#include "memory/static_guest_memory_testing.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <sys/mman.h>
#include <thread>

namespace {

using uml::memory::StaticGuestMemory;
using uml::testing::ScratchBacking;
using uml::testing::require;
namespace holders = uml::memory::code_holders;
namespace memory_testing = uml::memory::testing;

constexpr std::string_view backing_name = "uml-code-holder-XXXXXX";
constexpr unsigned long page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr unsigned long code_address = 0x4a00'0000;
constexpr unsigned long writer_address = 0x4b00'0000;
constexpr std::uint32_t old_word = 0xd280'0020U; // movz x0, #1
constexpr std::uint32_t new_word = 0xd280'0040U; // movz x0, #2
constexpr std::uint32_t third_word = 0xd280'0060U; // movz x0, #3

void map(unsigned long generation, unsigned long address,
	ScratchBacking &backing, int protection, int code)
{
	require(darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(address), backing.descriptor(), 0, page,
		protection, code, (protection & PROT_WRITE) != 0 ? 1 : 0) == 0);
}

void map_code(unsigned long generation, ScratchBacking &backing)
{
	map(generation, code_address, backing, PROT_READ | PROT_EXEC, 1);
}

void destroy(unsigned long generation)
{
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* What a vCPU's fetch does: this generation's memory, registering, reads the
 * instruction at the page's first word. */
std::uint32_t fetch(StaticGuestMemory &memory, unsigned long generation)
{
	memory.set_code_holder_registration(true);
	memory.prepare(generation, darwin_uml_static_vm_changes(generation));
	std::uint32_t word = 0;
	require(memory.fetch(code_address,
		std::as_writable_bytes(std::span{&word, 1})));
	return word;
}

std::uint32_t fetch(unsigned long generation)
{
	auto memory = std::make_unique<StaticGuestMemory>();
	return fetch(*memory, generation);
}

/* The holder word of the frame that `generation` maps at the code address. */
holders::Word &word_of(unsigned long generation)
{
	void *host = nullptr;
	void *holder = nullptr;
	require(darwin_uml_static_vm_translate_execute(generation, code_address,
		&host, &holder) == DARWIN_UML_STATIC_TRANSLATED);
	require(holder != nullptr);
	return *static_cast<holders::Word *>(holder);
}

std::uint64_t code_changes(unsigned long generation)
{
	void *context = darwin_uml_static_vm_execution_context_create(generation);
	require(context != nullptr);
	const auto changes =
		darwin_uml_static_vm_execution_context_sequences(context).code_changes;
	darwin_uml_static_vm_execution_context_destroy(context);
	return changes;
}

/* A guest code synchronization by `writer` at its own view of the frame. */
void synchronize(unsigned long writer)
{
	require(darwin_uml_static_vm_activate(writer) == 0);
	std::atomic_thread_fence(std::memory_order_seq_cst);
	require(darwin_uml_static_vm_synchronize_code(writer_address) == 0);
}

/* A writer's mappings: a writable view of the frame, the way another
 * process holds it. */
void map_writer(unsigned long writer, ScratchBacking &backing)
{
	map(writer, writer_address, backing, PROT_READ | PROT_WRITE, 0);
}

holders::Counters since(const holders::Counters &before)
{
	const auto now = holders::counters();
	return {now.registrations - before.registrations,
		now.overflows - before.overflows, now.exchanges - before.exchanges,
		now.publications - before.publications,
		now.all_publications - before.all_publications,
		now.generations_published - before.generations_published,
		now.leaves - before.leaves,
		now.unknown_backings - before.unknown_backings,
		now.overlap_widenings - before.overlap_widenings};
}

void test_the_word_fills_two_slots_then_reads_every_generation()
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	constexpr unsigned long first = 1001, second = 1002, third = 1003;
	for (const auto generation : {first, second, third})
		map_code(generation, backing);
	const auto before = holders::counters();

	require(fetch(first) == old_word);
	auto &word = word_of(first);
	require(word.load() == holders::join(first, 0));
	/* The same generation again changes nothing. */
	require(fetch(first) == old_word);
	require(word.load() == holders::join(first, 0));
	require(fetch(second) == old_word);
	require(word.load() == holders::join(first, second));
	require(fetch(third) == old_word);
	require(word.load() == holders::all);
	/* Every generation holds, and registers nothing further. */
	require(fetch(first) == old_word);
	require(word.load() == holders::all);

	const auto delta = since(before);
	require(delta.registrations == 3 && delta.overflows == 1);
	/* One leaf for the region, however many frames of it are asked. */
	require(delta.leaves == 1);
	for (const auto generation : {first, second, third})
		destroy(generation);
}

void test_a_generation_that_is_gone_is_dropped_on_the_way_to_every_one()
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	constexpr unsigned long first = 1011, second = 1012, third = 1013;
	for (const auto generation : {first, second, third})
		map_code(generation, backing);
	(void)fetch(first);
	(void)fetch(second);
	auto &word = word_of(second);
	require(word.load() == holders::join(first, second));

	destroy(first);
	(void)fetch(third);
	require(word.load() == holders::join(third, second));
	destroy(second);
	destroy(third);
}

void test_a_generation_a_slot_cannot_hold_registers_as_every_one()
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	constexpr unsigned long wide = holders::max_generation + 5;
	map_code(wide, backing);
	(void)fetch(wide);
	require(word_of(wide).load() == holders::all);
	destroy(wide);
}

/* The reader is held by the fetch observer after it copied the frame's bytes
 * and before it returns: a writer from another process stores new bytes and
 * synchronizes them there. The reader copied the old word, so the
 * synchronization must reach its generation, which registered before it
 * copied. A reader that registers after its copy is not yet a holder when the
 * writer looks, and is never told. */
std::atomic<bool> reader_copied;
std::atomic<bool> writer_done;

void hold_the_reader(std::uint64_t) noexcept
{
	reader_copied.store(true, std::memory_order_release);
	const auto limit =
		std::chrono::steady_clock::now() + std::chrono::seconds{30};
	while (!writer_done.load(std::memory_order_acquire)) {
		if (std::chrono::steady_clock::now() > limit)
			uml::testing::fail("the writer never finished");
		std::this_thread::yield();
	}
}

void test_a_publication_between_the_reader_s_copy_and_its_return_reaches_it()
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	constexpr unsigned long reader = 1021, writer = 1022;
	map_code(reader, backing);
	map_writer(writer, backing);
	require(darwin_uml_static_vm_activate(reader) == 0);
	const auto changes_before = code_changes(reader);

	reader_copied.store(false);
	writer_done.store(false);
	memory_testing::set_fetch_observer(&hold_the_reader);
	std::uint32_t copied = 0;
	std::thread fetching{[&] { copied = fetch(reader); }};
	const auto limit =
		std::chrono::steady_clock::now() + std::chrono::seconds{30};
	while (!reader_copied.load(std::memory_order_acquire)) {
		require(std::chrono::steady_clock::now() < limit,
			"the reader never copied the frame");
		std::this_thread::yield();
	}
	backing.put32(0, new_word);
	synchronize(writer);
	writer_done.store(true, std::memory_order_release);
	fetching.join();
	memory_testing::set_fetch_observer(nullptr);

	require(copied == old_word, "the reader copied after the write");
	require(code_changes(reader) > changes_before,
		"the synchronization did not reach the reader");
	destroy(reader);
	destroy(writer);
}

void test_a_holder_that_is_gone_is_skipped_and_not_brought_back()
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	constexpr unsigned long reader = 1031, other = 1032, writer = 1033;
	map_code(reader, backing);
	map_code(other, backing);
	map_writer(writer, backing);
	(void)fetch(reader);
	(void)fetch(other);
	destroy(reader);
	require(darwin_uml_static_vm_generation_live(reader) == 0);
	const auto others_before = code_changes(other);

	synchronize(writer);
	require(darwin_uml_static_vm_generation_live(reader) == 0,
		"the publication made the dead generation again");
	require(code_changes(other) > others_before);
	destroy(other);
	destroy(writer);
	require(darwin_uml_static_vm_generation_live(reader) == 0);
}

void test_a_frame_held_by_its_writer_alone_is_published_without_an_exchange()
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	constexpr unsigned long writer = 1041, reader = 1042;
	/* The writer also runs the page it writes: a writable and executable
	 * mapping, which is its own translator and the frame's only holder. */
	map(writer, code_address, backing, PROT_READ | PROT_WRITE | PROT_EXEC, 1);
	map(writer, writer_address, backing, PROT_READ | PROT_WRITE, 0);
	map_code(reader, backing);
	(void)fetch(writer);
	auto &word = word_of(writer);
	require(word.load() == holders::join(writer, 0));

	/* The word is empty or the writer's own: nothing is taken. */
	auto before = holders::counters();
	synchronize(writer);
	synchronize(writer);
	require(since(before).exchanges == 0 && since(before).publications == 0);
	require(word.load() == holders::join(writer, 0),
		"the writer's own registration was taken");

	/* Another generation holds it too: the first publication takes the word
	 * and tells the other; the second finds it empty of the other. */
	(void)fetch(reader);
	const auto reader_before = code_changes(reader);
	before = holders::counters();
	synchronize(writer);
	require(since(before).exchanges == 1 && since(before).publications == 1);
	require(since(before).overlap_widenings == 0,
		"a publication that overlapped nothing widened");
	require(code_changes(reader) > reader_before);
	before = holders::counters();
	synchronize(writer);
	require(since(before).exchanges == 0,
		"a second publication took a word that was already taken");
	destroy(writer);
	destroy(reader);
}

void test_a_backing_without_a_leaf_publishes_to_every_generation()
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	constexpr unsigned long reader = 1051, bystander = 1052, writer = 1053;
	map_code(reader, backing);
	map_code(bystander, backing);
	map_writer(writer, backing);
	require(darwin_uml_static_vm_activate(bystander) == 0);
	require(darwin_uml_static_vm_activate(reader) == 0);
	const auto reader_before = code_changes(reader);
	const auto before = holders::counters();

	memory_testing::fail_next_allocation(
		memory_testing::AllocationSite::holder_leaf);
	/* The reader's fetch finds no leaf and cannot make one: it registers
	 * nothing, and the backing is unknown from now on. */
	require(fetch(reader) == old_word);
	memory_testing::clear_allocation_failure();
	require(since(before).unknown_backings == 1);

	backing.put32(0, new_word);
	synchronize(writer);
	require(code_changes(reader) > reader_before,
		"a backing without a table did not publish to the reader");
	require(since(before).all_publications == 1);
	destroy(reader);
	destroy(bystander);
	destroy(writer);
}

/* The first publisher has taken the frame's word, so the reader that held it
 * is no longer named there, and has not yet published to it (the observer
 * holds it at the start of its publication). A second synchronization of the
 * same frame then finds the word empty, or holding only a generation that
 * registered after the take. The reader must hear of the second change before
 * the first publisher goes on: a completed synchronization cannot leave a
 * holder that another publisher consumed unannounced. */
std::atomic<bool> publisher_paused;
std::atomic<bool> publisher_released;
thread_local bool holds_at_publication;

void hold_the_publisher(memory_testing::AllocationSite site) noexcept
{
	if (site != memory_testing::AllocationSite::holder_publication ||
	    !holds_at_publication)
		return;
	publisher_paused.store(true, std::memory_order_release);
	const auto limit =
		std::chrono::steady_clock::now() + std::chrono::seconds{30};
	while (!publisher_released.load(std::memory_order_acquire)) {
		if (std::chrono::steady_clock::now() > limit)
			uml::testing::fail("the held publisher was never released");
		std::this_thread::yield();
	}
}

void two_publishers(bool late_holder, unsigned long base)
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	const unsigned long reader = base, late = base + 1, first = base + 2,
		second = base + 3;
	map_code(reader, backing);
	map_code(late, backing);
	map_writer(first, backing);
	map_writer(second, backing);
	(void)fetch(reader);
	const auto changes_before = code_changes(reader);
	const auto before = holders::counters();

	publisher_paused.store(false);
	publisher_released.store(false);
	memory_testing::set_allocation_observer(&hold_the_publisher);
	std::thread first_writer{[&] {
		holds_at_publication = true;
		backing.put32(0, new_word);
		synchronize(first);
	}};
	const auto limit =
		std::chrono::steady_clock::now() + std::chrono::seconds{30};
	while (!publisher_paused.load(std::memory_order_acquire)) {
		if (std::chrono::steady_clock::now() > limit) {
			publisher_released.store(true);
			first_writer.join();
			uml::testing::fail("the first publisher never took the word");
		}
		std::this_thread::yield();
	}
	if (late_holder)
		(void)fetch(late);
	backing.put32(0, third_word);
	synchronize(second);
	const bool heard = code_changes(reader) > changes_before;
	publisher_released.store(true, std::memory_order_release);
	first_writer.join();
	memory_testing::set_allocation_observer(nullptr);

	require(heard, late_holder ?
		"a publication that took a late holder missed the reader the "
		"first publisher had consumed" :
		"a publication that found the word empty missed the reader the "
		"first publisher had consumed");
	require(since(before).overlap_widenings == 1,
		"the overlap did not widen exactly one publication");
	for (const auto generation : {reader, late, first, second})
		destroy(generation);
}

void test_a_publication_that_overlaps_an_unfinished_one_reaches_its_holders()
{
	two_publishers(false, 1061);
	two_publishers(true, 1071);
}

} // namespace

int main()
{
	test_the_word_fills_two_slots_then_reads_every_generation();
	test_a_generation_that_is_gone_is_dropped_on_the_way_to_every_one();
	test_a_generation_a_slot_cannot_hold_registers_as_every_one();
	test_a_publication_between_the_reader_s_copy_and_its_return_reaches_it();
	test_a_holder_that_is_gone_is_skipped_and_not_brought_back();
	test_a_frame_held_by_its_writer_alone_is_published_without_an_exchange();
	test_a_backing_without_a_leaf_publishes_to_every_generation();
	test_a_publication_that_overlaps_an_unfinished_one_reaches_its_holders();
	std::puts("static-mmu-code-holder: ok");
	return 0;
}
