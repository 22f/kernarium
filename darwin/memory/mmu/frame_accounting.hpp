// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "uml_darwin_guest.h"

#if DARWIN_UML_X86_64_GUEST

#include "mapped_backing.hpp"
#include "writer_location.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace uml::memory {

struct GenerationState;

/* The frame a fill index word maps, as far as writer accounting needs it. */
struct FrameRef final {
	MappedBacking *backing{};
	std::uint64_t offset{};
	bool writable{};
};

/* Per 4 KiB frame of a backing, the exact locations at which a generation's
 * fill index maps it writable. A translator that registers a frame needs
 * them to retire the write translations that may predate its registration.
 *
 * A location exists for exactly as long as the writable word it names: a
 * mapping primitive adds it before the word is installed (a location without
 * a word costs a spurious retire at most) and removes it after the word is
 * gone, with execution of the generation quiesced. The four primitives that
 * change a word are `commit_planned_pages`, `protect`, `unmap_locked`'s
 * erase and `clear_mappings`; each calls here.
 *
 * Every change of a frame's writers happens under that frame's stripe lock.
 * Nothing allocates or waits under it: what an addition may need (the index
 * leaf, and for a second writer the frame's code-page record with room for
 * the list) is made before the lock is taken.
 *
 * Compiled only for the x86-64 guest, the one guest whose translators see a
 * store through any alias of a frame at once.
 */
class FrameAccounting final {
public:
	/* Alias ids are 26 bits so that a location fits a frame's word. */
	static constexpr std::uint32_t maximum_alias = (1U << 26) - 1U;

	/* Only a page an entered context can fill is a location: the fill never
	 * serves a page at or above the guest address limit. */
	[[nodiscard]] static constexpr bool tracks(std::uint64_t page) noexcept
	{
		return page < std::uint64_t{DARWIN_UML_GUEST_ADDRESS_LIMIT};
	}

	/* Adds `location` to the writers of the frame at `offset` unless it is
	 * already one; true if this call added it. Throws std::bad_alloc, with
	 * nothing changed, when memory for it cannot be had. A frame whose
	 * accounting is in error ("unknown") records nothing. */
	[[nodiscard]] static bool add_writer(MappedBacking &backing,
		std::uint64_t offset, const WriterLocation &location);
	/* Removes `location`. A removal that finds no such location is an
	 * accounting error: the frame becomes "unknown" and the error is
	 * counted. */
	static void remove_writer(MappedBacking &backing, std::uint64_t offset,
		const WriterLocation &location) noexcept;

	/* Reserves the writer location of a page that becomes `final`. For a
	 * holder of the generation's mapping lock; see WriterReservation. */
	static void reserve(GenerationState &state, std::uint64_t page,
		const FrameRef &final);
	/* Applies the change of one committed page from `before` to `after`:
	 * removes the location of `before` unless the page stays writable to the
	 * same frame. Called after the word changed and before the backing slot
	 * of `before` is given back. Allocates nothing. */
	static void apply(GenerationState &state, std::uint64_t page,
		const FrameRef &before, const FrameRef &after) noexcept;
	/* The location of the writable fill index word `word` of `page`, which
	 * is going away. */
	static void release_word(GenerationState &state, std::uint64_t page,
		std::uintptr_t word) noexcept;

	/* The change of the primitive that reserved took effect or did not: the
	 * locations it added stay, or are taken back. Idempotent. */
	static void commit(GenerationState &state) noexcept;
	static void settle(GenerationState &state) noexcept;

	/* Removes every writer location of the generation, before its words go
	 * (`clear_mappings`). The generation stays usable. */
	static void walk(GenerationState &state) noexcept;
	/* The generation is gone: marks its state walked and gives back its alias
	 * id. Later reservations on it are refused. */
	static void finish(GenerationState &state) noexcept;
	/* The retire walk: the same for a state whose words stay as they are. */
	static void retire(GenerationState &state) noexcept;

	/* What the accounting has done in this process. The counters are always
	 * compiled in and move on cold paths only. `errors` and
	 * `nonempty_state_destructions` must stay 0. */
	struct Diagnostics final {
		/* Removals that found no such location. */
		std::uint64_t errors;
		/* States destroyed while still holding an alias id: their walk did
		 * not run. */
		std::uint64_t nonempty_state_destructions;
		std::uint64_t writer_additions;
		std::uint64_t writer_removals;
		/* Reservations taken back because their change did not complete. */
		std::uint64_t rollbacks;
	};
	[[nodiscard]] static Diagnostics diagnostics() noexcept;
	static void note_nonempty_state_destruction() noexcept;
	/* The state that holds `alias`, or null. */
	[[nodiscard]] static GenerationState *state_of(std::uint32_t alias) noexcept;
#if UML_MEMORY_ALLOCATION_TESTING
	/* The locations of the frame at `offset`; false when its accounting is in
	 * error. For a test that excludes every change. */
	[[nodiscard]] static bool locations_for_testing(MappedBacking &backing,
		std::uint64_t offset, std::vector<WriterLocation> &locations);
#endif
};

/* The locations one mapping change added, so that a change that fails or is
 * abandoned takes back exactly those and no location that was already there.
 * Each primitive that changes words holds the generation's mapping lock
 * exclusively from its first reservation to its commit or settle, so the
 * object is used by one thread at a time. */
class WriterReservation final {
public:
	/* Adds `page`'s location at the frame, remembering it if this call added
	 * it. Throws std::bad_alloc, or when the state is walked. */
	void add(GenerationState &state, MappedBacking &backing,
		std::uint64_t offset, std::uint64_t page);
	/* The change took effect: what was added stays. */
	void commit() noexcept { added_.clear(); }
	/* The change did not: removes what this reservation added. */
	void settle(std::uint32_t alias) noexcept;

private:
	struct Added final {
		MappedBacking *backing;
		std::uint64_t offset;
		std::uint64_t page;
	};

	std::vector<Added> added_;
};

/* Takes back the locations a primitive reserved if it did not commit them. */
class WriterSettler final {
public:
	explicit WriterSettler(GenerationState &state) noexcept : state_{state} {}
	~WriterSettler();
	WriterSettler(const WriterSettler &) = delete;
	WriterSettler &operator=(const WriterSettler &) = delete;

private:
	GenerationState &state_;
};

/* A generation state's part of the accounting. */
struct StateAccounting final {
	/* 0 until the state first reserves a location. */
	std::uint32_t alias{};
	/* Set once the generation is gone; a walked state refuses reservations. */
	bool walked{};
	WriterReservation reservation;

	StateAccounting() = default;
	StateAccounting(const StateAccounting &) = delete;
	StateAccounting &operator=(const StateAccounting &) = delete;
	~StateAccounting();
};

} // namespace uml::memory

#if UML_MEMORY_ALLOCATION_TESTING
#include <memory>
#include <tuple>
#include <utility>

namespace uml::memory::testing {

/* What the writer accounting holds against what the generations' fill
 * indexes say, for the current domain. `equal` is true when every writable
 * word of every live state has exactly its location and no frame holds any
 * other. */
struct FrameWriterAudit final {
	std::size_t state_locations{};
	std::size_t frame_locations{};
	std::size_t unknown_frames{};
	bool equal{};
};

/* One writer location of one frame, for comparing two ways of counting. */
struct FrameWriterSite final {
	const MappedBacking *backing;
	std::uint64_t frame;
	std::uint32_t alias;
	std::uint64_t page;

	[[nodiscard]] bool operator<(const FrameWriterSite &other) const noexcept
	{
		return std::tie(backing, frame, alias, page) <
			std::tie(other.backing, other.frame, other.alias, other.page);
	}
	[[nodiscard]] bool operator==(const FrameWriterSite &) const noexcept = default;
};

[[nodiscard]] FrameWriterAudit audit_frame_writers() noexcept;
/* The live state of `generation` in the current domain; null when there is
 * none. The caller's reference outlives the generation. */
[[nodiscard]] std::shared_ptr<GenerationState> generation_state(
	std::uint64_t generation);
/* The backing that `fd` names in the current domain, if it has one. */
[[nodiscard]] std::shared_ptr<MappedBacking> mapped_backing(int fd);
/* {generation, page} of each writer of the frame at `offset` of the backing
 * `fd` names in the current domain. */
[[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>>
frame_writers(int fd, std::uint64_t offset);

} // namespace uml::memory::testing
#endif

#endif
