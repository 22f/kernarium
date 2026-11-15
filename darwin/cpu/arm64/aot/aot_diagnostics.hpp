// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>

#if !UML_AOT_DIAGNOSTICS
#error "aot_diagnostics.hpp is for builds that set UML_AOT_DIAGNOSTICS"
#endif

/* The counters, the test switches and the report of a diagnostics build of
 * the AOT runtime. Production builds do not set UML_AOT_DIAGNOSTICS and carry
 * none of this. The counters are fixed atomics bumped by the vCPU thread; the
 * report is written by a thread of its own to DARWIN_UML_AOT_REPORT
 * (a few times a second), so no vCPU thread does file I/O or allocates. */
namespace uml::cpu::arm64::aot::diagnostics {

struct Stats final {
	std::uint64_t binds{};
	/* Full verifications (every page) and pages hashed in all. */
	std::uint64_t verifications{};
	std::uint64_t pages_hashed{};
	/* Verifications that found a page unreadable. */
	std::uint64_t unreadable{};
	/* Instances whose pages differ from the ones the code stands for. */
	std::uint64_t poisoned{};
	std::uint64_t enters{};
	std::uint64_t steps{};
	std::uint64_t retired{};
	std::uint64_t refills{};
	std::uint64_t refill_failures{};
	std::uint64_t cross_hits{};
	std::uint64_t cross_misses{};
	std::array<std::uint64_t, 4> refusals{};
};

/* Override DARWIN_UML_AOT (nullopt: read the environment again). */
void set_enabled(std::optional<bool> state) noexcept;
[[nodiscard]] Stats stats() noexcept;
void reset_stats() noexcept;

} // namespace uml::cpu::arm64::aot::diagnostics
