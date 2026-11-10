// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace uml::runtime {

/*
 * The calls an embedded kernel makes that can block its host thread, marked.
 *
 * The loader resolves each of the kernel's blocking imports to a wrapper that
 * says what the thread waits for (platform/host_wait.hpp) for the length of
 * the call and then makes the same call. A call that cannot block, a poll or
 * select with a zero timeout, is made unmarked. The stall watch reads the
 * marks to tell a vCPU that waits for the host from one the system did not
 * run (runtime/stall_watch.hpp):
 *
 *   pselect, pause, pthread_cond_wait, pthread_cond_timedwait   idle
 *   poll                                                        answer
 *   read, recvfrom                                              receive
 *   sendmsg, sendto                                             send
 *   pread, pwrite                                               disk read, write
 *   fsync                                                       flush
 *
 * recvmsg is marked by the resource ledger (kernel_resources.hpp), write
 * (send) by the session record (session_record.hpp) and the commit of guest
 * RAM by the memory layer.
 */

/* The wrapper for a kernel import, or nullptr when the call is not marked or
 * the system's own cannot be found. `name` is the symbol as the kernel names
 * it; a leading underscore, which the Darwin SDK's asm labels add, is
 * ignored. The wrapper calls the system's function of that name. */
[[nodiscard]] void *kernel_wait_import(const char *name) noexcept;

} // namespace uml::runtime
