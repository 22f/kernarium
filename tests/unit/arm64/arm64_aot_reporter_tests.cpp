// SPDX-License-Identifier: GPL-2.0-or-later
/* The reporter of the AOT runtime's diagnostics build (aot_runtime.cpp): the
 * thread that writes the report, and a process that started it and then ends
 * through exit() with that thread still running. A guest run reads the report
 * a few times a second while the emulator runs, and the emulator itself ends
 * through _exit(), so only a row like this one sees what exit() does to it. */

#include "cpu/arm64/aot/aot_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <unistd.h>

namespace aot = uml::cpu::arm64::aot;
namespace fs = std::filesystem;

#define require(condition)                                                  \
	do {                                                                \
		if (!(condition)) {                                         \
			std::fprintf(stderr, "%s:%d: requirement failed: %s\n", \
				__FILE__, __LINE__, #condition);            \
			std::abort();                                       \
		}                                                           \
	} while (false)

namespace {

constexpr auto wait_limit = std::chrono::seconds(30);

std::string read_file(const fs::path &path)
{
	std::ifstream stream{path};
	std::ostringstream text;
	text << stream.rdbuf();
	return text.str();
}

/* The report once it has this text in it (the reporter writes every half
 * second); an empty string if it does not come. */
std::string wait_for_report(const fs::path &path, const std::string &needle)
{
	const auto deadline = std::chrono::steady_clock::now() + wait_limit;
	while (std::chrono::steady_clock::now() < deadline) {
		const auto text = read_file(path);
		if (text.find(needle) != std::string::npos)
			return text;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return {};
}

} // namespace

int main()
{
	char pattern[] = "/tmp/uml-aot-reporter-XXXXXX";
	const char *directory = mkdtemp(pattern);
	require(directory != nullptr);
	const fs::path report = fs::path{directory} / "report";
	require(setenv("DARWIN_UML_AOT", "1", 1) == 0);
	require(setenv("DARWIN_UML_AOT_REPORT", report.c_str(), 1) == 0);

	/* The first ask for the component starts the reporter. */
	require(aot::enabled());
	const auto text = wait_for_report(report, "count_audit runs");
	require(!text.empty());
	/* The report is made beside and renamed: no front of one is left. */
	require(!fs::exists(fs::path{report}.concat(".part")) ||
		!read_file(report).empty());

	std::error_code ignored;
	fs::remove_all(directory, ignored);
	std::printf("the reporter wrote its report and the process left through exit()\n");
	/* Returning from main is exit(): the atexit handlers and the static
	 * destructors run while the reporter's thread goes on. */
	return 0;
}
