// SPDX-License-Identifier: GPL-2.0-or-later
#include "code_holders.hpp"

#ifndef UML_APP_STORE_RUNTIME

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <string>
#include <thread>

namespace uml::memory::code_holders {
namespace {

/* DARWIN_UML_CODE_HOLDER_COUNTERS=<path> writes the counters to <path> once a
 * second for as long as the process runs: a diagnostic for what the holder
 * words cost a guest. The emulator leaves through _Exit(), so the file is up
 * to a second old at the end. App Store builds have neither the variable nor
 * the thread. */
void write_counters(const std::string &path)
{
	const auto now = counters();
	std::FILE *out = std::fopen(path.c_str(), "w");
	if (out == nullptr)
		return;
	std::fprintf(out,
		"registrations=%llu overflows=%llu exchanges=%llu "
		"publications=%llu all_publications=%llu "
		"generations_published=%llu leaves=%llu leaf_bytes=%llu "
		"unknown_backings=%llu\n",
		static_cast<unsigned long long>(now.registrations),
		static_cast<unsigned long long>(now.overflows),
		static_cast<unsigned long long>(now.exchanges),
		static_cast<unsigned long long>(now.publications),
		static_cast<unsigned long long>(now.all_publications),
		static_cast<unsigned long long>(now.generations_published),
		static_cast<unsigned long long>(now.leaves),
		static_cast<unsigned long long>(now.leaves * sizeof(Leaf)),
		static_cast<unsigned long long>(now.unknown_backings));
	std::fclose(out);
}

bool start_report() noexcept
{
	const char *path = std::getenv("DARWIN_UML_CODE_HOLDER_COUNTERS");
	if (path == nullptr || *path == '\0')
		return false;
	try {
		/* Host signals are the guest's interrupts and must never be
		 * delivered to this thread; it inherits the blocked mask. */
		sigset_t all;
		sigset_t previous;
		sigfillset(&all);
		pthread_sigmask(SIG_BLOCK, &all, &previous);
		std::thread{[file = std::string{path}] {
			for (;;) {
				write_counters(file);
				std::this_thread::sleep_for(std::chrono::seconds{1});
			}
		}}.detach();
		pthread_sigmask(SIG_SETMASK, &previous, nullptr);
		return true;
	} catch (...) {
		return false;
	}
}

} // namespace

void report_counters_once() noexcept
{
	static const bool started = start_report();
	(void)started;
}

} // namespace uml::memory::code_holders

#else

namespace uml::memory::code_holders {

void report_counters_once() noexcept {}

} // namespace uml::memory::code_holders

#endif
