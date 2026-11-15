// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "memory/static_guest_memory.hpp"
#include "memory/tlb.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

/* Ahead-of-time native code for the Android image's own arm64 libraries
 * (docs/architecture.md, "Ahead-of-time code"). The framework holds one
 * generated blob (tools/aot/rewrite.py): the entry routine, one unit of
 * rewritten code per library with its entry table and page digests. This
 * header is the whole boundary the gadget executor sees. Counters, reports
 * and test switches are in aot_diagnostics.hpp and exist only in builds that
 * set UML_AOT_DIAGNOSTICS. */
namespace uml::cpu::arm64::aot {

/* Whether AOT code may be entered: DARWIN_UML_AOT=1 at start-up. Off by
 * default. */
[[nodiscard]] bool enabled() noexcept;

/* Why an entry was refused, for the diagnostics build's counters. */
enum class Refusal : unsigned {
	tbi_disabled,
	sp_misaligned,
	exclusive_monitor,
	fp_environment,
	count,
};
void note_refusal(Refusal reason) noexcept;
/* Where a diagnostics build's test hook runs: after the dispatcher read the
 * address space's sequences, before anything is looked up (nothing in a
 * production build). */
void note_run_start() noexcept;
/* Bracket the engine's execution of one instruction rewritten code left to
 * it (counted and timed in a diagnostics build only). */
[[nodiscard]] std::uint64_t step_begin() noexcept;
void step_end(std::uint64_t began) noexcept;

/* Why AOT code returned; the numbers are the generator's. */
enum class Reason : std::uint64_t {
	budget = 0,
	branch = 1,
	step = 2,
};

struct EnterResult final {
	Reason reason;
	std::uint64_t retired;
};

/* One guest instruction the rewritten code can be entered at. `pages` is
 * the state of the text pages of the instance it is in: byte p is 1 once
 * text page p was compared with the stored source (rewritten code reads it
 * where it leaves a page; the binder alone writes it, between entries). */
struct Hit final {
	const void *native{};
	std::uint64_t bias{};
	std::uint32_t instance{};
	std::uint32_t library{};
	const std::uint8_t *pages{};
};

/* A library instance rewritten code may branch to. The thunk generated for
 * a cross-library branch reads `bias` and `pages` (tools/aot/rewrite.py:
 * CROSS_BIAS, CROSS_PAGES). */
struct CrossEntry final {
	std::uint64_t low;
	std::uint64_t high;
	std::uint64_t bias;
	const std::uint8_t *pages;
	std::uint32_t library;
	std::uint32_t reserved;
};
static_assert(offsetof(CrossEntry, bias) == 16 &&
	offsetof(CrossEntry, pages) == 24);

/* The answer of a cross-library lookup: the native code of a guest address
 * in another library instance, and that instance's entry. */
struct Target final {
	const void *native;
	const CrossEntry *entry;
};

/* What rewritten code asks the host for, and the libraries it may branch to
 * without leaving: the instances of the entered address space. The first
 * three members are called by the generated refill, lookup and MOPS
 * thunks (tools/aot/rewrite.py: XTAB_*). */
struct CrossTable final {
	static constexpr std::size_t capacity = 64;
	using Entry = CrossEntry;
	/* Fill the TLB entry of an access: `address` is the address the probe
	 * formed (the last byte of an access of more than one byte) and `code`
	 * is 0 or 1 (read or write) | bytes << 8. Answers whether the entry now
	 * holds the page and the access fits in it. */
	std::uint64_t (*refill)(void *tlb, std::uint64_t address,
		std::uint64_t code) noexcept;
	Target (*lookup)(const CrossTable *table, std::uint64_t address) noexcept;
	/* The epilogue of a memory copy or set (FEAT_MOPS) when the thunk's own
	 * fast path does not apply. `registers` holds the destination, count and
	 * source (the fill value of a set) and the flags, in that order; the
	 * three are updated as the engine's epilogue does. `kind` is 0 for CPYF,
	 * 1 for CPY and 2 for SET. Answers 1 when the epilogue completed, 0 when
	 * it stopped at a fault (the registers are committed to the region the
	 * fault is in) and 2 when the registers cannot have come from the
	 * prologue. */
	std::uint64_t (*mops)(CpuState *cpu, ::uml::memory::Tlb *tlb,
		std::uint64_t *registers, std::uint64_t kind) noexcept;
	std::uint64_t count{};
	std::array<Entry, capacity> entries{};
};

/* The libraries an address space has mapped, identified by the digest of
 * their code pages. Each text page is compared, byte for byte, with the page
 * of the stored source before any rewritten instruction of it runs (when
 * find() is asked for an entry in it, and when rewritten code leaves another
 * page for it: it tests the page's state byte), and again after the address
 * space's code-change history names it. An instance with a page whose bytes
 * differ is never entered again in that address space generation; a page
 * that cannot be read yet is tried again later. One binder belongs to one
 * executor thread. */
class Binder final {
public:
	Binder() noexcept;

	/* The AOT entry for `pc`, or an empty Hit. `context` is the executor's
	 * execution context (it names the code-change history); `code_changes`
	 * is the sequence the dispatcher read. */
	[[nodiscard]] Hit find(::uml::memory::StaticGuestMemory &memory,
		void *context, unsigned long generation, std::uint64_t code_changes,
		std::uint64_t pc) noexcept;
	/* The entry for `pc` in an instance find() returned, or an empty Hit. */
	[[nodiscard]] Hit entry(std::uint32_t instance,
		std::uint64_t pc) const noexcept;
	/* The instances rewritten code may branch to: those usable in
	 * `generation`, current as of the sequence find() was given (call it
	 * after find()). */
	[[nodiscard]] const CrossTable &cross(unsigned long generation,
		std::uint64_t code_changes) noexcept;
	/* The library and offset a guest address belongs to, for reports. */
	[[nodiscard]] bool locate(unsigned long generation, std::uint64_t address,
		std::uint32_t &library, std::uint64_t &offset) const noexcept;

private:
	struct Instance final {
		unsigned long generation{};
		std::uint32_t library{};
		std::uint64_t bias{};
		bool poisoned{};
		bool used{};
		/* One byte per text page: 1 = compared with the stored source and
		 * not named by a code change since; as of `state_changes`. */
		std::vector<std::uint8_t> pages{};
		std::uint64_t state_changes{};
	};
	struct PageSlot final {
		unsigned long generation{};
		std::uint64_t stamp{};
		std::uint64_t page{1};
		std::int32_t instance{-1};
		/* An unreadable page is not tried again before this many lookups. */
		std::uint64_t retry_at{};
		std::uint64_t backoff{};
	};

	/* resolve() answers an instance index, none (not a library page, or a
	 * poisoned instance: remembered), or not_ready (do not remember). */
	static constexpr std::int32_t none = -1;
	static constexpr std::int32_t not_ready = -2;
	enum class Verdict { match, unreadable, mismatch };
	[[nodiscard]] std::int32_t resolve(
		::uml::memory::StaticGuestMemory &memory, void *context,
		unsigned long generation, std::uint64_t code_changes,
		std::uint64_t page) noexcept;
	/* Bring the page states of every instance of `generation` up to
	 * `code_changes`: pages the code-change history names since an
	 * instance's last sequence are cleared. */
	void sync(void *context, unsigned long generation,
		std::uint64_t code_changes) noexcept;
	/* Compare text page `index` of the instance with the stored source. */
	[[nodiscard]] Verdict verify_page(
		::uml::memory::StaticGuestMemory &memory, Instance &instance,
		std::uint64_t index) noexcept;
	/* Whether a code change after `sequence` touches [low, high). */
	[[nodiscard]] static bool changed_since(void *context,
		std::uint64_t sequence, std::uint64_t low, std::uint64_t high,
		std::uint64_t &current) noexcept;

	static constexpr std::size_t instance_capacity = CrossTable::capacity;
	static constexpr std::size_t page_slots = 2048;
	std::array<Instance, instance_capacity> instances_{};
	std::array<PageSlot, page_slots> pages_{};
	std::size_t next_instance_{};
	std::uint64_t ticks_{};
	/* The last (generation, sequence) sync() brought the instances to. */
	unsigned long sync_generation_{};
	std::uint64_t sync_changes_{};
	/* The last code-change sequence find() was given (a diagnostics build
	 * counts how often it moves). The layout is the same in both
	 * compilations of the runtime, which share the executor's object. */
	[[maybe_unused]] std::uint64_t last_changes_{};
	/* Moves whenever the set of usable instances changes. */
	std::uint64_t epoch_{1};
	CrossTable cross_{};
	unsigned long cross_generation_{};
	std::uint64_t cross_epoch_{};
	std::uint64_t cross_changes_{};
};

/* Run rewritten code from `hit` until it leaves. `budget` bounds the
 * instructions it may retire before it returns. The guest registers, vectors
 * and flags are committed in `cpu` on every return. */
[[nodiscard]] EnterResult enter(CpuState &cpu, ::uml::memory::Tlb &tlb,
	const Hit &hit, std::uint64_t budget, const CrossTable &table) noexcept;

} // namespace uml::cpu::arm64::aot
