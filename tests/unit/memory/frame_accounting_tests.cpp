// SPDX-License-Identifier: GPL-2.0-or-later
/* The x86-64 guest's writer accounting: for each 4 KiB frame of a backing,
 * the exact set of {generation, page} locations whose fill index word maps it
 * writable. A translator that registers a frame later retires the write
 * translations of exactly those locations, so a location missing while its
 * word exists is a stale translation, and one left behind costs only a
 * spurious retire.
 *
 * Every row drives the mapping primitives through the bridge and, after each
 * step, compares the frames' sets with a recount from the generations' fill
 * indexes (the test variant's audit). The diagnostics must show no accounting
 * error and no state destroyed with its locations. */
#include "domain_contract.hpp"
#include "frame_accounting.hpp"
#include "allocation_fault.hpp"
#include "address_space_types.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <sys/mman.h>
#include <thread>
#include <utility>
#include <vector>

namespace {

using uml::memory::FrameAccounting;
using uml::memory::testing::AllocationSite;
using uml::testing::ScratchBacking;
using uml::testing::require;

constexpr unsigned long page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr int rw = PROT_READ | PROT_WRITE;
constexpr int rwx = PROT_READ | PROT_WRITE | PROT_EXEC;
constexpr int ro = PROT_READ;

std::atomic_uint64_t active_domain{};

bool provide_domain(std::uint64_t &domain) noexcept
{
	domain = active_domain.load(std::memory_order_acquire);
	return domain != 0;
}

/* The steps of a row run on a thread of their own, in a domain of their own,
 * so retiring the domain ends everything the row cached. */
class Domain final {
public:
	explicit Domain(std::uint64_t id) : id_{id} {}

	void run(const std::function<void()> &steps)
	{
		std::thread thread{[&] {
			active_domain.store(id_, std::memory_order_release);
			steps();
			active_domain.store(0, std::memory_order_release);
		}};
		thread.join();
	}

	void retire()
	{
		uml::memory::retire_domain(id_);
	}

private:
	std::uint64_t id_;
};

int map(unsigned long generation, unsigned long address, ScratchBacking &backing,
	unsigned long first_page, unsigned long pages, int protection,
	int guest_code = 1)
{
	return darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(address), backing.descriptor(),
		first_page * page, pages * page, protection, guest_code, 1);
}

int protect(unsigned long address, unsigned long pages, int protection)
{
	return darwin_uml_static_vm_protect(
		reinterpret_cast<void *>(address), pages * page, protection);
}

int unmap(unsigned long generation, unsigned long address, unsigned long pages)
{
	return darwin_uml_static_vm_unmap_generation(generation,
		reinterpret_cast<void *>(address), pages * page);
}

/* After every step: the frames' sets are the fill indexes' writable words. */
void audit(const char *step, std::size_t locations)
{
	const auto result = uml::memory::testing::audit_frame_writers();
	if (!result.equal || result.state_locations != locations ||
	    result.frame_locations != locations) {
		std::printf("audit after %s: states %zu, frames %zu, unknown %zu, "
			    "equal %d, expected %zu\n",
			step, result.state_locations, result.frame_locations,
			result.unknown_frames, result.equal ? 1 : 0, locations);
		require(false, "the frames' writers differ from the writable words");
	}
}

/* The generations that write the frame, as sorted pairs. */
std::size_t writers_of(ScratchBacking &backing, unsigned long frame)
{
	return uml::memory::testing::frame_writers(
		backing.descriptor(), frame * page).size();
}

bool writes(ScratchBacking &backing, unsigned long frame,
	unsigned long generation, unsigned long address)
{
	for (const auto &[writer, writer_page] : uml::memory::testing::frame_writers(
		     backing.descriptor(), frame * page))
		if (writer == generation && writer_page == address)
			return true;
	return false;
}

struct Baseline final {
	FrameAccounting::Diagnostics start{FrameAccounting::diagnostics()};

	/* No accounting error and no state left with its locations since. */
	void clean(const char *row) const
	{
		const auto now = FrameAccounting::diagnostics();
		if (now.errors != start.errors ||
		    now.nonempty_state_destructions != start.nonempty_state_destructions) {
			std::printf("%s: errors +%llu, nonempty destructions +%llu\n", row,
				static_cast<unsigned long long>(now.errors - start.errors),
				static_cast<unsigned long long>(
					now.nonempty_state_destructions -
					start.nonempty_state_destructions));
			require(false, "the writer accounting reported an error");
		}
	}
};

/* M1: a frame's writers through one, two and back to one location; a
 * protection that gains and loses write; a page that moves to another frame;
 * overlapping operations of one batch; unmap; flush_generation; destroy; and
 * a retired domain. */
void test_locations_follow_every_primitive()
{
	const Baseline baseline;
	constexpr unsigned long base_a = 0x100'0000;
	constexpr unsigned long base_b = 0x200'0000;
	ScratchBacking backing{"uml-frame-accounting-XXXXXX", 16 * page};
	Domain domain{71'001};
	std::shared_ptr<uml::memory::MappedBacking> kept;
	domain.run([&] {
		audit("start", 0);
		require(map(601, base_a, backing, 0, 4, rw) == 0);
		kept = uml::memory::testing::mapped_backing(backing.descriptor());
		require(kept != nullptr);
		audit("map 601", 4);
		require(writes(backing, 0, 601, base_a));
		require(writers_of(backing, 3) == 1);

		/* A second generation writes frames 0 and 1: two, then one again. */
		require(map(602, base_b, backing, 0, 2, rw) == 0);
		audit("map 602", 6);
		require(writers_of(backing, 0) == 2 && writers_of(backing, 1) == 2);
		require(writes(backing, 0, 602, base_b));
		require(map(602, base_b, backing, 0, 2, ro) == 0);
		audit("602 read only", 4);
		require(writers_of(backing, 0) == 1 && writes(backing, 0, 601, base_a));

		/* Losing and gaining write through protect. */
		require(darwin_uml_static_vm_activate(601) == 0);
		require(protect(base_a, 2, ro) == 0);
		audit("601 loses write", 2);
		require(writers_of(backing, 0) == 0 && writers_of(backing, 2) == 1);
		require(protect(base_a, 2, rw) == 0);
		audit("601 gains write", 4);

		/* A page moves to another frame. */
		require(map(601, base_a, backing, 5, 1, rw) == 0);
		audit("601 moves", 4);
		require(writers_of(backing, 0) == 0 && writes(backing, 5, 601, base_a));

		/* Overlapping operations of one batch: only the last survives. */
		require(darwin_uml_static_vm_batch_begin(601) == 0);
		require(map(601, base_a + page, backing, 6, 1, rw) == 0);
		require(map(601, base_a + page, backing, 7, 1, rw) == 0);
		require(unmap(601, base_a + 2 * page, 1) == 0);
		require(darwin_uml_static_vm_batch_end() == 0);
		audit("batch", 3);
		require(writers_of(backing, 6) == 0 && writes(backing, 7, 601, base_a + page));

		require(unmap(601, base_a, 2) == 0);
		audit("unmap", 1);

		/* flush_generation takes every location of the generation; the
		 * generation stays usable. */
		require(map(602, base_b + 4 * page, backing, 8, 2, rw) == 0);
		audit("602 maps", 3);
		require(darwin_uml_static_vm_flush_generation(602) == 0);
		audit("flush 602", 1);
		const auto flushed = uml::memory::testing::generation_state(602);
		require(flushed != nullptr && !flushed->accounting.walked,
			"a flush walked the state");
		require(map(602, base_b + 4 * page, backing, 8, 1, rw) == 0);
		audit("map after flush", 2);

		/* destroy walks it for good. */
		const auto doomed = uml::memory::testing::generation_state(601);
		require(doomed != nullptr);
		require(darwin_uml_static_vm_destroy(601) == 0);
		audit("destroy 601", 1);
		require(doomed->accounting.walked && doomed->accounting.alias == 0,
			"destroy did not mark the state walked");
		require(uml::memory::testing::generation_state(601) == nullptr);
	});
	/* A retired domain: its states are walked without their words going. */
	domain.run([&] {
		require(map(603, base_a, backing, 12, 2, rw) == 0);
		audit("map 603", 3);
	});
	domain.retire();
	std::vector<uml::memory::WriterLocation> left;
	for (unsigned long frame = 0; frame < 16; ++frame)
		require(FrameAccounting::locations_for_testing(
			*kept, frame * page, left));
	require(left.empty(), "a retired domain left writer locations");
	baseline.clean("M1");
}

/* M11: flush_generation leaves the state usable and later maps account
 * normally; destroy marks it walked, and a reservation on it is refused. */
void test_flush_and_destroy_walk_differently()
{
	const Baseline baseline;
	constexpr unsigned long base = 0x300'0000;
	ScratchBacking backing{"uml-frame-accounting-walk-XXXXXX", 8 * page};
	Domain domain{71'002};
	std::shared_ptr<uml::memory::GenerationState> state;
	std::shared_ptr<uml::memory::MappedBacking> kept;
	domain.run([&] {
		require(map(611, base, backing, 0, 2, rw) == 0);
		kept = uml::memory::testing::mapped_backing(backing.descriptor());
		state = uml::memory::testing::generation_state(611);
		require(state != nullptr && kept != nullptr);
		const auto alias = state->accounting.alias;
		require(alias != 0);
		require(darwin_uml_static_vm_flush_generation(611) == 0);
		require(!state->accounting.walked && state->accounting.alias == alias,
			"a flush marked the state walked");
		audit("flush", 0);
		require(map(611, base, backing, 0, 2, rw) == 0);
		audit("map after flush", 2);
		require(writes(backing, 1, 611, base + page));
		require(darwin_uml_static_vm_destroy(611) == 0);
		audit("destroy", 0);
		require(state->accounting.walked && state->accounting.alias == 0,
			"destroy did not mark the state walked");
	});
	/* The state outlives its generation, as an old reference would hold it. */
	bool refused = false;
	try {
		state->accounting.reservation.add(*state, *kept, 3 * page, base);
	} catch (const std::bad_alloc &) {
		refused = true;
	}
	require(refused, "a walked state took a reservation");
	std::vector<uml::memory::WriterLocation> left;
	require(FrameAccounting::locations_for_testing(*kept, 3 * page, left) &&
		left.empty(), "a refused reservation left a location");
	state.reset();
	domain.retire();
	baseline.clean("M11");
}

/* M12: a page that stays writable to the same frame adds and removes
 * nothing, whatever else about its mapping changes; a change that does not
 * complete takes back exactly the locations it added. */
void test_reservation_is_idempotent_and_abandoned_plans_roll_back()
{
	const Baseline baseline;
	constexpr unsigned long base = 0x400'0000;
	ScratchBacking backing{"uml-frame-accounting-idem-XXXXXX", 8 * page};
	Domain domain{71'003};
	domain.run([&] {
		require(map(621, base, backing, 0, 2, rwx) == 0);
		audit("map", 2);
		const auto before = FrameAccounting::diagnostics();
		/* Execute dropped, then the code flag, then both back: the same
		 * frame stays writable at the same pages. */
		require(map(621, base, backing, 0, 2, rw) == 0);
		require(map(621, base, backing, 0, 2, rw, 0) == 0);
		require(darwin_uml_static_vm_activate(621) == 0);
		require(protect(base, 2, rwx) == 0);
		audit("kept writable", 2);
		const auto after = FrameAccounting::diagnostics();
		require(after.writer_additions == before.writer_additions &&
			after.writer_removals == before.writer_removals,
			"a page that stayed writable to its frame changed its location");

		/* A plan with one page that stays and two that are new, abandoned
		 * by a history allocation that fails after the reservation. */
		const auto rollbacks = FrameAccounting::diagnostics().rollbacks;
		uml::memory::testing::fail_next_allocation(AllocationSite::history);
		require(darwin_uml_static_vm_batch_begin(621) == 0);
		require(map(621, base, backing, 0, 3, rw) == 0);
		const auto rc = darwin_uml_static_vm_batch_end();
		uml::memory::testing::clear_allocation_failure();
		require(rc != 0, "the abandoned plan completed");
		audit("abandoned", 2);
		require(writers_of(backing, 2) == 0 && writes(backing, 0, 621, base));
		require(FrameAccounting::diagnostics().rollbacks == rollbacks + 1,
			"the abandoned plan was not rolled back once");
	});
	domain.retire();
	baseline.clean("M12");
}

/* M8: an allocation that fails anywhere in the reservation of a plan, or of
 * a protect, leaves nothing of it behind. Every allocation point of the
 * reservation fails in turn. */
void test_every_allocation_failure_rolls_back_the_reservation()
{
	const Baseline baseline;
	constexpr unsigned long base = 0x500'0000;
	for (const bool plan : {true, false}) {
		std::size_t points = 0;
		for (std::size_t failing = 0; failing <= points; ++failing) {
			ScratchBacking backing{"uml-frame-accounting-fail-XXXXXX", 8 * page};
			Domain domain{72'000 + failing + (plan ? 0 : 100)};
			const unsigned long generation = 630 + failing + (plan ? 0 : 100);
			domain.run([&] {
				if (plan) {
					/* Two pages already write; a plan keeps them and adds
					 * three more. */
					require(map(generation, base, backing, 0, 2, rw) == 0);
				} else {
					/* Four pages read only; protect makes them writable. */
					require(map(generation, base, backing, 0, 4, ro) == 0);
					require(darwin_uml_static_vm_activate(generation) == 0);
				}
				const std::size_t locations = plan ? 2 : 0;
				audit("before", locations);
				uml::memory::testing::reset_allocation_attempt_counts();
				if (failing != 0)
					uml::memory::testing::fail_allocation_at(
						AllocationSite::writers, failing);
				int rc = 0;
				if (plan) {
					require(darwin_uml_static_vm_batch_begin(generation) == 0);
					require(map(generation, base, backing, 0, 5, rw) == 0);
					rc = darwin_uml_static_vm_batch_end();
				} else {
					rc = protect(base, 4, rw);
				}
				uml::memory::testing::clear_allocation_failure();
				if (failing == 0) {
					require(rc == 0);
					points = uml::memory::testing::allocation_attempt_count(
						AllocationSite::writers);
					require(points != 0, "the reservation made no allocation");
					audit("completed", plan ? 5 : 4);
				} else {
					require(rc != 0, "an allocation failure did not fail the change");
					audit("failed", locations);
					if (plan)
						require(writers_of(backing, 2) == 0 &&
							writers_of(backing, 4) == 0,
							"a failed plan left a location behind");
				}
			});
			domain.retire();
		}
		std::printf("%s: %zu allocation points in the reservation\n",
			plan ? "plan" : "protect", points);
	}

	/* protect, when the history allocation fails after its first scan. */
	ScratchBacking backing{"uml-frame-accounting-hist-XXXXXX", 8 * page};
	Domain domain{72'900};
	domain.run([&] {
		require(map(699, base, backing, 0, 4, ro) == 0);
		require(darwin_uml_static_vm_activate(699) == 0);
		const auto rollbacks = FrameAccounting::diagnostics().rollbacks;
		uml::memory::testing::fail_next_allocation(AllocationSite::history);
		const auto rc = protect(base, 4, rw);
		uml::memory::testing::clear_allocation_failure();
		require(rc != 0, "protect completed without its history");
		audit("protect history failure", 0);
		require(FrameAccounting::diagnostics().rollbacks == rollbacks + 1,
			"protect did not take its reservation back");
		require(protect(base, 4, rw) == 0);
		audit("protect after", 4);
	});
	domain.retire();
	baseline.clean("M8");
}

} // namespace

int main()
{
	uml::memory::set_domain_provider(&provide_domain);
	test_locations_follow_every_primitive();
	test_flush_and_destroy_walk_differently();
	test_reservation_is_idempotent_and_abandoned_plans_roll_back();
	test_every_allocation_failure_rolls_back_the_reservation();
	uml::memory::set_domain_provider(nullptr);
	std::puts("frame accounting tests passed");
	return EXIT_SUCCESS;
}
