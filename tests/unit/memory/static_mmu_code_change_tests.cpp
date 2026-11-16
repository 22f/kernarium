// SPDX-License-Identifier: GPL-2.0-or-later
/* What the static MMU publishes as a code change. A translator keeps code for
 * a page it could fetch from, a guest-code page that grants execute, and a
 * change to the mapping leaves such a translation wrong only when the page
 * stops being executable or holds other bytes: another backing, or another
 * place in it. The tests hold both halves of that contract. A change that
 * keeps execute and the bytes (a fork write-protecting a page, an mprotect
 * round trip) publishes no code change, which is what keeps a forking process
 * from retranslating its working set; every change that can leave a
 * translation wrong publishes a range covering exactly the pages it can, so a
 * reader that retires only those ranges runs no stale code. */
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"
#include "allocation_fault.hpp"
#include "domain_registry_testing.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <vector>

namespace {

using uml::testing::ScratchBacking;
using uml::testing::require;

constexpr std::string_view backing_name = "uml-static-mmu-code-XXXXXX";
constexpr unsigned long page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr int rwx = PROT_READ | PROT_WRITE | PROT_EXEC;
constexpr int rx = PROT_READ | PROT_EXEC;
constexpr int rw = PROT_READ | PROT_WRITE;

struct Range final {
	std::uint64_t address;
	std::uint64_t length;
};

/* One generation, observed as a vCPU's engine does: through an execution
 * context, from the sequences the engine last consumed. */
class Observer final {
public:
	explicit Observer(unsigned long generation)
		: generation_{generation},
		  context_{darwin_uml_static_vm_execution_context_create(generation)}
	{
		require(context_ != nullptr);
		consume();
	}

	~Observer()
	{
		darwin_uml_static_vm_execution_context_destroy(context_);
	}

	Observer(const Observer &) = delete;
	Observer &operator=(const Observer &) = delete;

	/* Takes what changed since the last call: the mapping changes (-1 when
	 * the history cannot say) and the code ranges (-1 likewise). */
	void consume()
	{
		const auto now = darwin_uml_static_vm_execution_context_sequences(
			context_);
		std::array<darwin_uml_static_code_change, 64> coded{};
		std::uint64_t current = now.code_changes;
		const int count =
			darwin_uml_static_vm_execution_context_code_change_ranges(
				context_, code_seen_, coded.data(), coded.size(), &current);
		code_untracked = count < 0;
		code.clear();
		for (int index = 0; index < count; ++index)
			code.push_back({coded[index].address, coded[index].length});
		code_seen_ = current;

		std::array<darwin_uml_static_mapping_change, 64> mapped{};
		std::uint64_t mapping_current = now.mapping_changes;
		const int mapping_count = darwin_uml_static_vm_mapping_changes(
			generation_, mapping_seen_, mapped.data(), mapped.size(),
			&mapping_current);
		mapping_untracked = mapping_count < 0;
		mapping.clear();
		for (int index = 0; index < mapping_count; ++index)
			mapping.push_back({mapped[index].address, mapped[index].length});
		mapping_seen_ = mapping_current;
	}

	std::vector<Range> code;
	std::vector<Range> mapping;
	bool code_untracked{};
	bool mapping_untracked{};

private:
	unsigned long generation_;
	void *context_;
	std::uint64_t code_seen_{};
	std::uint64_t mapping_seen_{};
};

void map(unsigned long generation, unsigned long address, ScratchBacking &backing,
	unsigned long offset, unsigned long pages, int protection, int code)
{
	require(darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(address), backing.descriptor(), offset,
		pages * page, protection, code, 1) == 0);
}

void protect(unsigned long address, unsigned long pages, int protection)
{
	require(darwin_uml_static_vm_protect(reinterpret_cast<void *>(address),
		pages * page, protection) == 0);
}

void unmap(unsigned long generation, unsigned long address, unsigned long pages)
{
	require(darwin_uml_static_vm_unmap_generation(generation,
		reinterpret_cast<void *>(address), pages * page) == 0);
}

/* The change published exactly this code range, or none when `pages` is 0. */
void require_code(const Observer &seen, unsigned long address,
	unsigned long pages)
{
	if (pages == 0) {
		require(!seen.code_untracked && seen.code.empty());
		return;
	}
	require(!seen.code_untracked && seen.code.size() == 1 &&
		seen.code[0].address == address && seen.code[0].length == pages * page);
}

/* Fork write-protects the parent's pages and the parent writes them again
 * once the copy is broken or the child is gone: protection moves, the bytes
 * and execute stay, and no translation is wrong. Every kind of mapping call
 * says so, and the TLB-bound mapping history still names the pages. */
void test_a_protection_change_that_keeps_execute_and_bytes_changes_no_code()
{
	constexpr unsigned long generation = 501;
	constexpr unsigned long base = 0x100'0000;
	ScratchBacking backing{backing_name, 4 * page};
	map(generation, base, backing, 0, 4, rwx, 1);
	require(darwin_uml_static_vm_activate(generation) == 0);
	Observer seen{generation};

	protect(base, 4, rx);
	seen.consume();
	require_code(seen, 0, 0);
	require(!seen.mapping_untracked && seen.mapping.size() == 1 &&
		seen.mapping[0].address == base && seen.mapping[0].length == 4 * page);

	protect(base + page, 2, rwx);
	seen.consume();
	require_code(seen, 0, 0);
	require(seen.mapping.size() == 1);

	/* Through a mapping call over the same pages: the same backing and
	 * offsets, only the protection moves. */
	map(generation, base, backing, 0, 4, rx, 1);
	seen.consume();
	require_code(seen, 0, 0);
	require(seen.mapping.size() == 1);

	/* A batch of them, as a fork's walker publishes. */
	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	map(generation, base, backing, 0, 2, rwx, 1);
	map(generation, base + 2 * page, backing, 2 * page, 2, rwx, 1);
	require(darwin_uml_static_vm_batch_end() == 0);
	seen.consume();
	require_code(seen, 0, 0);
	require(seen.mapping.size() == 2);

	/* Whether the guest may write does not change a translation either. */
	require(darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(base), backing.descriptor(), 0, 4 * page,
		rwx, 1, 0) == 0);
	seen.consume();
	require_code(seen, 0, 0);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* A page that stops being executable can no longer be translated from, and
 * the translations it had go: exactly the pages that lose execute. */
void test_losing_execute_publishes_the_pages_that_lose_it()
{
	constexpr unsigned long generation = 502;
	constexpr unsigned long base = 0x110'0000;
	ScratchBacking backing{backing_name, 4 * page};
	/* Two executable pages, then two that never could be translated from. */
	map(generation, base, backing, 0, 2, rwx, 1);
	map(generation, base + 2 * page, backing, 2 * page, 1, rw, 1);
	map(generation, base + 3 * page, backing, 3 * page, 1, rwx, 0);
	require(darwin_uml_static_vm_activate(generation) == 0);
	Observer seen{generation};

	protect(base, 4, PROT_READ);
	seen.consume();
	require_code(seen, base, 2);
	require(seen.mapping.size() == 1);

	/* Gaining it again was never a change a translation could be wrong
	 * about: none exists for the page. */
	protect(base, 4, rwx);
	seen.consume();
	require_code(seen, 0, 0);

	/* A round trip through no access at all, page by page. */
	protect(base + page, 1, PROT_NONE);
	seen.consume();
	require_code(seen, base + page, 1);
	protect(base + page, 1, rwx);
	seen.consume();
	require_code(seen, 0, 0);

	/* A mapping call that drops execute names the same pages. */
	map(generation, base, backing, 0, 2, rw, 1);
	seen.consume();
	require_code(seen, base, 2);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* The bytes of an executable page change by remapping it: to another place
 * in its backing, to another backing, or to a page that is no longer guest
 * code. Each publishes the page. */
void test_remapping_an_executable_page_publishes_it()
{
	constexpr unsigned long generation = 503;
	constexpr unsigned long base = 0x120'0000;
	ScratchBacking backing{backing_name, 8 * page};
	ScratchBacking other{backing_name, 8 * page};
	map(generation, base, backing, 0, 4, rwx, 1);
	require(darwin_uml_static_vm_activate(generation) == 0);
	Observer seen{generation};

	/* Copy-on-write breaks by giving the page another frame. */
	map(generation, base + page, backing, 5 * page, 1, rwx, 1);
	seen.consume();
	require_code(seen, base + page, 1);

	/* Another backing at the same offsets. */
	map(generation, base + 2 * page, other, 2 * page, 1, rwx, 1);
	seen.consume();
	require_code(seen, base + 2 * page, 1);

	/* Still guest code and still executable, in the place it was: nothing. */
	map(generation, base + 2 * page, other, 2 * page, 1, rx, 1);
	seen.consume();
	require_code(seen, 0, 0);

	/* It stops being guest code. */
	map(generation, base + 3 * page, backing, 3 * page, 1, rwx, 0);
	seen.consume();
	require_code(seen, base + 3 * page, 1);
	/* And starts again, which no translation was waiting for. */
	map(generation, base + 3 * page, backing, 3 * page, 1, rwx, 1);
	seen.consume();
	require_code(seen, 0, 0);

	/* A mapping that spans pages of both kinds publishes the span of those
	 * that change, no wider. */
	map(generation, base, backing, 4 * page, 4, rwx, 1);
	seen.consume();
	require(!seen.code_untracked && seen.code.size() == 1);
	require(seen.code[0].address == base && seen.code[0].length == 4 * page);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* A page that was not mapped has no translation to retire, so mapping one
 * publishes nothing (demand paging stays free of code changes), and removing
 * an executable one publishes it. */
void test_mapping_and_unmapping_pages()
{
	constexpr unsigned long generation = 504;
	constexpr unsigned long base = 0x130'0000;
	ScratchBacking backing{backing_name, 4 * page};
	require(darwin_uml_static_vm_activate(generation) == 0);
	Observer seen{generation};

	map(generation, base, backing, 0, 2, rwx, 1);
	map(generation, base + 2 * page, backing, 2 * page, 1, rw, 1);
	map(generation, base + 3 * page, backing, 3 * page, 1, rwx, 0);
	seen.consume();
	require_code(seen, 0, 0);
	require(seen.mapping.size() == 3);

	/* Pages that held no translations and an absent range. */
	unmap(generation, base + 2 * page, 2);
	unmap(generation, base + 8 * page, 4);
	seen.consume();
	require_code(seen, 0, 0);

	unmap(generation, base, 3);
	seen.consume();
	require_code(seen, base, 2);

	/* The same through a batch: a page the batch maps and unmaps again
	 * leaves nothing to retire, one it unmaps does. */
	map(generation, base, backing, 0, 2, rwx, 1);
	seen.consume();
	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	map(generation, base + 4 * page, backing, 0, 1, rwx, 1);
	unmap(generation, base + 4 * page, 1);
	unmap(generation, base + page, 1);
	require(darwin_uml_static_vm_batch_end() == 0);
	seen.consume();
	require_code(seen, base + page, 1);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* exit_mmap() unmaps a task's whole user range in one call, 64 TiB or more.
 * The cost follows the pages mapped, not the range: a walk of every page of
 * it takes half a minute per exit. The pages inside go, those outside stay,
 * and the code range spans the translatable pages that went. */
void test_unmapping_a_sparse_user_range_costs_by_the_pages_mapped()
{
	constexpr unsigned long generation = 509;
	constexpr unsigned long below = 0x140'0000;
	constexpr unsigned long range_start = DARWIN_UML_NATIVE_ADDRESS_END;
	constexpr unsigned long range_end = DARWIN_UML_GUEST_ADDRESS_LIMIT;
	constexpr unsigned long inside = range_start + page;
	constexpr unsigned long above = range_end / 2;
	ScratchBacking backing{backing_name, 4 * page};
	map(generation, below, backing, 0, 1, rwx, 1);
	map(generation, inside, backing, page, 1, rwx, 1);
	map(generation, above, backing, 2 * page, 1, rx, 1);
	require(darwin_uml_static_vm_activate(generation) == 0);
	Observer seen{generation};

	const auto started = std::chrono::steady_clock::now();
	unmap(generation, range_start, (range_end - range_start) / page);
	require(std::chrono::steady_clock::now() - started <
		std::chrono::seconds{5});
	seen.consume();
	require_code(seen, inside, (above + page - inside) / page);

	/* What was below stays mapped, and what was inside is gone. */
	unmap(generation, inside, 1);
	unmap(generation, above, 1);
	seen.consume();
	require_code(seen, 0, 0);
	unmap(generation, below, 1);
	seen.consume();
	require_code(seen, below, 1);

	/* An empty generation costs the same: nothing to find. */
	const auto empty_started = std::chrono::steady_clock::now();
	unmap(generation, range_start, (range_end - range_start) / page);
	require(std::chrono::steady_clock::now() - empty_started <
		std::chrono::seconds{5});
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* A batch judges each page against the mapping it replaces, once, and a
 * change that does not touch a translation costs a reader nothing. */
void test_a_batch_publishes_what_it_changes()
{
	constexpr unsigned long generation = 505;
	constexpr unsigned long base = 0x140'0000;
	ScratchBacking backing{backing_name, 8 * page};
	map(generation, base, backing, 0, 8, rwx, 1);
	require(darwin_uml_static_vm_activate(generation) == 0);
	Observer seen{generation};

	/* Write-protecting everything, then moving page 3 to another frame. */
	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	map(generation, base, backing, 0, 8, rx, 1);
	map(generation, base + 3 * page, backing, 3 * page + 4 * page, 1, rx, 1);
	require(darwin_uml_static_vm_batch_end() == 0);
	seen.consume();
	require_code(seen, base + 3 * page, 1);

	/* More changed pages than a batch publishes ranges for: sixteen
	 * operations each remapping one page is as many ranges as it carries,
	 * a seventeenth leaves the history unable to say. */
	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	for (unsigned long index = 0; index < 8; ++index)
		map(generation, base + index * page, backing,
			((index + 1) % 8) * page, 1, rx, 1);
	for (unsigned long index = 0; index < 8; ++index)
		map(generation, base + index * page, backing,
			((index + 2) % 8) * page, 1, rx, 1);
	map(generation, base, backing, 3 * page, 1, rx, 1);
	require(darwin_uml_static_vm_batch_end() == 0);
	seen.consume();
	require(seen.code_untracked);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* A flush drops every mapping at once and names the whole address space,
 * because a reader that kept a translation could not tell which pages
 * Linux dropped for good. */
void test_a_flush_publishes_the_whole_address_space()
{
	constexpr unsigned long generation = 506;
	constexpr unsigned long base = 0x150'0000;
	ScratchBacking backing{backing_name, 2 * page};
	map(generation, base, backing, 0, 2, rwx, 1);
	Observer seen{generation};

	require(darwin_uml_static_vm_flush_generation(generation) == 0);
	seen.consume();
	require(!seen.code_untracked && seen.code.size() == 1);
	require(seen.code[0].length == 0);
	require(seen.mapping_untracked);

	/* Mapping again after the flush is a first mapping. */
	map(generation, base, backing, 0, 2, rwx, 1);
	seen.consume();
	require_code(seen, 0, 0);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* The page a uaccess found mapped to other backing than the kernel's PTE is
 * discarded, and its translations with it. */
void test_discarding_a_stale_page_publishes_it()
{
	constexpr unsigned long generation = 507;
	constexpr unsigned long base = 0x160'0000;
	ScratchBacking backing{backing_name, 2 * page};
	map(generation, base, backing, 0, 2, rwx, 1);
	require(darwin_uml_static_vm_activate(generation) == 0);
	Observer seen{generation};

	/* The page already names this backing and offset. */
	require(darwin_uml_static_vm_discard_if_stale(
		base, backing.descriptor(), 0) == 0);
	seen.consume();
	require_code(seen, 0, 0);
	require(seen.mapping.empty());

	require(darwin_uml_static_vm_discard_if_stale(
		base, backing.descriptor(), page) == 0);
	seen.consume();
	require_code(seen, base, 1);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* The stamp of a page certifies the bytes a translation was validated
 * against, and a store into a page that is writable but not executable moves
 * nothing, because no translator holds such a page. A page that leaves the
 * translatable state therefore moves its own stamp with the publication, and
 * only then: a change that keeps execute and the bytes leaves it alone. */
void test_a_published_page_moves_its_stamp()
{
	constexpr unsigned long generation = 508;
	constexpr unsigned long base = 0x170'0000;
	ScratchBacking backing{backing_name, 8 * page};
	map(generation, base, backing, 0, 6, rwx, 1);
	require(darwin_uml_static_vm_activate(generation) == 0);

	std::array<void *, 6> handles{};
	std::array<darwin_uml_static_code_page_stamp, 6> stamps{};
	for (unsigned long index = 0; index < handles.size(); ++index)
		require(darwin_uml_static_vm_code_page(generation,
			base + index * page, &handles[index], &stamps[index]) ==
			DARWIN_UML_STATIC_TRANSLATED);
	const auto moved = [&](unsigned long index) {
		darwin_uml_static_code_page_stamp now{};
		require(darwin_uml_static_vm_code_page_stamp(
			handles[index], &now) == 0);
		return now.version != stamps[index].version;
	};

	/* Keeping execute and the bytes: no stamp moves. */
	protect(base, 6, rx);
	protect(base, 6, rwx);
	for (unsigned long index = 0; index < handles.size(); ++index)
		require(!moved(index));

	/* Losing execute moves the pages that lose it, not their neighbours. */
	protect(base + page, 2, rw);
	require(!moved(0) && moved(1) && moved(2) && !moved(3));
	protect(base + page, 2, rwx);

	/* So does unmapping, and remapping to other bytes. */
	unmap(generation, base + 3 * page, 1);
	require(moved(3) && !moved(4));
	map(generation, base + 4 * page, backing, 7 * page, 1, rwx, 1);
	require(moved(4) && !moved(5));

	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* The kernel's immutable fact travels in bit 1 of `guest_code` and means
 * something only for guest code (bit 0). A caller that passes 0 or 1 never
 * sets it, and 3 does. The fact belongs to the mapping: changing it replaces
 * the mapping, which a translator sees as a mapping change, but it changes
 * neither the bytes nor the right to execute them, so it publishes no code
 * change and moves no stamp. A replay of the same fact changes nothing. */
void test_the_immutable_fact_replaces_a_mapping_without_a_code_change()
{
	constexpr unsigned long generation = 509;
	constexpr unsigned long base = 0x180'0000;
	ScratchBacking backing{backing_name, 8 * page};
	map(generation, base, backing, 0, 4, rwx, 1);
	require(darwin_uml_static_vm_activate(generation) == 0);
	Observer seen{generation};

	void *handle = nullptr;
	darwin_uml_static_code_page_stamp before{};
	require(darwin_uml_static_vm_code_page(generation, base + page, &handle,
		&before) == DARWIN_UML_STATIC_TRANSLATED);
	const auto moved = [&] {
		darwin_uml_static_code_page_stamp now{};
		require(darwin_uml_static_vm_code_page_stamp(handle, &now) == 0);
		return now.version != before.version || now.identity != before.identity;
	};

	/* The fact on for one page. */
	map(generation, base + page, backing, page, 1, rwx, 3);
	seen.consume();
	require_code(seen, 0, 0);
	require(!seen.mapping_untracked && seen.mapping.size() == 1 &&
		seen.mapping[0].address == base + page &&
		seen.mapping[0].length == page);
	require(!moved());

	/* Again: the same mapping, nothing to publish. */
	map(generation, base + page, backing, page, 1, rwx, 3);
	seen.consume();
	require_code(seen, 0, 0);
	require(seen.mapping.empty());

	/* Bit 1 without bit 0 is guest code and never immutable; as it is the
	 * same mapping as the unflagged one, it publishes nothing for the pages
	 * that are not flagged. */
	map(generation, base + 2 * page, backing, 2 * page, 1, rwx, 2);
	seen.consume();
	require_code(seen, 0, 0);
	require(seen.mapping.empty());

	/* A fact that goes off again replaces the mapping again, and so does
	 * the protection round trip of a flagged page not at all. */
	protect(base + page, 1, rx);
	protect(base + page, 1, rwx);
	map(generation, base + page, backing, page, 1, rwx, 1);
	seen.consume();
	require_code(seen, 0, 0);
	require(!seen.mapping_untracked && !seen.mapping.empty());
	require(!moved());

	/* Not guest code, flagged or not: a page that is not code is not made
	 * code by the fact. */
	map(generation, base + 3 * page, backing, 3 * page, 1, rwx, 0);
	seen.consume();
	require_code(seen, base + 3 * page, 1);
	void *plain = nullptr;
	darwin_uml_static_code_page_stamp unused{};
	require(darwin_uml_static_vm_code_page(generation, base + 3 * page,
		&plain, &unused) != DARWIN_UML_STATIC_TRANSLATED);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* Two mappings of one frame that disagree on the fact still name one
 * physical-page record: the stamp and the code history are the frame's, the
 * fact is the mapping's. */
void test_mappings_of_one_frame_that_disagree_share_its_record()
{
	constexpr unsigned long generation = 510;
	constexpr unsigned long base = 0x190'0000;
	ScratchBacking backing{backing_name, 4 * page};
	map(generation, base, backing, page, 1, rwx, 1);
	map(generation, base + 4 * page, backing, page, 1, rwx, 3);
	map(generation, base + 8 * page, backing, page, 1, rx, 3);
	require(darwin_uml_static_vm_activate(generation) == 0);

	std::array<void *, 3> handles{};
	std::array<darwin_uml_static_code_page_stamp, 3> stamps{};
	const std::array<unsigned long, 3> addresses{base, base + 4 * page,
		base + 8 * page};
	for (std::size_t index = 0; index < handles.size(); ++index)
		require(darwin_uml_static_vm_code_page(generation, addresses[index],
			&handles[index], &stamps[index]) == DARWIN_UML_STATIC_TRANSLATED);
	for (std::size_t index = 1; index < handles.size(); ++index) {
		require(handles[index] == handles[0]);
		require(stamps[index].identity == stamps[0].identity &&
			stamps[index].version == stamps[0].version);
	}

	/* One of them leaving does not take the frame's record from the others. */
	unmap(generation, base + 4 * page, 1);
	void *remaining = nullptr;
	darwin_uml_static_code_page_stamp stamp{};
	require(darwin_uml_static_vm_code_page(generation, base, &remaining,
		&stamp) == DARWIN_UML_STATIC_TRANSLATED);
	require(remaining == handles[0] && stamp.identity == stamps[0].identity);
	require(darwin_uml_static_vm_code_page(generation, base + 8 * page,
		&remaining, &stamp) == DARWIN_UML_STATIC_TRANSLATED);
	require(remaining == handles[0] && stamp.identity == stamps[0].identity);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* A page's stamp carries the fact of the mapping it was asked through, not
 * of the frame. A stamp read by handle names the frame and so never carries
 * the fact; the epoch moves at a publication and with it the version. */
void test_a_stamp_carries_the_epoch_and_the_mapping_fact()
{
	constexpr unsigned long generation = 511;
	constexpr unsigned long base = 0x1a0'0000;
	ScratchBacking backing{backing_name, 4 * page};
	map(generation, base, backing, 0, 1, rwx, 3);
	map(generation, base + page, backing, 0, 1, rwx, 1);
	map(generation, base + 2 * page, backing, page, 1, rx, 3);
	require(darwin_uml_static_vm_activate(generation) == 0);

	std::array<void *, 3> handles{};
	std::array<darwin_uml_static_code_page_stamp, 3> stamps{};
	for (std::size_t index = 0; index < handles.size(); ++index)
		require(darwin_uml_static_vm_code_page(generation,
			base + index * page, &handles[index], &stamps[index]) ==
			DARWIN_UML_STATIC_TRANSLATED);
	require((stamps[0].flags & DARWIN_UML_STATIC_STAMP_IMMUTABLE) != 0);
	require((stamps[1].flags & DARWIN_UML_STATIC_STAMP_IMMUTABLE) == 0);
	require((stamps[2].flags & DARWIN_UML_STATIC_STAMP_IMMUTABLE) != 0);
	for (const auto &stamp : stamps)
		require((stamp.flags & DARWIN_UML_STATIC_STAMP_NEVER_SHARE) == 0);
	require(handles[0] == handles[1] && handles[0] != handles[2]);
	require(stamps[0].epoch == stamps[1].epoch);

	/* By handle: the frame's own fields, and not the mapping's fact. */
	darwin_uml_static_code_page_stamp by_handle{};
	require(darwin_uml_static_vm_code_page_stamp(handles[0], &by_handle) == 0);
	require(by_handle.identity == stamps[0].identity &&
		by_handle.version == stamps[0].version &&
		by_handle.epoch == stamps[0].epoch && by_handle.flags == 0);

	/* A publication through the handle moves the version and the epoch of
	 * that frame only. */
	darwin_uml_static_vm_invalidate_code_page(handles[0]);
	require(darwin_uml_static_vm_code_page_stamp(handles[1], &by_handle) == 0);
	require(by_handle.version == stamps[0].version + 1 &&
		by_handle.epoch == stamps[0].epoch + 1);
	require(darwin_uml_static_vm_code_page_stamp(handles[2], &by_handle) == 0);
	require(by_handle.version == stamps[2].version &&
		by_handle.epoch == stamps[2].epoch);

	/* So does a copy into the page that publishes (a ptrace or
	 * /proc/pid/mem store, here as the bridge spells it). */
	const std::uint32_t word = 0xd503'201fU;
	require(darwin_uml_static_vm_copy_from_buffer(base, &word, sizeof word) ==
		0);
	require(darwin_uml_static_vm_code_page_stamp(handles[0], &by_handle) == 0);
	require(by_handle.version == stamps[0].version + 2 &&
		by_handle.epoch == stamps[0].epoch + 2);

	/* Flipping the fact on a mapping changes what a full query says. */
	map(generation, base + page, backing, 0, 1, rwx, 3);
	darwin_uml_static_code_page_stamp flipped{};
	void *again = nullptr;
	require(darwin_uml_static_vm_code_page(generation, base + page, &again,
		&flipped) == DARWIN_UML_STATIC_TRANSLATED);
	require(again == handles[0] &&
		(flipped.flags & DARWIN_UML_STATIC_STAMP_IMMUTABLE) != 0);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* A kernel write into a guest-code page, the bytes a 4 KiB uaccess copies
 * back from the kernel's alias, lands either way. It publishes no code change
 * until code whose fetch is coherent with every store (x86) is about to run:
 * arm64 code sees the write only after its own instruction cache maintenance,
 * which publishes it. From then on it publishes the page and moves its stamp,
 * as a plain copy always does. */
void test_a_kernel_write_publishes_once_x86_code_may_run()
{
	constexpr unsigned long generation = 509;
	constexpr unsigned long base = 0x180'0000;
	ScratchBacking backing{backing_name, page};
	map(generation, base, backing, 0, 1, rwx, 1);
	require(darwin_uml_static_vm_activate(generation) == 0);
	Observer seen{generation};
	void *handle = nullptr;
	darwin_uml_static_code_page_stamp stamp{};
	require(darwin_uml_static_vm_code_page(generation, base, &handle,
		&stamp) == DARWIN_UML_STATIC_TRANSLATED);
	const auto moved = [&] {
		darwin_uml_static_code_page_stamp now{};
		require(darwin_uml_static_vm_code_page_stamp(handle, &now) == 0);
		const bool changed = now.version != stamp.version;
		stamp = now;
		return changed;
	};
	const auto holds = [&](unsigned long address, std::uint32_t expected) {
		void *host = nullptr;
		require(darwin_uml_static_vm_translate(generation, address,
			DARWIN_UML_STATIC_READ, &host) ==
			DARWIN_UML_STATIC_TRANSLATED);
		std::uint32_t value{};
		std::memcpy(&value, host, sizeof value);
		return value == expected;
	};

	const std::uint32_t first = 0xd503'201fU;
	require(darwin_uml_static_vm_copy_kernel_write(base + 8, &first,
		sizeof first) == 0);
	require(holds(base + 8, first));
	seen.consume();
	require_code(seen, 0, 0);
	require(!moved());

	/* A plain copy publishes whatever runs. */
	const std::uint32_t second = 0xd503'3fdfU;
	require(darwin_uml_static_vm_copy_from_buffer(base + 16, &second,
		sizeof second) == 0);
	require(holds(base + 16, second));
	seen.consume();
	require_code(seen, base, 1);
	require(moved());

	darwin_uml_static_vm_require_coherent_kernel_code_writes();
	darwin_uml_static_vm_require_coherent_kernel_code_writes();
	const std::uint32_t third = 0xd65f'03c0U;
	require(darwin_uml_static_vm_copy_kernel_write(base + 8, &third,
		sizeof third) == 0);
	require(holds(base + 8, third));
	seen.consume();
	require_code(seen, base, 1);
	require(moved());
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

} // namespace

int main()
{
	test_a_protection_change_that_keeps_execute_and_bytes_changes_no_code();
	test_losing_execute_publishes_the_pages_that_lose_it();
	test_remapping_an_executable_page_publishes_it();
	test_mapping_and_unmapping_pages();
	test_unmapping_a_sparse_user_range_costs_by_the_pages_mapped();
	test_a_batch_publishes_what_it_changes();
	test_a_flush_publishes_the_whole_address_space();
	test_discarding_a_stale_page_publishes_it();
	test_a_published_page_moves_its_stamp();
	test_the_immutable_fact_replaces_a_mapping_without_a_code_change();
	test_mappings_of_one_frame_that_disagree_share_its_record();
	test_a_stamp_carries_the_epoch_and_the_mapping_fact();
	test_a_kernel_write_publishes_once_x86_code_may_run();
	/* Every generation of every row is gone: no record counts a mapping. */
	require(uml::memory::testing::audit_executable_counts() == 0);
	require(uml::memory::testing::count_anomalies() == 0);
	std::puts("Static MMU code change tests passed");
	return 0;
}
