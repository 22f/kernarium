// SPDX-License-Identifier: GPL-2.0-or-later
/* Whether the ahead-of-time component is on is decided once per process, so
 * each case runs in a process of its own: the parent starts this program
 * again with the case named and the environment the case states, and the
 * child answers with its exit status. The component is on by default; the
 * environment turns it off, and a launch's own word (--aot=) beats the
 * environment. */
#include "cpu/aot_selection.hpp"
#include "cpu/arm64/aot/aot_runtime.hpp"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern char **environ;

namespace {

namespace aot = uml::cpu::arm64::aot;
using uml::cpu::AotSelection;

#define require(condition) \
	do { \
		if (!(condition)) { \
			std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
				#condition); \
			return 1; \
		} \
	} while (false)

struct Case final {
	const char *name;
	const char *environment; // DARWIN_UML_AOT, or nullptr for unset
	int (*run)();
};

int default_is_on()
{
	require(aot::enabled());
	return 0;
}

int environment_turns_it_off()
{
	require(!aot::enabled());
	return 0;
}

int environment_one_is_on()
{
	require(aot::enabled());
	return 0;
}

int other_values_are_the_default()
{
	require(aot::enabled());
	return 0;
}

int automatic_follows_the_environment_on()
{
	require(aot::select(AotSelection::automatic));
	require(aot::enabled());
	return 0;
}

int automatic_follows_the_environment_off()
{
	require(aot::select(AotSelection::automatic));
	require(!aot::enabled());
	return 0;
}

int launch_off_beats_the_default()
{
	require(aot::select(AotSelection::off));
	require(!aot::enabled());
	return 0;
}

int launch_on_beats_the_environment()
{
	require(aot::select(AotSelection::on));
	require(aot::enabled());
	return 0;
}

int a_late_change_is_refused()
{
	require(aot::enabled());
	/* The same word is kept, a different one cannot be. */
	require(aot::select(AotSelection::on));
	require(aot::select(AotSelection::automatic));
	require(!aot::select(AotSelection::off));
	require(aot::enabled());
	return 0;
}

int a_late_off_is_refused_for_on()
{
	require(aot::select(AotSelection::off));
	require(!aot::enabled());
	require(!aot::select(AotSelection::on));
	require(aot::select(AotSelection::off));
	return 0;
}

const Case cases[]{
	{"default_is_on", nullptr, default_is_on},
	{"environment_turns_it_off", "0", environment_turns_it_off},
	{"environment_one_is_on", "1", environment_one_is_on},
	{"other_values_are_the_default", "yes", other_values_are_the_default},
	{"automatic_on", nullptr, automatic_follows_the_environment_on},
	{"automatic_off", "0", automatic_follows_the_environment_off},
	{"launch_off", nullptr, launch_off_beats_the_default},
	{"launch_on", "0", launch_on_beats_the_environment},
	{"late_change", nullptr, a_late_change_is_refused},
	{"late_on_after_off", "1", a_late_off_is_refused_for_on},
};

int run_child(const Case &test)
{
	return test.run();
}

int run_parent(const char *program)
{
	int failures = 0;
	for (const auto &test : cases) {
		std::vector<std::string> environment;
		for (char **entry = environ; *entry != nullptr; ++entry)
			if (std::strncmp(*entry, "DARWIN_UML_AOT", 14) != 0)
				environment.emplace_back(*entry);
		if (test.environment != nullptr)
			environment.push_back(
				std::string{"DARWIN_UML_AOT="} + test.environment);
		std::vector<char *> envp;
		for (auto &entry : environment)
			envp.push_back(entry.data());
		envp.push_back(nullptr);
		std::string program_copy{program};
		std::string case_name{test.name};
		char *argv[]{program_copy.data(), case_name.data(), nullptr};
		pid_t child{};
		if (posix_spawn(&child, program, nullptr, nullptr, argv,
			    envp.data()) != 0) {
			std::fprintf(stderr, "%s: cannot start the case\n", test.name);
			++failures;
			continue;
		}
		int status{};
		if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
		    WEXITSTATUS(status) != 0) {
			std::fprintf(stderr, "%s: failed\n", test.name);
			++failures;
		}
	}
	if (failures == 0)
		std::printf("the component is on by default, the environment "
			    "turns it off and a launch's word wins\n");
	return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc == 2) {
		for (const auto &test : cases)
			if (std::strcmp(argv[1], test.name) == 0)
				return run_child(test);
		return 2;
	}
	return run_parent(argv[0]);
}
