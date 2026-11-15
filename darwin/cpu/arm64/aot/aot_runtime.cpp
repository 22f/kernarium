// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/aot/aot_runtime.hpp"

#include <arm_acle.h>

#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

#if UML_AOT_DIAGNOSTICS
#include "cpu/arm64/aot/aot_diagnostics.hpp"

#include <chrono>
#include <cstdio>
#include <thread>
#endif

/* The framework's blob (aot_blob.S). */
extern "C" const std::uint8_t uml_aot_blob[];

namespace uml::cpu::arm64::aot {
namespace {

constexpr std::uint64_t page_size = 4096;
constexpr char blob_magic[8] = {'U', 'M', 'L', 'A', 'O', 'T', 'B', '1'};

/* The generator's layout (tools/aot/rewrite.py: build_blob). */
constexpr std::size_t header_library_count = 16;
constexpr std::size_t header_records = 20;
constexpr std::size_t header_enter = 24;
constexpr std::size_t header_hash = 28;
constexpr std::size_t header_slots = 32;
constexpr std::size_t record_size = 256;
constexpr std::size_t record_table_base = 152;
constexpr std::size_t record_table_end = 160;
constexpr std::size_t record_table_offset = 168;
constexpr std::size_t record_table_words = 172;
constexpr std::size_t record_digests = 176;
constexpr std::size_t record_pages = 180;
constexpr std::size_t record_unit_offset = 200;

template<typename T>
[[nodiscard]] T read(std::size_t offset) noexcept
{
	T value;
	std::memcpy(&value, uml_aot_blob + offset, sizeof(T));
	return value;
}

struct Library final {
	std::uint64_t table_base;
	std::uint64_t table_end;
	std::size_t table_offset;
	std::size_t table_words;
	std::size_t digests;
	std::size_t pages;
	std::size_t unit_offset;
};

struct Directory final {
	bool valid{};
	std::uint32_t libraries{};
	std::size_t records{};
	std::size_t enter{};
	std::size_t hash{};
	std::uint32_t slots{};

	[[nodiscard]] Library library(std::uint32_t index) const noexcept
	{
		const auto base = records + record_size * index;
		return {read<std::uint64_t>(base + record_table_base),
			read<std::uint64_t>(base + record_table_end),
			read<std::uint32_t>(base + record_table_offset),
			read<std::uint32_t>(base + record_table_words),
			read<std::uint32_t>(base + record_digests),
			read<std::uint32_t>(base + record_pages),
			read<std::uint32_t>(base + record_unit_offset)};
	}
};

[[nodiscard]] const Directory &directory() noexcept
{
	static const Directory value = []() noexcept {
		Directory d;
		if (std::memcmp(uml_aot_blob, blob_magic, sizeof(blob_magic)) != 0)
			return d;
		d.libraries = read<std::uint32_t>(header_library_count);
		d.records = read<std::uint32_t>(header_records);
		d.enter = read<std::uint32_t>(header_enter);
		d.hash = read<std::uint32_t>(header_hash);
		d.slots = read<std::uint32_t>(header_slots);
		d.valid = d.libraries != 0 && d.slots != 0 &&
			(d.slots & (d.slots - 1)) == 0;
		return d;
	}();
	return value;
}

/* The identity of one 4 KiB page (tools/aot/rewrite.py: page_digest). */
[[nodiscard]] std::uint64_t page_digest(const std::byte *page) noexcept
{
	std::uint32_t low = 0;
	std::uint32_t high = 0x9e3779b9U;
	for (std::size_t offset = 0; offset < page_size; offset += 8) {
		std::uint64_t word;
		std::memcpy(&word, page + offset, sizeof(word));
		low = __crc32cd(low, word);
		high = __crc32cd(high, word);
	}
	const auto digest = (std::uint64_t{high} << 32) | low;
	return digest != 0 ? digest : 1;
}


std::atomic<int> g_state{-1};

#if UML_AOT_DIAGNOSTICS
namespace counters {
std::atomic<std::uint64_t> binds{0}, verifications{0}, pages_hashed{0},
	unreadable{0}, poisoned{0}, enters{0}, steps{0}, retired{0}, refills{0},
	refill_failures{0}, cross_hits{0}, cross_misses{0};
std::array<std::atomic<std::uint64_t>, 4> refusals{};
/* The guest addresses AOT code stepped at most: library << 40 | offset. */
constexpr std::size_t step_slots = 32768;
std::array<std::atomic<std::uint64_t>, step_slots> step_key{};
std::array<std::atomic<std::uint64_t>, step_slots> step_count{};
std::array<std::atomic<std::uint64_t>, 64> library_enters{};
std::array<std::atomic<std::uint64_t>, 64> library_steps{};
std::atomic<bool> reporter_started{false};
} // namespace counters

#define UML_AOT_COUNT(name, amount) \
	counters::name.fetch_add((amount), std::memory_order_relaxed)

void note_step(std::uint64_t key) noexcept
{
	std::size_t slot = (key * 0x9e3779b97f4a7c15ULL) >> 49;
	for (std::size_t probe = 0; probe < 32; ++probe) {
		const auto at = (slot + probe) % counters::step_slots;
		auto seen = counters::step_key[at].load(std::memory_order_relaxed);
		if (seen == 0)
			counters::step_key[at].compare_exchange_strong(seen, key);
		if (counters::step_key[at].load(std::memory_order_relaxed) == key) {
			counters::step_count[at].fetch_add(1, std::memory_order_relaxed);
			return;
		}
	}
}

/* Written from a thread of its own: it allocates and does file I/O. */
void write_report() noexcept
{
	const char *path = std::getenv("DARWIN_UML_AOT_REPORT");
	if (path == nullptr)
		return;
	std::FILE *file = std::fopen(path, "w");
	if (file == nullptr)
		return;
	auto get = [](const std::atomic<std::uint64_t> &value) {
		return static_cast<unsigned long long>(
			value.load(std::memory_order_relaxed));
	};
	std::fprintf(file,
		"binds %llu verifications %llu pages_hashed %llu unreadable %llu "
		"poisoned %llu enters %llu steps %llu retired %llu refills %llu "
		"refill_failures %llu cross_hits %llu cross_misses %llu\n",
		get(counters::binds), get(counters::verifications),
		get(counters::pages_hashed), get(counters::unreadable),
		get(counters::poisoned), get(counters::enters), get(counters::steps),
		get(counters::retired), get(counters::refills),
		get(counters::refill_failures), get(counters::cross_hits),
		get(counters::cross_misses));
	std::fprintf(file, "refused tbi %llu sp %llu exclusive %llu fp %llu\n",
		get(counters::refusals[0]), get(counters::refusals[1]),
		get(counters::refusals[2]), get(counters::refusals[3]));
	for (std::size_t library = 0; library < 64; ++library)
		if (get(counters::library_enters[library]) != 0)
			std::fprintf(file, "library %zu enters %llu steps %llu\n", library,
				get(counters::library_enters[library]),
				get(counters::library_steps[library]));
	for (std::size_t slot = 0; slot < counters::step_slots; ++slot) {
		const auto count = get(counters::step_count[slot]);
		if (count != 0) {
			const auto key = get(counters::step_key[slot]);
			std::fprintf(file, "step %llu %llx %llu\n", key >> 40,
				key & ((1ULL << 40) - 1), count);
		}
	}
	std::fclose(file);
}

void start_reporter() noexcept
{
	if (std::getenv("DARWIN_UML_AOT_REPORT") == nullptr ||
	    counters::reporter_started.exchange(true))
		return;
	try {
		std::thread([] {
			for (;;) {
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				write_report();
			}
		}).detach();
	} catch (...) {
	}
}
#else
#define UML_AOT_COUNT(name, amount) ((void)0)
#endif

[[nodiscard]] bool environment_enabled() noexcept
{
	const char *value = std::getenv("DARWIN_UML_AOT");
	return value != nullptr && value[0] == '1' && value[1] == '\0' &&
		directory().valid;
}

/* Rewritten code asks for the TLB entry of a page it missed (see the refill
 * thunk in tools/aot/rewrite.py): fill it as the engine does and say whether
 * the entry now holds the page. A fault is not recorded for the guest here;
 * the instruction steps and the engine takes it. An access that crosses a
 * page also steps: the probe found it as a miss. */
std::uint64_t refill(void *tlb_pointer, std::uint64_t address,
	std::uint64_t code) noexcept
{
	UML_AOT_COUNT(refills, 1);
	auto &tlb = *static_cast<::uml::memory::Tlb *>(tlb_pointer);
	const auto kind = (code & 0xff) == 1 ? ::uml::memory::Access::write :
		::uml::memory::Access::read;
	const auto bytes = code >> 8;
	if (bytes > 1)
		address -= bytes - 1;
	if ((address & (page_size - 1)) + bytes > page_size) {
		UML_AOT_COUNT(refill_failures, 1);
		return 0;
	}
	if (tlb.translate(address, kind) == nullptr) {
		UML_AOT_COUNT(refill_failures, 1);
		return 0;
	}
	const auto page = address >> DARWIN_UML_GUEST_PAGE_SHIFT;
	const auto &entry = tlb.entries[page & (::uml::memory::tlb_entry_count - 1)];
	return (kind == ::uml::memory::Access::write ? entry.write_page :
		entry.read_page) == page ? 1 : 0;
}

/* Rewritten code branches to an address outside its own library: answer the
 * native code of the verified instance that holds it, or none. */
Target lookup(const CrossTable *table, std::uint64_t address) noexcept
{
	for (std::uint64_t index = 0; index < table->count; ++index) {
		const auto &entry = table->entries[index];
		if (address < entry.low || address >= entry.high)
			continue;
		const auto library = directory().library(entry.library);
		const std::uint64_t offset = address - entry.bias;
		if ((offset & 3U) != 0)
			break;
		const auto native = read<std::uint32_t>(library.table_offset +
			4 * ((offset - library.table_base) / 4));
		if (native == 0)
			break;
		UML_AOT_COUNT(cross_hits, 1);
		return {uml_aot_blob + library.unit_offset + native, entry.bias};
	}
	UML_AOT_COUNT(cross_misses, 1);
	return {};
}

} // namespace

bool enabled() noexcept
{
	auto state = g_state.load(std::memory_order_relaxed);
	if (state < 0) {
		state = environment_enabled() ? 1 : 0;
		g_state.store(state, std::memory_order_relaxed);
#if UML_AOT_DIAGNOSTICS
		if (state != 0)
			start_reporter();
#endif
	}
	return state != 0;
}

void note_refusal(Refusal reason) noexcept
{
#if UML_AOT_DIAGNOSTICS
	counters::refusals[static_cast<std::size_t>(reason)].fetch_add(
		1, std::memory_order_relaxed);
#else
	(void)reason;
#endif
}

#if UML_AOT_DIAGNOSTICS
namespace diagnostics {

void set_enabled(std::optional<bool> state) noexcept
{
	g_state.store(!state.has_value() ? -1 :
		(*state && directory().valid ? 1 : 0), std::memory_order_relaxed);
}

Stats stats() noexcept
{
	auto get = [](const std::atomic<std::uint64_t> &value) {
		return value.load(std::memory_order_relaxed);
	};
	Stats result{get(counters::binds), get(counters::verifications),
		get(counters::pages_hashed), get(counters::unreadable),
		get(counters::poisoned), get(counters::enters), get(counters::steps),
		get(counters::retired), get(counters::refills),
		get(counters::refill_failures), get(counters::cross_hits),
		get(counters::cross_misses), {}};
	for (std::size_t i = 0; i < result.refusals.size(); ++i)
		result.refusals[i] = get(counters::refusals[i]);
	return result;
}

void reset_stats() noexcept
{
	for (auto *counter : {&counters::binds, &counters::verifications,
		     &counters::pages_hashed, &counters::unreadable,
		     &counters::poisoned, &counters::enters, &counters::steps,
		     &counters::retired, &counters::refills,
		     &counters::refill_failures, &counters::cross_hits,
		     &counters::cross_misses})
		counter->store(0, std::memory_order_relaxed);
	for (auto &counter : counters::refusals)
		counter.store(0, std::memory_order_relaxed);
}

} // namespace diagnostics
#endif

Binder::Binder() noexcept
{
	cross_.refill = &refill;
	cross_.lookup = &lookup;
}

bool Binder::changed_since(void *context, std::uint64_t sequence,
	std::uint64_t low, std::uint64_t high, std::uint64_t &current) noexcept
{
	if (context == nullptr)
		return true;
	darwin_uml_static_code_change ranges[32];
	const int count = darwin_uml_static_vm_execution_context_code_change_ranges(
		context, sequence, ranges, 32, &current);
	if (count < 0)
		return true;
	for (int index = 0; index < count; ++index) {
		/* A range of length zero names the whole address space. */
		if (ranges[index].length == 0)
			return true;
		if (ranges[index].address < high &&
		    ranges[index].address + ranges[index].length > low)
			return true;
	}
	return false;
}

Binder::Verdict Binder::verify_pages(
	::uml::memory::StaticGuestMemory &memory, const Instance &instance,
	std::uint64_t first, std::uint64_t last) noexcept
{
	const auto &dir = directory();
	const auto library = dir.library(instance.library);
	alignas(8) std::array<std::byte, page_size> page;
	for (std::uint64_t index = first; index < last; ++index) {
		const auto address =
			instance.bias + library.table_base + index * page_size;
		if (!memory.fetch(address, page))
			return Verdict::unreadable;
		UML_AOT_COUNT(pages_hashed, 1);
		if (page_digest(page.data()) !=
		    read<std::uint64_t>(library.digests + 8 * index))
			return Verdict::mismatch;
	}
	return Verdict::match;
}

Binder::Verdict Binder::verify(::uml::memory::StaticGuestMemory &memory,
	void *context, Instance &instance, std::uint64_t code_changes) noexcept
{
	const auto library = directory().library(instance.library);
	Verdict verdict = Verdict::match;
	bool full = !instance.verified;
	darwin_uml_static_code_change ranges[32];
	int count = 0;
	std::uint64_t current = 0;
	if (!full) {
		count = context == nullptr ? -1 :
			darwin_uml_static_vm_execution_context_code_change_ranges(
				context, instance.verified_changes, ranges, 32, &current);
		full = count < 0;
	}
	const std::uint64_t low = instance.bias + library.table_base;
	const std::uint64_t high = instance.bias + library.table_end;
	if (full) {
		UML_AOT_COUNT(verifications, 1);
		verdict = verify_pages(memory, instance, 0, library.pages);
	} else {
		for (int index = 0; index < count && verdict == Verdict::match;
		     ++index) {
			std::uint64_t from = ranges[index].address;
			std::uint64_t to = from + ranges[index].length;
			if (ranges[index].length == 0) {
				from = low;
				to = high;
			}
			if (from >= high || to <= low)
				continue;
			from = std::max(from, low);
			to = std::min(to, high);
			verdict = verify_pages(memory, instance,
				(from - low) / page_size,
				(to - low + page_size - 1) / page_size);
		}
	}
	if (verdict == Verdict::match) {
		instance.verified = true;
		instance.verified_changes = code_changes;
		instance.backoff = 0;
	}
	return verdict;
}

std::int32_t Binder::resolve(::uml::memory::StaticGuestMemory &memory,
	void *, unsigned long generation, std::uint64_t, std::uint64_t page_va) noexcept
{
	const auto &dir = directory();
	alignas(8) std::array<std::byte, page_size> page;
	if (!memory.fetch(page_va, page))
		return not_ready;
	const auto digest = page_digest(page.data());
	std::uint32_t slot = static_cast<std::uint32_t>(digest) & (dir.slots - 1);
	std::uint32_t library_index = 0;
	std::uint32_t page_index = 0;
	for (;;) {
		const auto at = dir.hash + 16 * static_cast<std::size_t>(slot);
		const auto stored = read<std::uint64_t>(at);
		if (stored == 0)
			return none;
		if (stored == digest) {
			library_index = read<std::uint32_t>(at + 8);
			page_index = read<std::uint32_t>(at + 12);
			break;
		}
		slot = (slot + 1) & (dir.slots - 1);
	}
	const auto library = dir.library(library_index);
	const std::uint64_t bias =
		page_va - (library.table_base + page_index * page_size);
	Instance *found = nullptr;
	for (auto &candidate : instances_)
		if (candidate.used && candidate.generation == generation &&
		    candidate.library == library_index && candidate.bias == bias) {
			found = &candidate;
			break;
		}
	if (found == nullptr) {
		const auto victim = next_instance_;
		next_instance_ = (next_instance_ + 1) % instance_capacity;
		/* No page slot may name the instance that takes this place. */
		for (auto &slot_entry : pages_)
			if (slot_entry.instance == static_cast<std::int32_t>(victim))
				slot_entry = PageSlot{};
		found = &instances_[victim];
		*found = Instance{generation, library_index, bias, 0, 0, 0, false,
			true, false};
		++epoch_;
	}
	return found->poisoned ? none :
		static_cast<std::int32_t>(found - instances_.data());
}

Hit Binder::find(::uml::memory::StaticGuestMemory &memory, void *context,
	unsigned long generation, std::uint64_t code_changes,
	std::uint64_t pc) noexcept
{
	if (!directory().valid || (pc & 3U) != 0)
		return {};
	++ticks_;
	const auto page_va = pc & ~(page_size - 1);
	auto &slot = pages_[((page_va >> 12) ^ generation) % page_slots];
	bool fresh = slot.page == page_va && slot.generation == generation;
	if (fresh && slot.stamp != code_changes) {
		std::uint64_t current = 0;
		if (changed_since(context, slot.stamp, page_va, page_va + page_size,
			    current))
			fresh = false;
		else
			slot.stamp = code_changes;
	}
	if (!fresh) {
		UML_AOT_COUNT(binds, 1);
		const auto answer =
			resolve(memory, context, generation, code_changes, page_va);
		if (answer == not_ready)
			return {};
		slot.instance = answer;
		slot.page = page_va;
		slot.generation = generation;
		slot.stamp = code_changes;
	}
	if (slot.instance < 0)
		return {};
	auto &instance = instances_[static_cast<std::size_t>(slot.instance)];
	if (!instance.used || instance.poisoned ||
	    instance.generation != generation) {
		slot = PageSlot{};
		return {};
	}
	if (!instance.verified || instance.verified_changes != code_changes) {
		if (ticks_ < instance.retry_at)
			return {};
		switch (verify(memory, context, instance, code_changes)) {
		case Verdict::match:
			++epoch_;
			break;
		case Verdict::unreadable:
			UML_AOT_COUNT(unreadable, 1);
			instance.backoff = std::min<std::uint64_t>(
				std::max<std::uint64_t>(instance.backoff * 2, 64), 65536);
			instance.retry_at = ticks_ + instance.backoff;
			instance.verified = false;
			return {};
		case Verdict::mismatch:
			instance.poisoned = true;
			instance.verified = false;
			UML_AOT_COUNT(poisoned, 1);
			++epoch_;
			return {};
		}
	}
	return entry(static_cast<std::uint32_t>(slot.instance), pc);
}

Hit Binder::entry(std::uint32_t instance_index,
	std::uint64_t pc) const noexcept
{
	const auto &instance = instances_[instance_index];
	if (!instance.used || instance.poisoned || !instance.verified)
		return {};
	const auto library = directory().library(instance.library);
	const std::uint64_t offset = pc - instance.bias;
	if (offset < library.table_base || offset >= library.table_end ||
	    (offset & 3U) != 0)
		return {};
	const auto word = (offset - library.table_base) / 4;
	const auto native =
		read<std::uint32_t>(library.table_offset + 4 * word);
	if (native == 0)
		return {};
	return {uml_aot_blob + library.unit_offset + native, instance.bias,
		instance_index, instance.library};
}

const CrossTable &Binder::cross(unsigned long generation,
	std::uint64_t code_changes) noexcept
{
	if (cross_generation_ == generation && cross_epoch_ == epoch_ &&
	    cross_changes_ == code_changes)
		return cross_;
	cross_.count = 0;
	for (const auto &instance : instances_) {
		if (!instance.used || instance.poisoned || !instance.verified ||
		    instance.generation != generation ||
		    instance.verified_changes != code_changes)
			continue;
		const auto library = directory().library(instance.library);
		cross_.entries[cross_.count++] = {
			instance.bias + library.table_base,
			instance.bias + library.table_end, instance.bias,
			instance.library, 0};
	}
	std::sort(cross_.entries.begin(), cross_.entries.begin() +
		static_cast<std::ptrdiff_t>(cross_.count),
		[](const CrossTable::Entry &a, const CrossTable::Entry &b) {
			return a.low < b.low;
		});
	cross_generation_ = generation;
	cross_epoch_ = epoch_;
	cross_changes_ = code_changes;
	return cross_;
}

bool Binder::locate(unsigned long generation, std::uint64_t address,
	std::uint32_t &library, std::uint64_t &offset) const noexcept
{
	for (const auto &instance : instances_) {
		if (!instance.used || instance.generation != generation)
			continue;
		const auto record = directory().library(instance.library);
		if (address >= instance.bias + record.table_base &&
		    address < instance.bias + record.table_end) {
			library = instance.library;
			offset = address - instance.bias;
			return true;
		}
	}
	return false;
}

EnterResult enter(CpuState &cpu, ::uml::memory::Tlb &tlb, const Hit &hit,
	std::uint64_t budget, const CrossTable &table) noexcept
{
	using Entry = EnterResult (*)(CpuState *, void *, std::uint64_t,
		const void *, std::uint64_t, const CrossTable *);
	/* The probe indexes the entries from the pointer the refill gets. */
	if (static_cast<const void *>(tlb.entries.data()) !=
	    static_cast<const void *>(&tlb))
		std::abort();
	const auto function = reinterpret_cast<Entry>(
		const_cast<std::uint8_t *>(uml_aot_blob) + directory().enter);
	UML_AOT_COUNT(enters, 1);
#if UML_AOT_DIAGNOSTICS
	if (hit.library < 64)
		counters::library_enters[hit.library].fetch_add(
			1, std::memory_order_relaxed);
#endif
	const auto result = function(&cpu, &tlb, hit.bias, hit.native, budget,
		&table);
	UML_AOT_COUNT(retired, result.retired);
#if UML_AOT_DIAGNOSTICS
	if (result.reason == Reason::step) {
		counters::steps.fetch_add(1, std::memory_order_relaxed);
		for (std::uint64_t index = 0; index < table.count; ++index) {
			const auto &entry = table.entries[index];
			if (cpu.pc >= entry.low && cpu.pc < entry.high) {
				if (entry.library < 64)
					counters::library_steps[entry.library].fetch_add(
						1, std::memory_order_relaxed);
				note_step((std::uint64_t{entry.library} << 40) |
					(cpu.pc - entry.bias));
				break;
			}
		}
	}
#endif
	return result;
}

} // namespace uml::cpu::arm64::aot
