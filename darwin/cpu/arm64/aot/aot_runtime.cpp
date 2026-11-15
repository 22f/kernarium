// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/aot/aot_runtime.hpp"

#include <arm_acle.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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
			read<std::uint32_t>(base + record_pages)};
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
std::atomic<std::uint64_t> g_binds{0};
std::atomic<std::uint64_t> g_verifications{0};
std::atomic<std::uint64_t> g_unreadable{0};
std::atomic<std::uint64_t> g_poisoned{0};
std::atomic<std::uint64_t> g_enters{0};
std::atomic<std::uint64_t> g_steps{0};
std::atomic<std::uint64_t> g_retired{0};

/* The guest addresses AOT code stepped at most, for the report. */
constexpr std::size_t step_slots = 8192;
std::atomic<std::uint64_t> g_step_pc[step_slots];
std::atomic<std::uint64_t> g_step_count[step_slots];
std::atomic<bool> g_step_histogram{false};

void note_step(std::uint64_t pc) noexcept
{
	std::size_t slot = (pc >> 2) * 0x9e3779b97f4a7c15ULL >> 51;
	for (std::size_t probe = 0; probe < 16; ++probe) {
		const auto at = (slot + probe) % step_slots;
		auto seen = g_step_pc[at].load(std::memory_order_relaxed);
		if (seen == 0)
			g_step_pc[at].compare_exchange_strong(seen, pc);
		if (g_step_pc[at].load(std::memory_order_relaxed) == pc) {
			g_step_count[at].fetch_add(1, std::memory_order_relaxed);
			return;
		}
	}
}

/* DARWIN_UML_AOT_REPORT=<path>: the counters, rewritten every 65536 entries
 * and at exit, for a run whose stderr nobody reads. */
void write_report() noexcept
{
	const char *path = std::getenv("DARWIN_UML_AOT_REPORT");
	if (path == nullptr)
		return;
	std::FILE *file = std::fopen(path, "w");
	if (file == nullptr)
		return;
	std::fprintf(file,
		"binds %llu verifications %llu unreadable %llu poisoned %llu "
		"enters %llu steps %llu retired %llu\n",
		static_cast<unsigned long long>(g_binds.load()),
		static_cast<unsigned long long>(g_verifications.load()),
		static_cast<unsigned long long>(g_unreadable.load()),
		static_cast<unsigned long long>(g_poisoned.load()),
		static_cast<unsigned long long>(g_enters.load()),
		static_cast<unsigned long long>(g_steps.load()),
		static_cast<unsigned long long>(g_retired.load()));
	/* The hottest step sites, from a copy of the counts. */
	std::vector<std::uint64_t> counts(step_slots);
	for (std::size_t i = 0; i < step_slots; ++i)
		counts[i] = g_step_count[i].load(std::memory_order_relaxed);
	for (int rank = 0; rank < 16; ++rank) {
		const auto best = static_cast<std::size_t>(
			std::max_element(counts.begin(), counts.end()) - counts.begin());
		if (counts[best] == 0)
			break;
		std::fprintf(file, "step %llx %llu\n",
			static_cast<unsigned long long>(
				g_step_pc[best].load(std::memory_order_relaxed)),
			static_cast<unsigned long long>(counts[best]));
		counts[best] = 0;
	}
	std::fclose(file);
}

[[nodiscard]] bool environment_enabled() noexcept
{
	const char *value = std::getenv("DARWIN_UML_AOT");
	return value != nullptr && value[0] == '1' && value[1] == '\0' &&
		directory().valid;
}

/* Rewritten code asks for the TLB entry of a page it missed (see the refill
 * thunk in tools/aot/rewrite.py): fill it as the engine does and say whether
 * the entry now holds the page. A fault is not recorded for the guest here;
 * the instruction steps and the engine takes it. */
extern "C" std::uint64_t uml_aot_refill(
	void *tlb_pointer, std::uint64_t address, std::uint64_t access) noexcept
{
	auto &tlb = *static_cast<::uml::memory::Tlb *>(tlb_pointer);
	const auto kind = access == 1 ? ::uml::memory::Access::write :
		::uml::memory::Access::read;
	if (tlb.translate(address, kind) == nullptr)
		return 0;
	const auto page = address >> DARWIN_UML_GUEST_PAGE_SHIFT;
	const auto &entry = tlb.entries[page & (::uml::memory::tlb_entry_count - 1)];
	return (kind == ::uml::memory::Access::write ? entry.write_page :
		entry.read_page) == page ? 1 : 0;
}

} // namespace

bool enabled() noexcept
{
	auto state = g_state.load(std::memory_order_relaxed);
	if (state < 0) {
		state = environment_enabled() ? 1 : 0;
		g_state.store(state, std::memory_order_relaxed);
		if (state != 0 && std::getenv("DARWIN_UML_AOT_REPORT") != nullptr) {
			g_step_histogram.store(true, std::memory_order_relaxed);
			std::atexit(write_report);
		}
	}
	return state != 0;
}

void set_enabled(std::optional<bool> state) noexcept
{
	g_state.store(!state.has_value() ? -1 :
		(*state && directory().valid ? 1 : 0), std::memory_order_relaxed);
}

Stats stats() noexcept
{
	return {g_binds.load(std::memory_order_relaxed),
		g_verifications.load(std::memory_order_relaxed),
		g_unreadable.load(std::memory_order_relaxed),
		g_poisoned.load(std::memory_order_relaxed),
		g_enters.load(std::memory_order_relaxed),
		g_steps.load(std::memory_order_relaxed),
		g_retired.load(std::memory_order_relaxed)};
}

void reset_stats() noexcept
{
	g_binds = 0;
	g_verifications = 0;
	g_unreadable = 0;
	g_poisoned = 0;
	g_enters = 0;
	g_steps = 0;
	g_retired = 0;
}

Binder::Verdict Binder::verify(::uml::memory::StaticGuestMemory &memory,
	const Instance &instance) noexcept
{
	g_verifications.fetch_add(1, std::memory_order_relaxed);
	const auto &dir = directory();
	const auto library = dir.library(instance.library);
	alignas(8) std::array<std::byte, page_size> page;
	for (std::size_t index = 0; index < library.pages; ++index) {
		const auto address =
			instance.bias + library.table_base + index * page_size;
		if (!memory.fetch(address, page))
			return Verdict::unreadable;
		if (page_digest(page.data()) !=
		    read<std::uint64_t>(library.digests + 8 * index))
			return Verdict::mismatch;
	}
	return Verdict::match;
}

std::int32_t Binder::resolve(::uml::memory::StaticGuestMemory &memory,
	unsigned long generation, std::uint64_t code_changes,
	std::uint64_t page_va) noexcept
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
		*found = Instance{generation, library_index, bias, ~code_changes, 0,
			0, false, true};
	}
	if (found->poisoned)
		return none;
	if (found->verified_changes != code_changes) {
		if (ticks_ < found->retry_at)
			return not_ready;
		switch (verify(memory, *found)) {
		case Verdict::match:
			found->verified_changes = code_changes;
			found->backoff = 0;
			break;
		case Verdict::unreadable:
			g_unreadable.fetch_add(1, std::memory_order_relaxed);
			found->backoff = std::min<std::uint64_t>(
				std::max<std::uint64_t>(found->backoff * 2, 64), 65536);
			found->retry_at = ticks_ + found->backoff;
			return not_ready;
		case Verdict::mismatch:
			found->poisoned = true;
			g_poisoned.fetch_add(1, std::memory_order_relaxed);
			return none;
		}
	}
	return static_cast<std::int32_t>(found - instances_.data());
}

Hit Binder::find(::uml::memory::StaticGuestMemory &memory,
	unsigned long generation, std::uint64_t code_changes,
	std::uint64_t pc) noexcept
{
	if (!directory().valid || (pc & 3U) != 0)
		return {};
	++ticks_;
	const auto page_va = pc & ~(page_size - 1);
	auto &slot = pages_[((page_va >> 12) ^ generation) % page_slots];
	if (slot.page != page_va || slot.generation != generation ||
	    slot.code_changes != code_changes) {
		g_binds.fetch_add(1, std::memory_order_relaxed);
		const auto answer =
			resolve(memory, generation, code_changes, page_va);
		if (answer == not_ready)
			return {};
		slot.instance = answer;
		slot.page = page_va;
		slot.generation = generation;
		slot.code_changes = code_changes;
	}
	if (slot.instance < 0)
		return {};
	return entry(static_cast<std::uint32_t>(slot.instance), pc);
}

Hit Binder::entry(std::uint32_t instance_index,
	std::uint64_t pc) const noexcept
{
	const auto &instance = instances_[instance_index];
	if (!instance.used || instance.poisoned)
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
	return {uml_aot_blob + native, instance.bias, instance_index};
}

EnterResult enter(CpuState &cpu, ::uml::memory::Tlb &tlb,
	std::uint64_t bias, const void *native, std::uint64_t budget) noexcept
{
	using Entry = EnterResult (*)(CpuState *, void *, std::uint64_t,
		const void *, std::uint64_t, std::uint64_t (*)(void *,
			std::uint64_t, std::uint64_t) noexcept);
	/* The probe indexes the entries from the pointer the refill gets. */
	if (static_cast<const void *>(tlb.entries.data()) !=
	    static_cast<const void *>(&tlb))
		std::abort();
	const auto function = reinterpret_cast<Entry>(
		const_cast<std::uint8_t *>(uml_aot_blob) + directory().enter);
	if ((g_enters.fetch_add(1, std::memory_order_relaxed) & 0xffff) == 0xffff)
		write_report();
	const auto result = function(&cpu, &tlb, bias, native, budget,
		uml_aot_refill);
	g_retired.fetch_add(result.retired, std::memory_order_relaxed);
	if (result.reason == Reason::step) {
		g_steps.fetch_add(1, std::memory_order_relaxed);
		if (g_step_histogram.load(std::memory_order_relaxed))
			note_step(cpu.pc - bias);
	}
	return result;
}

} // namespace uml::cpu::arm64::aot
