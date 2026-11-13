/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace uml::emulator {

/*
 * The loop guard of an application that boots a guest again after the guest
 * restarted itself.
 *
 * A restart is early when it comes less than `early_restart_window` after its
 * boot started, or when the boot had not finished: for a guest whose console
 * says when its boot ends (an Android guest, uml_emulator_run_end), a restart
 * before that point counts however long the boot ran, so a guest that
 * restarts itself every 20 s and never gets as far as booting is stopped like
 * one that restarts at once. A restart after a finished boot that lasted the
 * window is not early, however often it follows: a reboot test suite restarts
 * its guest without limit. A guest that restarts early `early_restart_limit`
 * times in a row is not going to boot: the application ends the loop instead
 * of rebooting it for ever. A restart that is not early resets the count.
 * These constants are the one place the policy lives; every application that
 * runs the loop uses them.
 */
inline constexpr std::chrono::seconds early_restart_window{15};
inline constexpr unsigned early_restart_limit = 5;

/* The macOS launchers' exit status when the guard ended the loop. 0 is a
 * power-off and 1 a launch error; a guest never ends with 3. */
inline constexpr int early_restart_exit_status = 3;

/* The library refuses to hand a restarted guest back, with
 * -UML_EMULATOR_RUNTIME_ERROR and no side effect, while a kernel thread of
 * the old guest is still ending. The refusal can be retried: an application
 * runs again up to this many times, this far apart, before it ends the launch
 * on the refusal. */
inline constexpr unsigned restart_run_retries = 3;
inline constexpr std::chrono::milliseconds restart_run_retry_delay{500};

/* The restart command is the guest's raw bytes, control characters and
 * quotes included. This is what an application may log or show: printable
 * ASCII as it is, a backslash or a single quote with a backslash before it,
 * and any other byte as \xNN, so the log line it sits in cannot be broken or
 * made to look like another. */
[[nodiscard]] inline std::string printable_restart_command(
	std::string_view command)
{
	constexpr char digits[] = "0123456789abcdef";
	std::string printable;
	printable.reserve(command.size());
	for (const char character : command) {
		const auto byte = static_cast<unsigned char>(character);
		if (byte == '\\' || byte == '\'') {
			printable.push_back('\\');
			printable.push_back(character);
		} else if (byte >= 0x20 && byte < 0x7f) {
			printable.push_back(character);
		} else {
			printable += "\\x";
			printable.push_back(digits[byte >> 4]);
			printable.push_back(digits[byte & 0xf]);
		}
	}
	return printable;
}

/* What the application knows of the boot that just ended. */
enum class BootCompletion {
	/* The guest's console says nothing of when its boot ends (a Linux
	 * desktop guest): only the early window counts. */
	unobserved,
	/* The console says so, and had not by the restart. */
	unfinished,
	/* The console said that the boot finished. */
	completed,
};

/* What an application tells the guard of a boot that ended: `observed` is
 * whether the guest's kind reports its boot's end at all, `completed` whether
 * it did (uml_emulator_run_end.boot_completed). */
[[nodiscard]] constexpr BootCompletion boot_completion(bool observed,
	bool completed) noexcept
{
	return !observed ? BootCompletion::unobserved :
		completed ? BootCompletion::completed : BootCompletion::unfinished;
}

/* What an application says when the guard ends the loop, after "the guest ":
 * the restarts in a row, and why they count. */
[[nodiscard]] inline std::string restart_loop_end_message(unsigned restarts,
	BootCompletion completion)
{
	std::string message = "restarted " + std::to_string(restarts) +
		" times in a row";
	message += completion == BootCompletion::unobserved ?
		" within " : ", each before it finished booting or within ";
	message += std::to_string(early_restart_window.count()) + " s of booting";
	return message;
}

class RestartLoopGuard final {
public:
	using Clock = std::chrono::steady_clock;

	/* A boot starts at `now`. */
	void boot_started(Clock::time_point now) noexcept { started_ = now; }

	/* The guest asked to restart at `now`, and its boot ended as
	 * `completion` says. Whether the application may boot it again: false
	 * once `early_restart_limit` early restarts followed each other. */
	[[nodiscard]] bool may_restart(Clock::time_point now,
		BootCompletion completion) noexcept
	{
		if (now - started_ < early_restart_window ||
		    completion == BootCompletion::unfinished)
			++early_in_a_row_;
		else
			early_in_a_row_ = 0;
		return early_in_a_row_ < early_restart_limit;
	}

	/* Restarts in a row that came early. */
	[[nodiscard]] unsigned early_restarts() const noexcept
	{
		return early_in_a_row_;
	}

private:
	Clock::time_point started_{};
	unsigned early_in_a_row_{};
};

} // namespace uml::emulator
