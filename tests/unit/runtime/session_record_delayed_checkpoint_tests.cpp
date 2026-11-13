// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The session record's state against a checkpoint that is delayed between its
 * last look at the session and its write. The test binary defines pwrite, which
 * the record's object resolves to, and holds the state write of the thread that
 * asks for it; every other write goes to the real one. An end is the last state
 * on disk once the checkpoint's repair has run, whether it waited for the
 * checkpoint (an ordinary end waits a bounded time) or did not (a crash). The
 * race is deterministic here: the checkpoint is stuck in its write while the
 * end runs. The instant between that write and its repair is accepted and not
 * tested.
 */
#include "scratch_directory.hpp"
#include "session_record.hpp"
#include "require.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <exception>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using uml::runtime::SessionEnd;
using uml::testing::require;
using namespace std::chrono_literals;

/* The thread that asks for its next state write to be held, and for how long. */
thread_local bool hold_state_write = false;
std::atomic<int> hold_ms{0};
std::atomic<bool> state_write_held{false};
std::atomic<bool> state_write_landed{false};

std::string read_file(const std::string &path)
{
	std::string text;
	if (std::FILE *file = std::fopen(path.c_str(), "r"); file != nullptr) {
		char buffer[4096];
		std::size_t count;
		while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
			text.append(buffer, count);
		std::fclose(file);
	}
	return text;
}

bool has(const std::string &text, const char *part)
{
	return text.find(part) != std::string::npos;
}

/* The record's own descriptors, on a file that takes everything written. */
class Console final {
public:
	Console() : descriptor_{open("/dev/null", O_WRONLY)}
	{
		require(descriptor_ >= 0, "no console");
	}
	Console(const Console &) = delete;
	Console &operator=(const Console &) = delete;
	~Console() { close(descriptor_); }

	std::vector<std::string> arguments() const
	{
		return {"ssl0=fd:" + std::to_string(descriptor_)};
	}

	void write(const std::string &text) const
	{
		require(uml::runtime::kernel_write(descriptor_, text.data(),
			text.size()) == static_cast<ssize_t>(text.size()));
	}

private:
	int descriptor_;
};

/* Begins a session of a guest whose boot is unfinished, and starts a
 * checkpoint on a thread whose state write is held for `ms`. Returns once the
 * thread is inside the held write. */
std::thread checkpoint_held_for(int ms, const Console &console)
{
	state_write_held.store(false);
	state_write_landed.store(false);
	hold_ms.store(ms);
	std::thread checkpoint{[&console] {
		hold_state_write = true;
		/* The record's own checkpoint thread may hold the writer's flag
		 * now: ask again, with more output, until this thread's write
		 * is the held one. */
		for (int attempt = 0; attempt < 200 &&
			     !state_write_held.load(); ++attempt) {
			console.write("init: line " + std::to_string(attempt) + "\n");
			uml::runtime::checkpoint_session_memory();
		}
	}};
	for (int wait = 0; wait < 500 && !state_write_held.load(); ++wait)
		std::this_thread::sleep_for(10ms);
	require(state_write_held.load(), "the checkpoint did not reach its write");
	return checkpoint;
}

/* An ordinary end waits a bounded time for the checkpoint, which is held longer,
 * then writes: the checkpoint's late write is followed by the end's record. */
void an_end_outlasts_a_delayed_checkpoint(bool crash)
{
	const Console console;
	const uml::testing::ScratchSubdirectory directory{
		"uml-session-delayed.XXXXXX"};
	const std::string path = (directory.path() / "session").string();
	uml::runtime::reset_session_record();
	uml::runtime::set_session_boot_facts(true, 0);
	require(uml::runtime::begin_session_record(path, console.arguments()));
	console.write("init: init first stage started!\n");
	std::thread checkpoint = checkpoint_held_for(300, console);
	if (crash)
		uml::runtime::record_kernel_crash();
	else
		uml::runtime::end_session_record(SessionEnd::stopped, 0);
	const std::string at_end = read_file(path);
	const bool end_came_first = !state_write_landed.load();
	checkpoint.join();
	const std::string after = read_file(path);
	const char *name = crash ? "state ended\nend kernel-abort\n" :
		"state ended\nend stopped\n";
	require(has(after, name) && !has(after, "state running\n"),
		std::string{"the delayed checkpoint overwrote the end: "} + after);
	require(state_write_landed.load(), "the held write never landed");
	/* The wait is bounded: the end did not wait out the delay. */
	require(end_came_first && has(at_end, name),
		"the end waited for a checkpoint that was held: " + at_end);
	require(after.size() == at_end.size(),
		"the record changed size after the end");
	uml::runtime::reset_session_record();
}

/* A session begun after an end is not repaired with the end before it. */
void the_next_session_begins_clean()
{
	const Console console;
	const uml::testing::ScratchSubdirectory directory{
		"uml-session-delayed.XXXXXX"};
	const std::string path = (directory.path() / "session").string();
	uml::runtime::reset_session_record();
	uml::runtime::set_session_boot_facts(true, 0);
	require(uml::runtime::begin_session_record(path, console.arguments()));
	uml::runtime::end_session_record(SessionEnd::restarted, 0);
	uml::runtime::set_session_boot_facts(true, 1);
	require(uml::runtime::begin_session_record(path, console.arguments()));
	console.write("init: init first stage started!\n");
	uml::runtime::checkpoint_session_memory();
	const std::string state = read_file(path);
	require(has(state, "state running\n") && !has(state, "state ended\n"),
		"the next session's checkpoint wrote the end before it: " + state);
	uml::runtime::end_session_record(SessionEnd::stopped, 0);
	require(has(read_file(path), "state ended\nend stopped\n"));
	uml::runtime::reset_session_record();
}

} // namespace

/* The record's object resolves pwrite here. */
extern "C" ssize_t pwrite(int descriptor, const void *bytes, size_t size,
	off_t offset)
{
	using Pwrite = ssize_t (*)(int, const void *, size_t, off_t);
	static const Pwrite real =
		reinterpret_cast<Pwrite>(dlsym(RTLD_NEXT, "pwrite"));
	if (!hold_state_write ||
	    offset != static_cast<off_t>(uml::runtime::session_state_offset))
		return real(descriptor, bytes, size, offset);
	hold_state_write = false;
	state_write_held.store(true);
	std::this_thread::sleep_for(std::chrono::milliseconds{hold_ms.load()});
	const ssize_t written = real(descriptor, bytes, size, offset);
	state_write_landed.store(true);
	return written;
}

int main()
{
	try {
		an_end_outlasts_a_delayed_checkpoint(false);
		an_end_outlasts_a_delayed_checkpoint(true);
		the_next_session_begins_clean();
	} catch (const std::exception &error) {
		std::fprintf(stderr, "delayed checkpoint test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
	std::puts("session record delayed checkpoint tests passed");
	return EXIT_SUCCESS;
}
