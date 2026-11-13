// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>

namespace uml::gpu::testing {

// Available only from the test-owned transport compiled with
// UML_VHOST_USER_WAIT_TESTING=1.

/* Replaces the clock the server counts the time of a backend wait and of a
 * frame by; null restores std::chrono::steady_clock. */
void setBackendWaitClock(
    std::chrono::steady_clock::time_point (*now)() noexcept) noexcept;

} // namespace uml::gpu::testing
