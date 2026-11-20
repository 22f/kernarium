// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>

namespace uml::runtime {

namespace detail {

[[nodiscard]] inline std::atomic_bool &start_refusal_flag() noexcept
{
	static std::atomic_bool refused{};
	return refused;
}

} // namespace detail

/* Whether the host refused the run that last began in this process for want of
 * memory, before its kernel started: the heap region, the window or the guest's
 * RAM could not be had. `-UML_EMULATOR_OUT_OF_MEMORY` alone does not say it: a
 * failed allocation of the library's own (the argument vector, the handle)
 * returns the same result with no window reserved, and a guest's own exit
 * status can be the same number. The run clears the mark when it begins, so a
 * caller that reads it after a run read what that run did. Any thread. */
inline void clear_start_refusal() noexcept
{
	detail::start_refusal_flag().store(false, std::memory_order_release);
}

inline void note_start_refused_for_memory() noexcept
{
	detail::start_refusal_flag().store(true, std::memory_order_release);
}

[[nodiscard]] inline bool start_refused_for_memory() noexcept
{
	return detail::start_refusal_flag().load(std::memory_order_acquire);
}

} // namespace uml::runtime
