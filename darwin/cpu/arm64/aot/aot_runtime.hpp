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

/* One guest instruction the rewritten code can be entered at. */
struct Hit final {
	const void *native{};
	std::uint64_t bias{};
	std::uint32_t instance{};
	std::uint32_t library{};
};

/* The answer of a cross-library lookup: the native code of a guest address
 * in another verified library instance and that instance's load bias. */
struct Target final {
	const void *native;
	std::uint64_t bias;
};

/* What rewritten code asks the host for, and the libraries it may branch to
 * without leaving: the verified instances of the entered address space. The
 * first two members are read by the generated refill and lookup thunks. */
struct CrossTable final {
	static constexpr std::size_t capacity = 64;
	struct Entry final {
		std::uint64_t low;
		std::uint64_t high;
		std::uint64_t bias;
		std::uint32_t library;
		std::uint32_t reserved;
	};
	/* Fill the TLB entry of an access: `address` is the address the probe
	 * formed (the last byte of an access of more than one byte) and `code`
	 * is 0 or 1 (read or write) | bytes << 8. Answers whether the entry now
	 * holds the page and the access fits in it. */
	std::uint64_t (*refill)(void *tlb, std::uint64_t address,
		std::uint64_t code) noexcept;
	Target (*lookup)(const CrossTable *table, std::uint64_t address) noexcept;
	std::uint64_t count{};
	std::array<Entry, capacity> entries{};
};

/* The libraries an address space has mapped, identified by the digest of
 * their code pages and verified before any of their code runs. An instance
 * whose bytes differ is never entered again in that address space generation;
 * one whose pages cannot be read yet is tried again later. After the first
 * full verification an instance is re-verified only for the pages that the
 * address space's code-change history names. One binder belongs to one
 * executor thread. */
class Binder final {
public:
	Binder() noexcept;

	/* The AOT entry for `pc`, or an empty Hit. `context` is the executor's
	 * execution context (it names the code-change history). */
	[[nodiscard]] Hit find(::uml::memory::StaticGuestMemory &memory,
		void *context, unsigned long generation, std::uint64_t code_changes,
		std::uint64_t pc) noexcept;
	/* The entry for `pc` in an instance find() returned, or an empty Hit. */
	[[nodiscard]] Hit entry(std::uint32_t instance,
		std::uint64_t pc) const noexcept;
	/* The instances rewritten code may branch to, as of `code_changes`. */
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
		/* The code-change sequence the instance's bytes were last known
		 * to match at. */
		std::uint64_t verified_changes{};
		/* Not tried again before this many lookups (an unreadable page
		 * stays unreadable for a while). */
		std::uint64_t retry_at{};
		std::uint64_t backoff{};
		bool poisoned{};
		bool used{};
		bool verified{};
		/* One bit per text page: known to equal the stored text as of
		 * `matched_changes`. */
		std::vector<std::uint64_t> matched{};
		std::uint64_t matched_changes{};
	};
	struct PageSlot final {
		unsigned long generation{};
		std::uint64_t stamp{};
		std::uint64_t page{1};
		std::int32_t instance{-1};
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
	/* Bring the instance up to `code_changes`: compare its pages with the
	 * stored text the first time, then only the pages the history names and
	 * those an earlier attempt could not read. */
	[[nodiscard]] Verdict verify(::uml::memory::StaticGuestMemory &memory,
		void *context, Instance &instance,
		std::uint64_t code_changes) noexcept;
	[[nodiscard]] Verdict verify_pages(
		::uml::memory::StaticGuestMemory &memory,
		Instance &instance) noexcept;
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
	/* The last code-change sequence find() was given (a diagnostics build
	 * counts how often it moves). */
	std::uint64_t last_changes_{};
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
