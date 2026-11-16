// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/aot/aot_runtime.hpp"

#include "cpu/arm64/aot/shared_verdicts.hpp"
#include "cpu/arm64/detail/memory_copy_set.hpp"

#include <CommonCrypto/CommonDigest.h>
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
#include <pthread.h>
#include <signal.h>
#include <thread>
#endif

/* The framework's blob (aot_blob.S). */
extern "C" const std::uint8_t uml_aot_blob[];

namespace uml::cpu::arm64::aot {
namespace {

constexpr std::uint64_t page_size = 4096;
/* The verdicts of every address space of the process (shared_verdicts.hpp).
 * A verdict names a physical-page record that is never reused, so an entry
 * can outlive the address space that filed it and never be answered wrongly. */
VerdictTable g_verdicts;
constexpr char blob_magic[8] = {'U', 'M', 'L', 'A', 'O', 'T', 'B', '2'};
/* The magic names the layout, and the word after it the version of that
 * layout: a change an older runtime would misread changes the magic, and an
 * addition it can ignore raises the version, which a runtime accepts at or
 * above the one it was built for (tools/aot/rewrite.py: BLOB_VERSION). */
constexpr std::size_t header_version = 8;
constexpr std::uint32_t blob_version = 1;
/* The stored hash of a text page: SHA-256 (tools/aot/rewrite.py:
 * page_hashes). */
constexpr std::size_t page_hash_size = CC_SHA256_DIGEST_LENGTH;

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
constexpr std::size_t record_pages = 180;
constexpr std::size_t record_unit_offset = 200;
constexpr std::size_t record_page_hashes = 204;

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
	std::size_t pages;
	std::size_t unit_offset;
	/* The SHA-256 of each text page the library was built from. */
	std::size_t page_hashes;
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
			read<std::uint32_t>(base + record_pages),
			read<std::uint32_t>(base + record_unit_offset),
			read<std::uint32_t>(base + record_page_hashes)};
	}
};

[[nodiscard]] const Directory &directory() noexcept
{
	static const Directory value = []() noexcept {
		Directory d;
		if (std::memcmp(uml_aot_blob, blob_magic, sizeof(blob_magic)) != 0)
			return d;
		if (read<std::uint32_t>(header_version) < blob_version)
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

/* What Binder::find answered: an entry, or why it found none. */
enum class FindResult : unsigned {
	hit,
	/* The page is not a page of a covered library (any other code, or
	 * data). */
	uncovered,
	/* The page could not be read. */
	not_ready,
	/* The library instance holds bytes other than the framework's. */
	poisoned,
	/* The instance is not verified (a page unreadable, a retry pending). */
	unverified,
	/* A verified instance, but not an instruction the framework has an
	 * entry for. */
	no_entry,
	/* The instance the page was bound to was replaced. */
	stale,
};

#if UML_AOT_DIAGNOSTICS
std::atomic<bool> g_refill_stress{false};
std::atomic<void (*)(void *)> g_run_hook{nullptr};
std::atomic<void *> g_run_hook_context{nullptr};
std::atomic<std::uint64_t> g_learning_epoch{0};

/* Leave every general and vector register the caller could rely on holding
 * something else: the refill thunk saves x0-x17, x30, NZCV and q0-q31 around
 * the call and must hand them back as they were. Callee-saved registers are
 * the compiler's to restore (the upper halves of v8-v15 are not). */
[[gnu::noinline]] void scramble_registers() noexcept
{
	asm volatile(
		"movz x0, #0xdead\n movk x0, #0xbeef, lsl #16\n"
		"mov x1, x0\n mov x2, x0\n mov x3, x0\n mov x4, x0\n"
		"mov x5, x0\n mov x6, x0\n mov x7, x0\n mov x8, x0\n"
		"mov x9, x0\n mov x10, x0\n mov x11, x0\n mov x12, x0\n"
		"mov x13, x0\n mov x14, x0\n mov x15, x0\n mov x16, x0\n"
		"mov x17, x0\n"
		"movi v0.16b, #0xa5\n movi v1.16b, #0xa5\n movi v2.16b, #0xa5\n"
		"movi v3.16b, #0xa5\n movi v4.16b, #0xa5\n movi v5.16b, #0xa5\n"
		"movi v6.16b, #0xa5\n movi v7.16b, #0xa5\n movi v8.16b, #0xa5\n"
		"movi v9.16b, #0xa5\n movi v10.16b, #0xa5\n movi v11.16b, #0xa5\n"
		"movi v12.16b, #0xa5\n movi v13.16b, #0xa5\n movi v14.16b, #0xa5\n"
		"movi v15.16b, #0xa5\n movi v16.16b, #0xa5\n movi v17.16b, #0xa5\n"
		"movi v18.16b, #0xa5\n movi v19.16b, #0xa5\n movi v20.16b, #0xa5\n"
		"movi v21.16b, #0xa5\n movi v22.16b, #0xa5\n movi v23.16b, #0xa5\n"
		"movi v24.16b, #0xa5\n movi v25.16b, #0xa5\n movi v26.16b, #0xa5\n"
		"movi v27.16b, #0xa5\n movi v28.16b, #0xa5\n movi v29.16b, #0xa5\n"
		"movi v30.16b, #0xa5\n movi v31.16b, #0xa5\n"
		"msr nzcv, x0\n"
		:
		:
		: "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
		  "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "v0",
		  "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10",
		  "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19",
		  "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28",
		  "v29", "v30", "v31", "cc");
}

[[nodiscard]] const char *find_result_name(std::size_t kind) noexcept
{
	constexpr const char *names[] = {"hit", "uncovered", "not_ready",
		"poisoned", "unverified", "no_entry", "stale"};
	return names[kind];
}

namespace counters {
std::atomic<std::uint64_t> binds{0}, verifications{0}, pages_hashed{0},
	unreadable{0}, poisoned{0}, enters{0}, steps{0}, retired{0}, refills{0},
	refill_failures{0}, cross_hits{0}, cross_misses{0}, max_retired{0},
	verify_nanos{0}, step_nanos{0}, steps_run{0}, code_bumps{0},
	history_queries{0}, mops_slow{0}, mops_fault{0}, mops_illegal{0},
	shared_hits{0};
std::array<std::atomic<std::uint64_t>, 5> refusals{};
/* The guest addresses AOT code stepped at most: library << 40 | offset. */
constexpr std::size_t step_slots = 32768;
std::array<std::atomic<std::uint64_t>, step_slots> step_key{};
std::array<std::atomic<std::uint64_t>, step_slots> step_count{};
std::array<std::atomic<std::uint64_t>, 64> library_enters{};
std::array<std::atomic<std::uint64_t>, 64> library_steps{};
/* Pages of a library compared with their stored hash, and pages whose
 * verification the shared table answered. */
std::array<std::atomic<std::uint64_t>, 64> library_verifications{};
std::array<std::atomic<std::uint64_t>, 64> library_shared_hits{};
/* What find() answered: an entry, or why not (FindResult), in all and per
 * library where the answer names one; and what a cross-library lookup that
 * found no entry looked at (CrossMiss). */
std::array<std::atomic<std::uint64_t>, 7> find_results{};
std::array<std::array<std::atomic<std::uint64_t>, 7>, 64> library_finds{};
std::array<std::atomic<std::uint64_t>, 3> cross_miss_kinds{};
std::array<std::atomic<std::uint64_t>, 64> library_cross_misses{};
std::atomic<bool> reporter_started{false};
/* The least stack any sampled thread had left at the points where the
 * runtime is deepest (entry, refill, page verification), and the stack size
 * of the thread that had it. */
std::atomic<std::uint64_t> stack_min_free{~std::uint64_t{0}};
std::atomic<std::uint64_t> stack_size_at_min{0};
} // namespace counters

/* A Linux task stack: CONFIG_KERNEL_STACK_ORDER 4 at 4 KiB pages, in guest
 * memory (never a host stack), aligned to its size like every page-order
 * allocation. The guest kernel's threads run the executor on one. */
constexpr std::uint64_t kernel_stack_bytes = 64 * 1024;

/* Record how much of the stack the caller's frame is on is left. A frame on
 * the thread's own host stack or its signal stack measures against that
 * stack; any other is a Linux task stack, whose base is the frame rounded
 * down to the stack size. */
#define UML_AOT_SAMPLE_STACK() sample_stack(__builtin_frame_address(0))
#define UML_AOT_SAMPLE_STACK_AT(lowest) sample_stack(lowest)

[[gnu::noinline]] void sample_stack(const void *frame) noexcept
{
	struct HostStacks {
		std::uintptr_t low[2];
		std::uintptr_t high[2];
	};
	thread_local const HostStacks host = [] {
		HostStacks stacks{};
		const auto self = pthread_self();
		const auto top = reinterpret_cast<std::uintptr_t>(
			pthread_get_stackaddr_np(self));
		stacks.high[0] = top;
		stacks.low[0] = top - pthread_get_stacksize_np(self);
		stack_t alternate;
		if (sigaltstack(nullptr, &alternate) == 0 &&
		    (alternate.ss_flags & SS_DISABLE) == 0) {
			stacks.low[1] = reinterpret_cast<std::uintptr_t>(
				alternate.ss_sp);
			stacks.high[1] = stacks.low[1] + alternate.ss_size;
		}
		return stacks;
	}();
	const auto here = reinterpret_cast<std::uintptr_t>(frame);
	std::uint64_t left = here & (kernel_stack_bytes - 1);
	std::uint64_t size = kernel_stack_bytes;
	for (int i = 0; i < 2; ++i) {
		if (here >= host.low[i] && here < host.high[i]) {
			left = here - host.low[i];
			size = host.high[i] - host.low[i];
			break;
		}
	}
	auto least = counters::stack_min_free.load(std::memory_order_relaxed);
	while (left < least) {
		if (counters::stack_min_free.compare_exchange_weak(
		        least, left, std::memory_order_relaxed)) {
			counters::stack_size_at_min.store(size,
				std::memory_order_relaxed);
			break;
		}
	}
}

#define UML_AOT_COUNT(name, amount) \
	counters::name.fetch_add((amount), std::memory_order_relaxed)
#define UML_AOT_COUNT_LIBRARY(name, library) \
	do { \
		if ((library) < 64) \
			counters::name[(library)].fetch_add(1, \
				std::memory_order_relaxed); \
	} while (0)

constexpr std::uint32_t no_library = 0xffffffffU;

void note_find(FindResult result, std::uint32_t library) noexcept
{
	counters::find_results[static_cast<std::size_t>(result)].fetch_add(
		1, std::memory_order_relaxed);
	if (library < 64)
		counters::library_finds[library][static_cast<std::size_t>(result)]
			.fetch_add(1, std::memory_order_relaxed);
}

/* The binder whose table the running vCPU thread's rewritten code uses. */
thread_local const Binder *g_cross_owner = nullptr;
thread_local unsigned long g_cross_generation = 0;

/* A cross-library lookup found no entry: inside a verified library but
 * not at an instruction it has one for (2), in a library instance the
 * table does not hold (1; unverified, or not verified at this sequence),
 * or in no instance this thread knows (0). */
void note_cross_miss(const CrossTable *table, std::uint64_t address) noexcept
{
	std::size_t kind = 0;
	std::uint32_t library = no_library;
	for (std::uint64_t index = 0; index < table->count; ++index) {
		const auto &entry = table->entries[index];
		if (address >= entry.low && address < entry.high) {
			kind = 2;
			library = entry.library;
			break;
		}
	}
	std::uint64_t offset = 0;
	if (kind == 0 && g_cross_owner != nullptr &&
	    g_cross_owner->locate(g_cross_generation, address, library, offset))
		kind = 1;
	counters::cross_miss_kinds[kind].fetch_add(1, std::memory_order_relaxed);
	if (library < 64)
		counters::library_cross_misses[library].fetch_add(
			1, std::memory_order_relaxed);
}

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
	std::fprintf(file,
		"max_retired %llu verify_nanos %llu step_nanos %llu steps_run %llu "
		"code_bumps %llu history_queries %llu mops_slow %llu mops_fault %llu "
		"mops_illegal %llu shared_hits %llu\n",
		get(counters::max_retired), get(counters::verify_nanos),
		get(counters::step_nanos), get(counters::steps_run),
		get(counters::code_bumps), get(counters::history_queries),
		get(counters::mops_slow), get(counters::mops_fault),
		get(counters::mops_illegal), get(counters::shared_hits));
	std::fprintf(file, "stack_free %llu stack_size %llu\n",
		get(counters::stack_min_free), get(counters::stack_size_at_min));
	std::fprintf(file, "refused tbi %llu sp %llu exclusive %llu fp %llu "
		"short %llu\n",
		get(counters::refusals[0]), get(counters::refusals[1]),
		get(counters::refusals[2]), get(counters::refusals[3]),
		get(counters::refusals[4]));
	std::fprintf(file, "find");
	for (std::size_t kind = 0; kind < counters::find_results.size(); ++kind)
		std::fprintf(file, " %s %llu", find_result_name(kind),
			get(counters::find_results[kind]));
	std::fprintf(file, "\ncross_miss uncovered %llu unbound %llu no_entry "
		"%llu\n", get(counters::cross_miss_kinds[0]),
		get(counters::cross_miss_kinds[1]),
		get(counters::cross_miss_kinds[2]));
	for (std::size_t library = 0; library < 64; ++library) {
		const auto enters = get(counters::library_enters[library]);
		unsigned long long finds = 0;
		for (const auto &counter : counters::library_finds[library])
			finds += get(counter);
		const auto cross = get(counters::library_cross_misses[library]);
		const auto hashed = get(counters::library_verifications[library]);
		const auto shared = get(counters::library_shared_hits[library]);
		if (enters == 0 && finds == 0 && cross == 0 && hashed == 0 &&
		    shared == 0)
			continue;
		std::fprintf(file, "library %zu enters %llu steps %llu cross_misses "
			"%llu verified %llu shared %llu", library, enters,
			get(counters::library_steps[library]), cross, hashed, shared);
		for (std::size_t kind = 0;
		     kind < counters::library_finds[library].size(); ++kind)
			std::fprintf(file, " %s %llu", find_result_name(kind),
				get(counters::library_finds[library][kind]));
		std::fprintf(file, "\n");
	}
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
inline void note_find(FindResult, std::uint32_t) noexcept {}
#define UML_AOT_COUNT_LIBRARY(name, library) ((void)0)
#define UML_AOT_SAMPLE_STACK() ((void)0)
#define UML_AOT_SAMPLE_STACK_AT(lowest) ((void)0)
constexpr std::uint32_t no_library = 0xffffffffU;
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
 * page also steps: the probe found it as a miss. This is noexcept although
 * Tlb::translate is not: the thunk that calls it has no unwind information,
 * so an exception could not be handled there, and one that escaped the fill
 * would end the process (std::terminate) rather than reach the guest. */
std::uint64_t refill(void *tlb_pointer, std::uint64_t address,
	std::uint64_t code) noexcept
{
	UML_AOT_COUNT(refills, 1);
	UML_AOT_SAMPLE_STACK();
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
	const bool filled = (kind == ::uml::memory::Access::write ?
		entry.write_page : entry.read_page) == page;
#if UML_AOT_DIAGNOSTICS
	if (filled && g_refill_stress.load(std::memory_order_relaxed)) {
		/* Leave only this access's entry in the TLB. */
		for (auto &other : tlb.entries)
			other = ::uml::memory::TlbEntry{};
		if (tlb.translate(address, kind) == nullptr)
			return 0;
		scramble_registers();
	}
#endif
	return filled ? 1 : 0;
}

/* The epilogue of a memory copy or set that the thunk of its registers did
 * not carry out (tools/aot/rewrite.py: emit_mops_thunks): the engine's own
 * routine on the registers the prologue formatted. */
std::uint64_t mops(CpuState *cpu, ::uml::memory::Tlb *tlb,
	std::uint64_t *registers, std::uint64_t kind) noexcept
{
	using namespace ::uml::cpu::arm64::detail;
	UML_AOT_COUNT(mops_slow, 1);
	cpu->nzcv = static_cast<std::uint32_t>(registers[3]);
	MopsOutcome outcome;
	switch (kind) {
	case 0:
		outcome = mops_epilogue<MopsFamily::copy_forward>(*cpu, *tlb,
			registers[0], registers[1], registers[2]);
		break;
	case 1:
		outcome = mops_epilogue<MopsFamily::copy_general>(*cpu, *tlb,
			registers[0], registers[1], registers[2]);
		break;
	default:
		outcome = mops_epilogue<MopsFamily::set>(*cpu, *tlb,
			registers[0], registers[1], registers[2]);
		break;
	}
	if (outcome == MopsOutcome::fault)
		UML_AOT_COUNT(mops_fault, 1);
	else if (outcome != MopsOutcome::complete)
		UML_AOT_COUNT(mops_illegal, 1);
	return outcome == MopsOutcome::complete ? 1 :
		outcome == MopsOutcome::fault ? 0 : 2;
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
		/* The target's text page must have matched its stored hash; a
		 * page not yet verified ends the entry at the branch, and the
		 * host verifies it before the next one. */
		if (entry.pages[(offset - library.table_base) / page_size] == 0)
			break;
		const auto native = read<std::uint32_t>(library.table_offset +
			4 * ((offset - library.table_base) / 4));
		if (native == 0)
			break;
		UML_AOT_COUNT(cross_hits, 1);
		return {uml_aot_blob + library.unit_offset + native, &entry};
	}
	UML_AOT_COUNT(cross_misses, 1);
#if UML_AOT_DIAGNOSTICS
	note_cross_miss(table, address);
#endif
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

void note_run_start() noexcept
{
#if UML_AOT_DIAGNOSTICS
	if (const auto hook = g_run_hook.load(std::memory_order_acquire))
		hook(g_run_hook_context.load(std::memory_order_relaxed));
#endif
}

std::uint64_t learning_epoch() noexcept
{
#if UML_AOT_DIAGNOSTICS
	return g_learning_epoch.load(std::memory_order_relaxed);
#else
	return 0;
#endif
}

std::uint64_t step_begin() noexcept
{
#if UML_AOT_DIAGNOSTICS
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
#else
	return 0;
#endif
}

void step_end(std::uint64_t began) noexcept
{
#if UML_AOT_DIAGNOSTICS
	counters::step_nanos.fetch_add(step_begin() - began,
		std::memory_order_relaxed);
	counters::steps_run.fetch_add(1, std::memory_order_relaxed);
#else
	(void)began;
#endif
}

#if UML_AOT_DIAGNOSTICS
namespace diagnostics {

void set_enabled(std::optional<bool> state) noexcept
{
	g_learning_epoch.fetch_add(1, std::memory_order_relaxed);
	g_state.store(!state.has_value() ? -1 :
		(*state && directory().valid ? 1 : 0), std::memory_order_relaxed);
}

void set_refill_stress(bool on) noexcept
{
	g_refill_stress.store(on, std::memory_order_relaxed);
}

void set_run_hook(void (*hook)(void *), void *context) noexcept
{
	g_run_hook_context.store(context, std::memory_order_relaxed);
	g_run_hook.store(hook, std::memory_order_release);
}

void clear_shared_verdicts() noexcept
{
	g_verdicts.clear();
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
	result.max_retired = get(counters::max_retired);
	result.verify_nanos = get(counters::verify_nanos);
	result.step_nanos = get(counters::step_nanos);
	result.steps_run = get(counters::steps_run);
	result.code_bumps = get(counters::code_bumps);
	result.history_queries = get(counters::history_queries);
	result.mops_slow = get(counters::mops_slow);
	result.mops_fault = get(counters::mops_fault);
	result.mops_illegal = get(counters::mops_illegal);
	for (std::size_t i = 0; i < result.finds.size(); ++i)
		result.finds[i] = get(counters::find_results[i]);
	result.shared_hits = get(counters::shared_hits);
	return result;
}

void reset_stats() noexcept
{
	for (auto *counter : {&counters::binds, &counters::verifications,
		     &counters::pages_hashed, &counters::unreadable,
		     &counters::poisoned, &counters::enters, &counters::steps,
		     &counters::retired, &counters::refills,
		     &counters::refill_failures, &counters::cross_hits,
		     &counters::cross_misses, &counters::max_retired,
		     &counters::verify_nanos, &counters::step_nanos,
		     &counters::steps_run, &counters::code_bumps,
		     &counters::history_queries, &counters::mops_slow,
		     &counters::mops_fault, &counters::mops_illegal,
		     &counters::shared_hits})
		counter->store(0, std::memory_order_relaxed);
	for (auto &counter : counters::refusals)
		counter.store(0, std::memory_order_relaxed);
	for (auto &counter : counters::find_results)
		counter.store(0, std::memory_order_relaxed);
	for (auto &row : counters::library_finds)
		for (auto &counter : row)
			counter.store(0, std::memory_order_relaxed);
	for (auto &counter : counters::cross_miss_kinds)
		counter.store(0, std::memory_order_relaxed);
	for (auto &counter : counters::library_cross_misses)
		counter.store(0, std::memory_order_relaxed);
}

} // namespace diagnostics
#endif

Binder::Binder() noexcept
{
	cross_.refill = &refill;
	cross_.lookup = &lookup;
	cross_.mops = &mops;
}

bool Binder::changed_since(void *context, std::uint64_t sequence,
	std::uint64_t low, std::uint64_t high, std::uint64_t &current) noexcept
{
	if (context == nullptr)
		return true;
	UML_AOT_COUNT(history_queries, 1);
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

Binder::Verdict Binder::verify_page(
	::uml::memory::StaticGuestMemory &memory, Instance &instance,
	std::uint64_t index) noexcept
{
	const auto library = directory().library(instance.library);
	const auto address =
		instance.bias + library.table_base + index * page_size;
	const auto outcome = verify_through_table(g_verdicts,
		[&](std::uint64_t at, PageStamp &stamp) noexcept {
#if !UML_AOT_SHARED_VERDICTS
			(void)at;
			(void)stamp;
			return false;
#else
			::uml::memory::ExtendedCodePageStamp seen;
			if (!memory.extended_code_page_stamp(at, nullptr, seen))
				return false;
			stamp = {seen.stamp.identity, seen.epoch, seen.shareable};
			return true;
#endif
		},
		[&]() noexcept {
			alignas(8) std::array<std::byte, page_size> page;
			if (!memory.fetch(address, page))
				return SharedOutcome::unreadable;
			UML_AOT_COUNT(verifications, 1);
			UML_AOT_COUNT_LIBRARY(library_verifications, instance.library);
			UML_AOT_SAMPLE_STACK_AT(page.data());
			/* The stored page is a SHA-256 of the bytes the code was built
			 * from: rewritten code runs for other bytes only on a collision
			 * of the hash. CC_SHA256 is CommonCrypto's one-shot digest,
			 * part of libSystem on macOS, iOS and the iOS Simulator, and
			 * runs on the SHA-2 instructions of the host. */
			std::array<std::uint8_t, page_hash_size> hash;
			CC_SHA256(page.data(), static_cast<CC_LONG>(page_size),
				hash.data());
			return std::memcmp(hash.data(), uml_aot_blob +
				library.page_hashes + index * page_hash_size,
				page_hash_size) == 0 ? SharedOutcome::match :
				SharedOutcome::mismatch;
		},
		address, VerdictTable::where(instance.library,
			static_cast<std::uint32_t>(index)));
	switch (outcome) {
	case SharedOutcome::unreadable:
		return Verdict::unreadable;
	case SharedOutcome::mismatch:
		return Verdict::mismatch;
	case SharedOutcome::shared_hit:
		UML_AOT_COUNT(shared_hits, 1);
		UML_AOT_COUNT_LIBRARY(library_shared_hits, instance.library);
		break;
	case SharedOutcome::match:
		break;
	}
	instance.pages[index] = 1;
	return Verdict::match;
}

void Binder::sync(void *context, unsigned long generation,
	std::uint64_t code_changes) noexcept
{
	if (sync_generation_ == generation && sync_changes_ == code_changes)
		return;
	darwin_uml_static_code_change ranges[32];
	std::uint64_t ranges_from = 0;
	int count = 0;
	bool have = false;
	for (auto &instance : instances_) {
		if (!instance.used || instance.generation != generation ||
		    instance.poisoned || instance.state_changes == code_changes)
			continue;
		/* The history answers what was published after a sequence, up to
		 * the present, so a change newer than code_changes clears a page
		 * early and never late. A sequence from the future or a history
		 * that no longer reaches back clears everything. */
		const auto from = instance.state_changes;
		if (!have || ranges_from != from) {
			std::uint64_t current = 0;
			count = context == nullptr || from > code_changes ? -1 :
				darwin_uml_static_vm_execution_context_code_change_ranges(
					context, from, ranges, 32, &current);
			ranges_from = from;
			have = true;
			UML_AOT_COUNT(history_queries, 1);
		}
		const auto library = directory().library(instance.library);
		const std::uint64_t low = instance.bias + library.table_base;
		const std::uint64_t high = instance.bias + library.table_end;
		if (count < 0) {
			std::fill(instance.pages.begin(), instance.pages.end(), 0);
		} else {
			for (int index = 0; index < count; ++index) {
				std::uint64_t first = ranges[index].address;
				std::uint64_t last = first + ranges[index].length;
				/* A range of length zero names the whole address space. */
				if (ranges[index].length == 0) {
					first = low;
					last = high;
				}
				if (first >= high || last <= low)
					continue;
				first = std::max(first, low);
				last = std::min(last, high);
				for (auto page = (first - low) / page_size;
				     page < (last - low + page_size - 1) / page_size; ++page)
					instance.pages[page] = 0;
			}
		}
		instance.state_changes = code_changes;
	}
	sync_generation_ = generation;
	sync_changes_ = code_changes;
}

std::int32_t Binder::resolve(::uml::memory::StaticGuestMemory &memory,
	void *, unsigned long generation, std::uint64_t code_changes,
	std::uint64_t page_va) noexcept
{
	const auto &dir = directory();
	alignas(8) std::array<std::byte, page_size> page;
	if (!memory.fetch(page_va, page))
		return not_ready;
	UML_AOT_COUNT(pages_hashed, 1);
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
		*found = Instance{};
		found->generation = generation;
		found->library = library_index;
		found->bias = bias;
		found->used = true;
		found->pages.assign(library.pages, 0);
		found->state_changes = code_changes;
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
#if UML_AOT_DIAGNOSTICS
	if (code_changes != last_changes_) {
		UML_AOT_COUNT(code_bumps, 1);
		last_changes_ = code_changes;
	}
#endif
	/* Every instance of the address space is current before any is used. */
	sync(context, generation, code_changes);
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
		if (answer == not_ready) {
			note_find(FindResult::not_ready, no_library);
			return {};
		}
		slot.instance = answer;
		slot.page = page_va;
		slot.generation = generation;
		slot.stamp = code_changes;
	}
	if (slot.instance < 0) {
		note_find(FindResult::uncovered, no_library);
		return {};
	}
	auto &instance = instances_[static_cast<std::size_t>(slot.instance)];
	if (!instance.used || instance.poisoned ||
	    instance.generation != generation) {
		note_find(instance.poisoned ? FindResult::poisoned :
			FindResult::stale, instance.poisoned ? instance.library :
			no_library);
		slot = PageSlot{};
		return {};
	}
	/* The page of `pc` must have matched its stored hash. */
	const auto record = directory().library(instance.library);
	const std::uint64_t page_index =
		(pc - instance.bias - record.table_base) / page_size;
	if (pc - instance.bias < record.table_base ||
	    page_index >= instance.pages.size()) {
		note_find(FindResult::no_entry, instance.library);
		return {};
	}
	if (instance.pages[page_index] == 0) {
		if (ticks_ < slot.retry_at) {
			note_find(FindResult::unverified, instance.library);
			return {};
		}
#if UML_AOT_DIAGNOSTICS
		const auto began = std::chrono::steady_clock::now();
#endif
		const auto verdict = verify_page(memory, instance, page_index);
#if UML_AOT_DIAGNOSTICS
		counters::verify_nanos.fetch_add(static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - began).count()),
			std::memory_order_relaxed);
#endif
		switch (verdict) {
		case Verdict::match:
			slot.backoff = 0;
			break;
		case Verdict::unreadable:
			UML_AOT_COUNT(unreadable, 1);
			slot.backoff = std::min<std::uint64_t>(
				std::max<std::uint64_t>(slot.backoff * 2, 64), 65536);
			slot.retry_at = ticks_ + slot.backoff;
			note_find(FindResult::unverified, instance.library);
			return {};
		case Verdict::mismatch:
			instance.poisoned = true;
			UML_AOT_COUNT(poisoned, 1);
			++epoch_;
			note_find(FindResult::poisoned, instance.library);
			return {};
		}
	}
	const auto found =
		entry(static_cast<std::uint32_t>(slot.instance), pc);
	note_find(found.native != nullptr ? FindResult::hit :
		FindResult::no_entry, instance.library);
	return found;
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
	return {uml_aot_blob + library.unit_offset + native, instance.bias,
		instance_index, instance.library, instance.pages.data()};
}

const CrossTable &Binder::cross(unsigned long generation,
	std::uint64_t code_changes) noexcept
{
#if UML_AOT_DIAGNOSTICS
	g_cross_owner = this;
	g_cross_generation = generation;
#endif
	if (cross_generation_ == generation && cross_epoch_ == epoch_ &&
	    cross_changes_ == code_changes)
		return cross_;
	cross_.count = 0;
	for (const auto &instance : instances_) {
		if (!instance.used || instance.poisoned ||
		    instance.generation != generation ||
		    instance.state_changes != code_changes)
			continue;
		const auto library = directory().library(instance.library);
		cross_.entries[cross_.count++] = {
			instance.bias + library.table_base,
			instance.bias + library.table_end, instance.bias,
			instance.pages.data(), instance.library, 0};
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
		const void *, std::uint64_t, const CrossTable *,
		const std::uint8_t *);
	/* The probe indexes the entries from the pointer the refill gets. */
	if (static_cast<const void *>(tlb.entries.data()) !=
	    static_cast<const void *>(&tlb))
		std::abort();
	const auto function = reinterpret_cast<Entry>(
		const_cast<std::uint8_t *>(uml_aot_blob) + directory().enter);
	UML_AOT_COUNT(enters, 1);
	UML_AOT_SAMPLE_STACK();
#if UML_AOT_DIAGNOSTICS
	if (hit.library < 64)
		counters::library_enters[hit.library].fetch_add(
			1, std::memory_order_relaxed);
#endif
	const auto result = function(&cpu, &tlb, hit.bias, hit.native, budget,
		&table, hit.pages);
	UML_AOT_COUNT(retired, result.retired);
#if UML_AOT_DIAGNOSTICS
	{
		auto seen = counters::max_retired.load(std::memory_order_relaxed);
		while (seen < result.retired &&
		       !counters::max_retired.compare_exchange_weak(
			       seen, result.retired, std::memory_order_relaxed)) {
		}
	}
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
