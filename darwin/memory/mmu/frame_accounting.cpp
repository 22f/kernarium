// SPDX-License-Identifier: GPL-2.0-or-later
#include "frame_accounting.hpp"

#if DARWIN_UML_X86_64_GUEST

#include "address_space_types.hpp"
#include "allocation_fault.hpp"
#include "frame_heap.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>

namespace uml::memory {
namespace {

constexpr std::uint64_t frame_bytes = 4096;
constexpr std::size_t stripe_count = 4096;

/* One lock per frame stripe, process-wide, so that no lock outlives the
 * backing it covers. A frame maps to a stripe by a hash of its backing and
 * its number. */
[[nodiscard]] WordSharedMutex &stripe_for(
	const MappedBacking *backing, std::uint64_t offset) noexcept
{
	static std::array<WordSharedMutex, stripe_count> stripes;
	auto key = (reinterpret_cast<std::uintptr_t>(backing) >> 4U) *
			0x9e3779b97f4a7c15ULL ^
		(offset / frame_bytes) * 0xc2b2ae3d27d4eb4fULL;
	key ^= key >> 29U;
	return stripes[key & (stripe_count - 1U)];
}

/* A frame's stripe lock, held exclusively. Nothing allocates, makes a record
 * or takes another lock while it is held; the test variant counts every
 * allocation made under it. */
class StripeLock final {
public:
	explicit StripeLock(WordSharedMutex &mutex) noexcept : mutex_{mutex}
	{
		mutex_.lock();
		UML_MEMORY_STRIPE_LOCK_ENTERED();
	}
	~StripeLock()
	{
		UML_MEMORY_STRIPE_LOCK_LEFT();
		mutex_.unlock();
	}
	StripeLock(const StripeLock &) = delete;
	StripeLock &operator=(const StripeLock &) = delete;

private:
	WordSharedMutex &mutex_;
};

using WriterList = std::vector<WriterLocation, FrameAllocator<WriterLocation>>;

/* A frame's word: its kind in the top two bits, and for a single writer its
 * location, the alias id and the page number. "Several" keeps its locations
 * in the frame's code-page record. */
enum class Kind : std::uint64_t { none, single, several, unknown };
constexpr unsigned kind_shift = 62;
constexpr unsigned alias_shift = 36;
constexpr std::uint64_t page_number_mask = (std::uint64_t{1} << alias_shift) - 1U;

[[nodiscard]] constexpr Kind kind_of(std::uint64_t word) noexcept
{
	return static_cast<Kind>(word >> kind_shift);
}

[[nodiscard]] constexpr std::uint64_t kind_word(Kind kind) noexcept
{
	return static_cast<std::uint64_t>(kind) << kind_shift;
}

[[nodiscard]] constexpr std::uint64_t single_word(
	const WriterLocation &location) noexcept
{
	return kind_word(Kind::single) |
		(std::uint64_t{location.alias} << alias_shift) |
		((location.page / frame_bytes) & page_number_mask);
}

[[nodiscard]] constexpr WriterLocation location_of(std::uint64_t word) noexcept
{
	return {static_cast<std::uint32_t>(
			(word >> alias_shift) & FrameAccounting::maximum_alias),
		(word & page_number_mask) * frame_bytes};
}

std::atomic_uint64_t accounting_errors{};
std::atomic_uint64_t nonempty_destructions{};
std::atomic_uint64_t writer_additions{};
std::atomic_uint64_t writer_removals{};
std::atomic_uint64_t reservation_rollbacks{};
std::atomic_uint64_t reservation_growths{};

/* The alias ids: which state a location names. Process-wide and never
 * destroyed, because an id that a leak keeps reserved outlives the address
 * space that leaked it. Id 0 is unused.
 *
 * The entries sit in stable chunks that a reader finds without a lock, so a
 * publication under a stripe lock takes none. Allocating and freeing an id
 * takes a mutex, which no stripe lock is held across. The chunks come from a
 * heap of their own, not the default malloc zone. */
constexpr std::uint32_t alias_chunk_entries = 1024;
constexpr std::size_t alias_chunk_count =
	(std::size_t{FrameAccounting::maximum_alias} + alias_chunk_entries) /
	alias_chunk_entries;

struct AliasEntry final {
	std::atomic<GenerationState *> state{};
	/* The next free id while this one is on the free list. */
	std::uint32_t next_free{};
};
using AliasChunk = std::array<AliasEntry, alias_chunk_entries>;

struct AliasTable final {
	std::mutex mutex;
	/* Chunks are published before any id in them is handed out. */
	std::array<std::atomic<AliasChunk *>, alias_chunk_count> chunks{};
	/* Guarded by `mutex`. */
	std::uint32_t free_head{};
	std::uint32_t next_new{1};
	DomainHeap heap;
};

[[nodiscard]] AliasTable &alias_table() noexcept
{
	static auto *table = new AliasTable;
	return *table;
}

[[nodiscard]] AliasEntry *alias_entry(std::uint32_t alias) noexcept
{
	if (alias == 0 || alias > FrameAccounting::maximum_alias)
		return nullptr;
	auto *chunk = alias_table().chunks[alias / alias_chunk_entries].load(
		std::memory_order_acquire);
	return chunk == nullptr ? nullptr : &(*chunk)[alias % alias_chunk_entries];
}

[[nodiscard]] std::uint32_t allocate_alias(GenerationState &state)
{
	auto &table = alias_table();
	const std::lock_guard lock{table.mutex};
	UML_MEMORY_ALLOCATION_CHECKPOINT(testing::AllocationSite::writers);
	std::uint32_t id = 0;
	if (table.free_head != 0) {
		id = table.free_head;
		table.free_head = alias_entry(id)->next_free;
	} else {
		if (table.next_new > FrameAccounting::maximum_alias)
			throw std::bad_alloc{};
		id = table.next_new;
		if (table.chunks[id / alias_chunk_entries].load(
			    std::memory_order_relaxed) == nullptr) {
			auto *chunk = static_cast<AliasChunk *>(frame_allocate(
				&table.heap, sizeof(AliasChunk), alignof(AliasChunk)));
			new (chunk) AliasChunk{};
			table.chunks[id / alias_chunk_entries].store(
				chunk, std::memory_order_release);
		}
		++table.next_new;
	}
	alias_entry(id)->state.store(&state, std::memory_order_release);
	return id;
}

/* Gives the id back for reuse, or with `leaked` retires it for good: a
 * location may still name it, and the entry then names no state. */
void release_alias(std::uint32_t alias, bool leaked) noexcept
{
	auto &table = alias_table();
	const std::lock_guard lock{table.mutex};
	auto *entry = alias_entry(alias);
	if (entry == nullptr)
		return;
	entry->state.store(nullptr, std::memory_order_release);
	if (leaked)
		return;
	entry->next_free = table.free_head;
	table.free_head = alias;
}

/* Adds `location` to the frame's writers, under the frame's stripe lock.
 * `record` is the frame's record, found before the lock was taken; null if it
 * has none. When the list needs a record or more room than `spare` brings,
 * returns false with the room wanted in `wanted`; the caller makes the record
 * and `spare` outside the lock and tries again. */
[[nodiscard]] bool try_add(MappedBacking &backing,
	MappedBacking::FrameRecord *record, std::uint64_t offset,
	const WriterLocation &location, WriterList &spare, std::size_t &wanted,
	bool &added) noexcept
{
	auto *word = backing.writer_word(offset);
	if (word == nullptr) {
		/* The caller made the leaf before taking the lock. */
		accounting_errors.fetch_add(1, std::memory_order_relaxed);
		added = false;
		return true;
	}
	added = false;
	switch (kind_of(*word)) {
	case Kind::none:
		*word = single_word(location);
		added = true;
		writer_additions.fetch_add(1, std::memory_order_relaxed);
		return true;
	case Kind::unknown:
		return true;
	case Kind::single: {
		const auto existing = location_of(*word);
		if (existing == location)
			return true;
		if (record == nullptr) {
			wanted = 2;
			return false;
		}
		auto *list = &record->writers;
		if (list->capacity() < 2) {
			if (spare.capacity() < 2) {
				wanted = 2;
				return false;
			}
			spare.assign(list->begin(), list->end());
			list->swap(spare);
		}
		list->clear();
		list->push_back(existing);
		list->push_back(location);
		*word = kind_word(Kind::several);
		added = true;
		writer_additions.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	case Kind::several: {
		if (record == nullptr) {
			/* Several writers without their record: an error. */
			accounting_errors.fetch_add(1, std::memory_order_relaxed);
			*word = kind_word(Kind::unknown);
			return true;
		}
		auto *list = &record->writers;
		if (std::find(list->begin(), list->end(), location) != list->end())
			return true;
		if (list->size() == list->capacity()) {
			const auto room = std::max<std::size_t>(4, list->size() * 2U);
			if (spare.capacity() < room) {
				wanted = room;
				return false;
			}
			spare.assign(list->begin(), list->end());
			list->swap(spare);
		}
		list->push_back(location);
		added = true;
		writer_additions.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	}
	return true;
}

} // namespace

bool FrameAccounting::add_writer(MappedBacking &backing,
	std::uint64_t offset, const WriterLocation &location)
{
	WriterList spare{FrameAllocator<WriterLocation>{backing.heap()}};
	std::size_t wanted = 0;
	for (;;) {
		UML_MEMORY_ALLOCATION_CHECKPOINT(testing::AllocationSite::writers);
		/* The leaf and the record are made here, before the lock. */
		if ((wanted != 0 || backing.writer_word(offset) == nullptr) &&
		    !backing.prepare_writers(offset, wanted != 0))
			throw std::bad_alloc{};
		if (wanted != 0 && spare.capacity() < wanted)
			spare.reserve(wanted);
		auto *record = backing.find_frame_record(offset);
		std::size_t more = 0;
		bool added = false;
		{
			const StripeLock lock{stripe_for(&backing, offset)};
			if (try_add(backing, record, offset, location, spare, more, added))
				return added;
		}
		wanted = more;
	}
}

void FrameAccounting::remove_writer(MappedBacking &backing,
	std::uint64_t offset, const WriterLocation &location) noexcept
{
	/* Found without the lock, and the room a list gives up is freed after
	 * it. */
	auto *record = backing.find_frame_record(offset);
	WriterList released{FrameAllocator<WriterLocation>{backing.heap()}};
	const StripeLock lock{stripe_for(&backing, offset)};
	auto *word = backing.writer_word(offset);
	if (word == nullptr) {
		accounting_errors.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	const auto fail = [&] {
		accounting_errors.fetch_add(1, std::memory_order_relaxed);
		*word = kind_word(Kind::unknown);
	};
	switch (kind_of(*word)) {
	case Kind::none:
		fail();
		return;
	case Kind::unknown:
		return;
	case Kind::single:
		if (location_of(*word) == location) {
			*word = 0;
			writer_removals.fetch_add(1, std::memory_order_relaxed);
		} else {
			fail();
		}
		return;
	case Kind::several: {
		auto *list = record == nullptr ? nullptr : &record->writers;
		const auto found = list == nullptr ? WriterList::iterator{} :
			std::find(list->begin(), list->end(), location);
		if (list == nullptr || found == list->end()) {
			fail();
			return;
		}
		list->erase(found);
		writer_removals.fetch_add(1, std::memory_order_relaxed);
		if (list->size() == 1) {
			*word = single_word(list->front());
			list->clear();
			list->swap(released);
		} else if (list->empty()) {
			*word = 0;
			list->swap(released);
		}
		return;
	}
	}
}

void FrameAccounting::reserve(GenerationState &state, std::uint64_t page,
	const FrameRef &final)
{
	if (!final.writable || final.backing == nullptr || !tracks(page))
		return;
	state.accounting.reservation.add(
		state, *final.backing, final.offset, page);
}

void FrameAccounting::apply(GenerationState &state, std::uint64_t page,
	const FrameRef &before, const FrameRef &after) noexcept
{
	if (!before.writable || before.backing == nullptr || !tracks(page))
		return;
	if (after.writable && after.backing == before.backing &&
	    after.offset / frame_bytes == before.offset / frame_bytes)
		return;
	remove_writer(*before.backing, before.offset,
		{state.accounting.alias, page});
}

void FrameAccounting::release_word(GenerationState &state,
	std::uint64_t page, std::uintptr_t word) noexcept
{
	const auto fields = FillIndex::decode(word);
	if (!fields.write || !tracks(page))
		return;
	const auto entry = state.backings.entry(fields.slot, page);
	if (entry.backing == nullptr) {
		accounting_errors.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	const auto offset = fields.host -
		reinterpret_cast<std::uintptr_t>(entry.backing->base());
	remove_writer(*entry.backing, offset, {state.accounting.alias, page});
}

void FrameAccounting::commit(GenerationState &state) noexcept
{
	state.accounting.reservation.commit();
}

void FrameAccounting::settle(GenerationState &state) noexcept
{
	state.accounting.reservation.settle(state.accounting.alias);
}

void FrameAccounting::walk(GenerationState &state) noexcept
{
	if (state.accounting.alias != 0)
		(void)state.fill_index.scan(0, ~std::uint64_t{0},
			[&](std::uint64_t page, std::uintptr_t word) {
				release_word(state, page, word);
				return false;
			});
}

void FrameAccounting::finish(GenerationState &state) noexcept
{
	state.accounting.walked = true;
	if (state.accounting.alias != 0) {
		release_alias(state.accounting.alias, false);
		state.accounting.alias = 0;
	}
}

void FrameAccounting::retire(GenerationState &state) noexcept
{
	walk(state);
	finish(state);
}

FrameAccounting::Diagnostics FrameAccounting::diagnostics() noexcept
{
	return {accounting_errors.load(std::memory_order_relaxed),
		nonempty_destructions.load(std::memory_order_relaxed),
		writer_additions.load(std::memory_order_relaxed),
		writer_removals.load(std::memory_order_relaxed),
		reservation_rollbacks.load(std::memory_order_relaxed),
		reservation_growths.load(std::memory_order_relaxed)};
}

void FrameAccounting::note_nonempty_state_destruction() noexcept
{
	nonempty_destructions.fetch_add(1, std::memory_order_relaxed);
}

StateAccounting::~StateAccounting()
{
	/* A state is destroyed after its walk, which gave the alias id back. */
	if (alias != 0)
		FrameAccounting::note_nonempty_state_destruction();
}

GenerationState *FrameAccounting::state_of(std::uint32_t alias) noexcept
{
	const auto *entry = alias_entry(alias);
	return entry == nullptr ? nullptr :
		entry->state.load(std::memory_order_acquire);
}

#if UML_MEMORY_ALLOCATION_TESTING
bool FrameAccounting::locations_for_testing(MappedBacking &backing,
	std::uint64_t offset, std::vector<WriterLocation> &locations)
{
	/* The room is made before the lock, as everywhere. */
	std::size_t room = 2;
	for (;;) {
		locations.reserve(locations.size() + room);
		const StripeLock lock{stripe_for(&backing, offset)};
		const auto *word = backing.writer_word(offset);
		if (word == nullptr)
			return true;
		switch (kind_of(*word)) {
		case Kind::none:
			return true;
		case Kind::unknown:
			return false;
		case Kind::single:
			locations.push_back(location_of(*word));
			return true;
		case Kind::several: {
			const auto *record = backing.find_frame_record(offset);
			if (record == nullptr)
				return true;
			if (locations.capacity() - locations.size() <
			    record->writers.size()) {
				room = record->writers.size();
				continue;
			}
			locations.insert(locations.end(), record->writers.begin(),
				record->writers.end());
			return true;
		}
		}
		return true;
	}
}
#endif

WriterSettler::~WriterSettler()
{
	FrameAccounting::settle(state_);
}

void WriterReservation::add(GenerationState &state, MappedBacking &backing,
	std::uint64_t offset, std::uint64_t page)
{
	auto &accounting = state.accounting;
	/* A walked state belongs to a generation that is gone; its callers hold
	 * no reference they could reach it by, so this is an error, and it
	 * fails the primitive like any other the primitive cannot complete. */
	if (accounting.walked)
		throw std::bad_alloc{};
	if (accounting.alias == 0)
		accounting.alias = allocate_alias(state);
	UML_MEMORY_ALLOCATION_CHECKPOINT(testing::AllocationSite::writers);
	/* The room for the entry is had before the location is added, so that
	 * recording it cannot fail. The entries live in the thread's scratch
	 * arena, which the plan's scope keeps until commit or settle, and the
	 * room doubles, so a plan of any size reserves it in a few steps. */
	if (!scope_)
		scope_.emplace();
	if (size_ == capacity_) {
		const auto room = std::max<std::size_t>(16, capacity_ * 2U);
		auto *items = static_cast<Added *>(
			scope_->arena().allocate(room * sizeof(Added), alignof(Added)));
		if (size_ != 0)
			std::memcpy(items, items_, size_ * sizeof(Added));
		items_ = items;
		capacity_ = room;
		reservation_growths.fetch_add(1, std::memory_order_relaxed);
	}
	if (FrameAccounting::add_writer(backing, offset, {accounting.alias, page}))
		items_[size_++] = {&backing, offset, page};
}

void WriterReservation::settle(std::uint32_t alias) noexcept
{
	if (size_ != 0)
		reservation_rollbacks.fetch_add(1, std::memory_order_relaxed);
	for (auto added = size_; added != 0; --added)
		FrameAccounting::remove_writer(*items_[added - 1U].backing,
			items_[added - 1U].offset, {alias, items_[added - 1U].page});
	release();
}

void WriterReservation::release() noexcept
{
	items_ = nullptr;
	size_ = 0;
	capacity_ = 0;
	/* The outermost scope's end hands the arena's extra chunks back. */
	scope_.reset();
}

} // namespace uml::memory

#endif
