// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A memory pressure injector for tests of the host's response to the system's
 * memory pressure (runtime/memory_pressure_response.hpp). A real event needs a
 * privileged tool, so a test runner inserts this library into the launcher it
 * starts (DYLD_INSERT_LIBRARIES) and names a directory in
 * UML_MEMORY_PRESSURE_INJECTOR_DIRECTORY. The library substitutes a source the
 * runner can raise for every memory pressure source the process makes, so that
 * the process's own handlers run unchanged. The library does nothing in a
 * process without the directory, and nothing in the product loads it.
 *
 *   request    "<level> <label>"    level is normal, warn or critical: the
 *                                   level is raised on every memory pressure
 *                                   source of the process, and <label>.done is
 *                                   written once the sources have it
 *
 * The runner writes a request as the file `request.tmp` and renames it to
 * `request`; the library removes `request` when it has taken it.
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dispatch/dispatch.h>
#include <fstream>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr char thread_name[] = "uml-pressure-injector";
constexpr char directory_variable[] = "UML_MEMORY_PRESSURE_INJECTOR_DIRECTORY";

const std::string *directory = nullptr;
std::mutex sources_mutex;
std::vector<dispatch_source_t> *sources = nullptr;
std::atomic<bool> started{false};

unsigned long level_flags(const std::string &level)
{
	if (level == "normal")
		return DISPATCH_MEMORYPRESSURE_NORMAL;
	if (level == "warn")
		return DISPATCH_MEMORYPRESSURE_WARN;
	if (level == "critical")
		return DISPATCH_MEMORYPRESSURE_CRITICAL;
	return 0;
}

void serve()
{
	pthread_setname_np(thread_name);
	const std::string request = *directory + "/request";
	for (;;) {
		std::ifstream in{request};
		std::string level;
		std::string label;
		if (in && (in >> level >> label)) {
			in.close();
			(void)std::remove(request.c_str());
			const unsigned long flags = level_flags(level);
			std::size_t raised = 0;
			{
				const std::lock_guard lock{sources_mutex};
				for (const dispatch_source_t source : *sources) {
					dispatch_source_merge_data(source, flags);
					++raised;
				}
			}
			std::ofstream out{*directory + "/" + label + ".done"};
			out << (flags != 0 ? "ok " : "unknown ") << raised << "\n";
		}
		std::this_thread::sleep_for(std::chrono::milliseconds{20});
	}
}

void start_serving()
{
	if (started.exchange(true))
		return;
	std::thread{serve}.detach();
}

/* The substitute: a memory pressure source becomes one the runner raises. The
 * handler sees the flags it would from the system, since a source of this type
 * delivers the data merged into it. */
dispatch_source_t injector_dispatch_source_create(
	dispatch_source_type_t type, uintptr_t handle, uintptr_t mask,
	dispatch_queue_t queue)
{
	if (directory == nullptr || type != DISPATCH_SOURCE_TYPE_MEMORYPRESSURE)
		return dispatch_source_create(type, handle, mask, queue);
	const dispatch_source_t source =
		dispatch_source_create(DISPATCH_SOURCE_TYPE_DATA_OR, 0, 0, queue);
	if (source != nullptr) {
		/* Kept for the life of the process: a request that arrives after
		 * the source's owner has ended it merges into a cancelled source. */
		dispatch_retain(source);
		{
			const std::lock_guard lock{sources_mutex};
			sources->push_back(source);
		}
		start_serving();
	}
	return source;
}

struct Interpose final {
	const void *replacement;
	const void *replacee;
};

__attribute__((used, section("__DATA,__interpose")))
Interpose interposers[] = {
	{reinterpret_cast<const void *>(&injector_dispatch_source_create),
		reinterpret_cast<const void *>(&dispatch_source_create)},
};

__attribute__((constructor)) void initialize()
{
	const char *const value = std::getenv(directory_variable);
	if (value == nullptr || value[0] != '/')
		return;
	directory = new std::string{value};
	sources = new std::vector<dispatch_source_t>;
}

} // namespace
