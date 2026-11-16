// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The counters reporter starts a thread with every host signal blocked, so
 * that the guest's interrupts never reach it, and gives the caller's mask back
 * afterwards. The caller is a translating thread: a start that fails (the
 * path or the thread's own state cannot be allocated) must leave it with the
 * mask it had, or its timer and wake signals stay blocked for good.
 *
 * Each run is one process, as the start happens once, and fails the Nth
 * allocation the start makes, N being the argument:
 *   1   the path's copy, which is made before the signals are blocked;
 *   2   the thread's state, with the signals blocked.
 * The signals the emulator's interrupts use must be unblocked afterwards.
 */
#include "allocation_failure.hpp"
#include "code_holders.hpp"
#include "require.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>

int main(int argc, char **argv)
{
	using uml::testing::require;
	require(argc == 2);
	const auto failing = std::strtoul(argv[1], nullptr, 10);
	require(failing >= 1 && failing <= 2);

	/* Longer than a short string keeps, so that copying it allocates, and
	 * naming nothing, so that a reporter that does start finds no file. */
	require(setenv("DARWIN_UML_CODE_HOLDER_COUNTERS",
		"/nonexistent-directory/code-holder-counters-reporter-test", 1) == 0);

	sigset_t unblocked;
	sigemptyset(&unblocked);
	require(pthread_sigmask(SIG_SETMASK, &unblocked, nullptr) == 0);

	uml::testing::fail_after_allocations(failing - 1);
	uml::memory::code_holders::report_counters_once();
	const bool consumed = uml::testing::last_allocation_failure().observed;
	(void)uml::testing::cancel_pending_allocation_failure();

	sigset_t after;
	require(pthread_sigmask(SIG_SETMASK, nullptr, &after) == 0);
	for (const int signal : {SIGALRM, SIGUSR1, SIGUSR2}) {
		if (sigismember(&after, signal) == 1) {
			std::fprintf(stderr,
				"failed reporter start left signal %d blocked\n", signal);
			return 1;
		}
	}
	require(consumed);
	return 0;
}
