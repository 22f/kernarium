// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "memory/static_guest_memory.hpp"
#include "memory/tlb.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

/* Ahead-of-time native code for the Android image's own arm64 libraries
 * (docs/architecture.md, "Ahead-of-time code"). The framework holds one
 * generated blob (tools/aot/rewrite.py): the entry routine, the rewritten
 * code, per-instruction entry tables and page digests. This header is the
 * whole boundary the gadget executor sees. */
namespace uml::cpu::arm64::aot {

struct Stats final {
	std::uint64_t binds{};
	std::uint64_t verifications{};
	/* Verifications that found a page unreadable (the guest has not mapped
	 * it yet): the instance is tried again later. */
	std::uint64_t unreadable{};
	/* Instances whose pages differ from the ones the code stands for: never
	 * entered again in that address space. */
	std::uint64_t poisoned{};
	std::uint64_t enters{};
	std::uint64_t steps{};
	std::uint64_t retired{};
};

/* Whether AOT code may be entered: DARWIN_UML_AOT=1 at start-up, or the last
 * set_enabled(). Off by default. */
[[nodiscard]] bool enabled() noexcept;
void set_enabled(std::optional<bool> state) noexcept;
[[nodiscard]] Stats stats() noexcept;
void reset_stats() noexcept;

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
};

/* The libraries an address space has mapped, identified by the digest of
 * their code pages and verified whole before any of their code runs. An
 * instance whose bytes differ, or whose pages cannot be read, is never entered
 * again in that address space generation. One binder belongs to one executor
 * thread. */
class Binder final {
public:
	/* The AOT entry for `pc`, or an empty Hit. */
	[[nodiscard]] Hit find(::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::uint64_t code_changes,
		std::uint64_t pc) noexcept;
	/* The entry for `pc` in an instance find() returned, or an empty Hit. */
	[[nodiscard]] Hit entry(std::uint32_t instance,
		std::uint64_t pc) const noexcept;

private:
	struct Instance final {
		unsigned long generation{};
		std::uint32_t library{};
		std::uint64_t bias{};
		std::uint64_t verified_changes{};
		/* Not tried again before this many lookups (an unreadable page
		 * stays unreadable for a while). */
		std::uint64_t retry_at{};
		std::uint64_t backoff{};
		bool poisoned{};
		bool used{};
	};
	struct PageSlot final {
		unsigned long generation{};
		std::uint64_t code_changes{};
		std::uint64_t page{1};
		std::int32_t instance{-1};
	};

	/* resolve() answers an instance index, none (not a library page, or a
	 * poisoned instance: remembered), or not_ready (do not remember). */
	static constexpr std::int32_t none = -1;
	static constexpr std::int32_t not_ready = -2;
	enum class Verdict { match, unreadable, mismatch };
	[[nodiscard]] std::int32_t resolve(
		::uml::memory::StaticGuestMemory &memory, unsigned long generation,
		std::uint64_t code_changes, std::uint64_t page) noexcept;
	[[nodiscard]] Verdict verify(::uml::memory::StaticGuestMemory &memory,
		const Instance &instance) noexcept;

	static constexpr std::size_t instance_capacity = 64;
	static constexpr std::size_t page_slots = 2048;
	std::array<Instance, instance_capacity> instances_{};
	std::array<PageSlot, page_slots> pages_{};
	std::size_t next_instance_{};
	std::uint64_t ticks_{};
};

/* Run rewritten code from `native` until it leaves. `budget` bounds the
 * instructions it may retire before it returns. The guest registers, vectors
 * and flags are committed in `cpu` on every return. */
[[nodiscard]] EnterResult enter(CpuState &cpu, ::uml::memory::Tlb &tlb,
	std::uint64_t bias, const void *native, std::uint64_t budget) noexcept;

} // namespace uml::cpu::arm64::aot
