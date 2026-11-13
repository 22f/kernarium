// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The console's word that an Android guest finished booting: Android's init
 * processing sys.boot_completed=1, or starting its recovery. The signal reads
 * the console's bytes in whatever chunks the kernel writes them, finds a
 * marker that a write splits, takes near-misses for nothing, and loses no
 * chunk to a race.
 */
#include "console_boot_signal.hpp"
#include "require.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using uml::runtime::ConsoleBootSignal;
using uml::testing::require;

constexpr std::string_view line =
	"[    9.2] init: processing action (sys.boot_completed=1) from "
	"(/system/etc/init/hw/init.rc:1)\n";
constexpr std::string_view recovery = "init: starting service 'recovery'...\n";

void feed(ConsoleBootSignal &signal, std::string_view text)
{
	signal.feed(text.data(), text.size());
}

void test_a_line_in_one_write_completes()
{
	ConsoleBootSignal signal;
	require(!signal.completed(), "a new signal was complete");
	feed(signal, "init: starting service 'zygote'...\n");
	require(!signal.completed(), "an unrelated line completed");
	feed(signal, line);
	require(signal.completed(), "the boot's line did not complete");
}

void test_recovery_completes()
{
	ConsoleBootSignal signal;
	feed(signal, recovery);
	require(signal.completed(), "recovery's start did not complete");
}

void test_a_marker_split_anywhere_is_found()
{
	for (const auto text : {line, recovery}) {
		for (std::size_t cut = 0; cut <= text.size(); ++cut) {
			ConsoleBootSignal signal;
			feed(signal, text.substr(0, cut));
			feed(signal, text.substr(cut));
			require(signal.completed(), "a split marker was missed");
		}
		ConsoleBootSignal bytes;
		for (const char byte : text)
			bytes.feed(&byte, 1);
		require(bytes.completed(), "a marker written a byte at a time was missed");
	}
}

void test_near_misses_complete_nothing()
{
	ConsoleBootSignal signal;
	feed(signal, "init: processing action (sys.boot_completed=0) from x\n");
	feed(signal, "init: processing action (sys.boot_completed\n");
	feed(signal, "processing action (sys.boot_complete=1)\n");
	feed(signal, "starting service 'recovery'\n");
	feed(signal, "init: starting service 'recoveryd'\n");
	feed(signal, "sys.boot_completed=1\n");
	require(!signal.completed(), "a near miss completed");
	/* A prefix repeated before the real marker does not hide it. */
	feed(signal, "processing action (processing action (sys.boot_completed=1)");
	require(signal.completed(), "a marker after its own prefix was missed");
}

void test_noise_around_a_marker()
{
	std::string noise(100000, 'p');
	for (std::size_t index = 0; index < noise.size(); index += 7)
		noise[index] = "pro cessing action (sys.b"[index % 25];
	ConsoleBootSignal before;
	feed(before, noise);
	require(!before.completed(), "noise completed");
	feed(before, line);
	require(before.completed(), "a marker after noise was missed");
	ConsoleBootSignal inside;
	feed(inside, noise + std::string{line} + noise);
	require(inside.completed(), "a marker inside noise was missed");
}

void test_a_completed_signal_latches_until_reset()
{
	ConsoleBootSignal signal;
	feed(signal, line);
	feed(signal, "anything else\n");
	require(signal.completed(), "later output undid a completed boot");
	signal.reset();
	require(!signal.completed(), "a reset did not forget the boot");
	feed(signal, "init: processing action (sys.boot_comp");
	signal.reset();
	feed(signal, "leted=1)");
	require(!signal.completed(), "a reset kept half a marker");
	feed(signal, recovery);
	require(signal.completed(), "the boot after a reset did not complete");
}

void test_empty_and_null_input()
{
	ConsoleBootSignal signal;
	signal.feed(nullptr, 5);
	signal.feed(line.data(), 0);
	require(!signal.completed(), "nothing completed the boot");
}

/* Threads that feed at once lose no chunk: a chunk that holds a whole marker
 * completes the boot whatever the others write meanwhile. */
void test_concurrent_writers_lose_no_chunk()
{
	for (int round = 0; round != 50; ++round) {
		ConsoleBootSignal signal;
		std::atomic_bool go{};
		std::vector<std::thread> writers;
		for (int writer = 0; writer != 6; ++writer)
			writers.emplace_back([&, writer] {
				while (!go.load())
					std::this_thread::yield();
				const std::string noise(257 + writer, 'x');
				for (int index = 0; index != 400; ++index)
					feed(signal, noise);
				if (writer == 3)
					feed(signal, line);
				for (int index = 0; index != 400; ++index)
					feed(signal, noise);
			});
		go.store(true);
		for (auto &writer : writers)
			writer.join();
		require(signal.completed(), "a chunk was lost to a race");
	}
}

} // namespace

int main()
{
	test_a_line_in_one_write_completes();
	test_recovery_completes();
	test_a_marker_split_anywhere_is_found();
	test_near_misses_complete_nothing();
	test_noise_around_a_marker();
	test_a_completed_signal_latches_until_reset();
	test_empty_and_null_input();
	test_concurrent_writers_lose_no_chunk();
	std::puts("console boot signal tests passed");
	return EXIT_SUCCESS;
}
