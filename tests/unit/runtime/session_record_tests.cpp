// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The session record (runtime/session_record): the console descriptors the
 * kernel arguments name, the kernel output it keeps, the panic message it
 * finds, and the record file through a session's begin, its first end, a
 * kernel crash and an exit() of the process, which runs in a child.
 */
#include "session_record.hpp"
#include "require.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using uml::runtime::SessionEnd;
using uml::testing::require;

constexpr std::size_t slot_bytes = uml::runtime::session_memory_slot_bytes;
constexpr std::size_t state_offset = uml::runtime::session_state_offset;

std::string read_file(const std::string &path)
{
	std::string text;
	if (FILE *file = std::fopen(path.c_str(), "r"); file != nullptr) {
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

std::string temporary_record()
{
	char path[] = "/tmp/uml-session-record-XXXXXX";
	const int descriptor = mkstemp(path);
	require(descriptor >= 0, "no temporary record");
	close(descriptor);
	return path;
}

std::string tail()
{
	std::vector<char> buffer(8192);
	return {buffer.data(), uml::runtime::kernel_output_tail(buffer)};
}

void test_names()
{
	require(uml::runtime::session_end_name(SessionEnd::running) == "running");
	require(uml::runtime::session_end_name(SessionEnd::guest_panic) ==
		"guest-panic");
	require(uml::runtime::session_end_name(
		SessionEnd::hibernation_restore_failed) ==
		"hibernation-restore-failed");
	require(uml::runtime::session_end_name(SessionEnd::process_exit) ==
		"process-exit");
	require(uml::runtime::session_end_name(SessionEnd::host_fault) ==
		"host-fault");
}

void test_console_descriptors()
{
	const std::vector<std::string> arguments{"console=ttyS0",
		"ssl0=fd:7,fd:7", "con=fd:0,fd:1", "con1=fd:3", "ssl=pts",
		"conx=fd:4", "ssl1=fd:-1", "ssl2=fd:9x", "mem=512M"};
	int found[4] = {-1, -1, -1, -1};
	require(uml::runtime::console_output_descriptors(arguments, found) == 3,
		"the console descriptors were miscounted");
	require(found[0] == 7 && found[1] == 1 && found[2] == 3,
		"a console's output descriptor was misread");
	int one[1] = {-1};
	require(uml::runtime::console_output_descriptors(arguments, one) == 1 &&
		one[0] == 7, "a short list was overrun");
}

void test_panic_message()
{
	using uml::runtime::panic_message;
	require(panic_message("no panic here\n").empty());
	require(panic_message("x\nDarwin UML panic: first\nDarwin UML panic: "
		"Attempted to kill init! exitcode=0x0000000b\nmore\n") ==
		"Attempted to kill init! exitcode=0x0000000b",
		"the last panic line was not the message");
	require(panic_message("Darwin UML panic: unterminated") ==
		"unterminated");
}

void test_kept_output()
{
	uml::runtime::reset_session_record();
	int console[2];
	require(pipe(console) == 0);
	int other[2];
	require(pipe(other) == 0);
	const std::vector<std::string> arguments{"ssl0=fd:" +
		std::to_string(console[1])};
	require(uml::runtime::begin_session_record({}, arguments),
		"a session without a record did not begin");
	require(uml::runtime::kernel_write(console[1], "console line\n", 13) ==
		13, "the console write did not reach its descriptor");
	require(uml::runtime::kernel_write(other[1], "elsewhere\n", 10) == 10);
	std::string kept = tail();
	require(kept == "console line\n", "the kept output is wrong: " + kept);
	/* More than the record keeps: the oldest bytes go. */
	std::string long_output(5000, 'a');
	long_output += "the end\n";
	for (std::size_t written = 0; written < long_output.size();) {
		const auto count = uml::runtime::kernel_write(console[1],
			long_output.data() + written,
			std::min<std::size_t>(512, long_output.size() - written));
		require(count > 0);
		written += static_cast<std::size_t>(count);
		char drain[512];
		(void)read(console[0], drain, sizeof(drain));
	}
	kept = tail();
	require(kept.size() == 4096 && kept.ends_with("aaaathe end\n"),
		"the kept output did not keep the newest bytes");
	for (const int descriptor : {console[0], console[1], other[0], other[1]})
		close(descriptor);
	uml::runtime::reset_session_record();
}

void test_record_lifecycle()
{
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	std::string record = read_file(path);
	require(record.starts_with("uml-session 5\nmemory at ") &&
		record.size() > state_offset && record[slot_bytes - 1] == '\n' &&
		record.substr(state_offset).starts_with(
			"state running\nend running\n"),
		"a begun session is not running: " + record);
	uml::runtime::end_session_record(SessionEnd::powered_off, 0);
	record = read_file(path);
	require(has(record, "state ended\nend powered-off\nstatus 0\n") &&
		has(record, "\nended ") && !has(record, "kernel-output"),
		"a power-off was recorded wrong: " + record);
	uml::runtime::end_session_record(SessionEnd::kernel_exit, 143);
	require(read_file(path).substr(state_offset) == record.substr(state_offset),
		"a second end replaced the first");

	/* A restart begins the next session in the same file. */
	require(uml::runtime::begin_session_record(path, {}));
	require(has(read_file(path), "state running\n"));
	const char line[] = "Darwin: the kernel says goodbye\n";
	(void)uml::runtime::kernel_write(STDERR_FILENO, line, sizeof(line) - 1);
	uml::runtime::end_session_record(SessionEnd::kernel_exit, 143,
		"exit\nwith\x01" "control");
	record = read_file(path);
	require(has(record, "end kernel-exit\nstatus 143\n") &&
		has(record, "message exit?with?control\n") &&
		has(record, "kernel-output\n") &&
		has(record, "the kernel says goodbye\n"),
		"a kernel exit was recorded wrong: " + record);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

void test_memory_checkpoints()
{
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	const std::string begun = read_file(path);
	const std::string slot = begun.substr(0, slot_bytes);
	require(has(slot, " footprint ") && has(slot, " peak "),
		"the memory slot has no footprint: " + slot);
	/* A checkpoint rewrites the slot and nothing after it. */
	constexpr int checkpoints = 500;
	const auto start = std::chrono::steady_clock::now();
	for (int index = 0; index < checkpoints; ++index)
		uml::runtime::checkpoint_session_memory();
	const auto each = std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now() - start).count() / checkpoints;
	std::printf("a memory checkpoint takes %.1f us\n", each);
	require(each < 5000.0, "a memory checkpoint is far too slow");
	const std::string checked = read_file(path);
	require(checked.size() == begun.size() &&
		checked.substr(state_offset) == begun.substr(state_offset),
		"a checkpoint changed the session's state");
	uml::runtime::end_session_record(SessionEnd::powered_off, 0);
	uml::runtime::checkpoint_session_memory();
	const std::string ended = read_file(path);
	require(ended.substr(state_offset).starts_with("state ended\nend powered-off\n"),
		"a checkpoint after the end replaced it: " + ended);
	/* The next session reuses the record's descriptor number. */
	const std::string next = temporary_record();
	require(uml::runtime::begin_session_record(next, {}));
	require(has(read_file(next), "\nstate running\n") &&
		has(read_file(path), "\nend powered-off\n"),
		"the next session's record was written to the wrong file");
	uml::runtime::reset_session_record();
	unlink(path.c_str());
	unlink(next.c_str());
}

std::atomic<unsigned> malloc_readings{0};

uml::runtime::MemoryTerms fixed_terms(bool read_malloc) noexcept
{
	constexpr std::uint64_t mib = std::uint64_t{1} << 20;
	if (read_malloc)
		malloc_readings.fetch_add(1, std::memory_order_relaxed);
	return {
		.guest_held = 4000 * mib,
		.gpu_budget = 1251 * mib,
		.gpu_held = 1240 * mib,
		.gpu_age_ms = 3500,
		.graphics = 700 * mib,
		.media = 5 * mib,
		.cache = 240 * mib,
		.heaps = 250 * mib,
		.malloc_allocated = read_malloc ? 900 * mib : 0,
		.malloc_in_use = read_malloc ? 800 * mib : 0,
		.internal = 5000 * mib,
		.compressed = 12 * mib,
	};
}

/* When a checkpoint reads malloc's term. */
namespace due {
using uml::runtime::memory_malloc_reading_due;
constexpr std::uint64_t mib = std::uint64_t{1} << 20;
static_assert(memory_malloc_reading_due(false, 0, 0),
	"the first checkpoint reads it");
static_assert(memory_malloc_reading_due(true, 1064 * mib, 1000 * mib) &&
		!memory_malloc_reading_due(true, 1064 * mib - 1, 1000 * mib) &&
		!memory_malloc_reading_due(true, 900 * mib, 1000 * mib),
	"then only a footprint 64 MiB above the highest reading's");
} // namespace due

std::size_t count_of(const std::string &text, const char *part)
{
	std::size_t count = 0;
	for (auto at = text.find(part); at != std::string::npos;
	     at = text.find(part, at + 1))
		++count;
	return count;
}

void test_memory_terms()
{
	uml::runtime::reset_session_record();
	uml::runtime::set_memory_terms_reader(&fixed_terms);
	malloc_readings.store(0);
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	const std::string begun = read_file(path);
	const std::string first = begun.substr(0, slot_bytes);
	require(has(first, "\nterms guest 4000 gpu 1240/1251 age 3 graphics 700 "
			"media 5 cache 240 heaps 250 malloc 900/800 age 0 other 0 "
			"internal 5000 compressed 12\n") &&
		has(first, "\npeak-terms at ") &&
		has(first, "\nhistory seconds footprint available guest gpu "
			"graphics cache heaps malloc other\n") &&
		count_of(first, "\nh ") == 1,
		"the first checkpoint has no terms: " + first);
	/* Enough checkpoints to fill both histories, which then stay at their
	 * sizes, oldest first. */
	constexpr unsigned checkpoints = uml::runtime::memory_history_recent +
		uml::runtime::memory_history_older_stride *
			(uml::runtime::memory_history_older + 2);
	for (unsigned index = 0; index < checkpoints; ++index)
		uml::runtime::checkpoint_session_memory();
	const std::string checked = read_file(path);
	const std::string slot = checked.substr(0, slot_bytes);
	require(count_of(slot, "\nh ") == uml::runtime::memory_history_recent +
			uml::runtime::memory_history_older &&
		slot[slot_bytes - 1] == '\n' &&
		slot.find_last_not_of(" \n") < slot_bytes - 2,
		"the history did not keep its sizes: " + slot);
	long previous = -1;
	for (auto at = slot.find("\nh "); at != std::string::npos;
	     at = slot.find("\nh ", at + 1)) {
		const long seconds = std::strtol(slot.c_str() + at + 3, nullptr, 10);
		require(seconds >= previous, "the history is out of order: " + slot);
		previous = seconds;
	}
	require(checked.substr(state_offset) == begun.substr(state_offset),
		"a checkpoint changed the session's state");
	/* Checkpoints at one footprint read malloc's term once, and repeat that
	 * reading. */
	require(malloc_readings.load() == 1 &&
			has(slot, "\nterms guest 4000 gpu 1240/1251 age 3 graphics "
				"700 media 5 cache 240 heaps 250 malloc 900/800 age "),
		"malloc's term was read again or not repeated: " + slot);

	/* What the product reads, and what that costs. */
	uml::runtime::set_memory_terms_reader(&uml::runtime::memory_terms);
	constexpr int timed = 200;
	const auto start = std::chrono::steady_clock::now();
	for (int index = 0; index < timed; ++index)
		uml::runtime::checkpoint_session_memory();
	const auto each = std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now() - start).count() / timed;
	const auto walk_start = std::chrono::steady_clock::now();
	for (int index = 0; index < timed; ++index)
		(void)uml::runtime::memory_terms(true);
	const auto walk = std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now() - walk_start).count() / timed;
	std::printf("a memory checkpoint with its terms takes %.1f us; reading "
		"malloc's term as well takes %.1f us\n", each, walk);
	require(each < 5000.0, "a memory checkpoint is far too slow");
	require(has(read_file(path).substr(0, slot_bytes), " malloc "),
		"the product's reader wrote no terms");
	uml::runtime::set_memory_terms_reader(nullptr);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

void *crash_on_a_thread(void *)
{
	uml::runtime::record_kernel_crash();
	static char name[64];
	pthread_getname_np(pthread_self(), name, sizeof(name));
	return name;
}

void test_kernel_crash()
{
	for (const bool panicked : {true, false}) {
		uml::runtime::reset_session_record();
		const std::string path = temporary_record();
		require(uml::runtime::begin_session_record(path, {}));
		const char panic[] = "[ 1.0] Kernel panic - not syncing: sysrq "
			"triggered crash\nDarwin UML panic: sysrq triggered crash\n";
		const char other[] = "[ 1.0] an ordinary line\n";
		if (panicked)
			(void)uml::runtime::kernel_write(STDERR_FILENO, panic,
				sizeof(panic) - 1);
		else
			(void)uml::runtime::kernel_write(STDERR_FILENO, other,
				sizeof(other) - 1);
		pthread_t thread;
		require(pthread_create(&thread, nullptr, crash_on_a_thread,
			nullptr) == 0);
		void *result = nullptr;
		require(pthread_join(thread, &result) == 0);
		const std::string name{static_cast<const char *>(result)};
		const std::string record = read_file(path);
		if (panicked) {
			require(name == "panic: sysrq triggered crash",
				"the panicking thread was named " + name);
			require(has(record, "end guest-panic\nstatus 6\n") &&
				has(record, "message sysrq triggered crash\n") &&
				has(record, "Kernel panic - not syncing"),
				"a panic was recorded wrong: " + record);
		} else {
			require(name.empty(), "a crash without a panic named its "
				"thread " + name);
			require(has(record, "end kernel-abort\n") &&
				!has(record, "message "),
				"a kernel abort was recorded wrong: " + record);
		}
		uml::runtime::reset_session_record();
		unlink(path.c_str());
	}
}

void test_host_fault()
{
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	const char registers[] = "Darwin host fault: signal=11 esr=0x92000046 "
		"far=0 pc=0x1a2b3c sp=0x16f000 lr=0x1a2b00\n";
	const char line[] = "Darwin host fault: taken on a host thread which "
		"owns no virtual CPU; the guest is not the faulting agent\n";
	(void)uml::runtime::kernel_write(STDERR_FILENO, registers,
		sizeof(registers) - 1);
	require(has(read_file(path), "state running\n"),
		"the registers alone ended the session");
	(void)uml::runtime::kernel_write(STDERR_FILENO, line, sizeof(line) - 1);
	const std::string record = read_file(path);
	require(has(record, "end host-fault\nstatus 11\n") &&
		has(record, "message signal=11 esr=0x92000046 far=0 pc=0x1a2b3c") &&
		has(record, "owns no virtual CPU"),
		"a host fault was recorded wrong: " + record);
	uml::runtime::record_kernel_crash();
	require(read_file(path).substr(state_offset) == record.substr(state_offset),
		"a later crash replaced the fault");
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

void test_process_exit_in_a_child()
{
	const std::string path = temporary_record();
	const pid_t child = fork();
	require(child >= 0);
	if (child == 0) {
		uml::runtime::reset_session_record();
		if (!uml::runtime::begin_session_record(path, {}))
			_exit(2);
		std::exit(0);
	}
	int status = 0;
	require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		WEXITSTATUS(status) == 0, "the child did not exit");
	const std::string record = read_file(path);
	require(has(record, "end process-exit\n") &&
		has(record, "message exit() was called while the guest ran\n"),
		"an exit() while the guest ran was recorded wrong: " + record);
	unlink(path.c_str());
}

/* The file is a fixed layout of a size it never exceeds, whatever the session
 * writes into it, and its parts do not overwrite each other. */
void test_size_bound_and_stall_slot()
{
	using uml::runtime::session_record_max_bytes;
	using uml::runtime::session_stall_slot_bytes;
	static_assert(session_record_max_bytes == 13824,
		"the record's largest size is stated in the documentation");
	uml::runtime::reset_session_record();
	const std::string path = temporary_record();
	const int sink = open("/dev/null", O_WRONLY | O_CLOEXEC);
	require(sink >= 0, "no sink");
	const std::string console = "con0=fd:" + std::to_string(sink) + ",fd:" +
		std::to_string(sink);
	const std::vector<std::string> arguments{console};
	require(uml::runtime::begin_session_record(path, arguments));
	const std::string begun = read_file(path);
	require(begun.substr(slot_bytes, 14) == "stalls 1 base " &&
		begun[state_offset - 1] == '\n' &&
		begun.substr(slot_bytes).find("total 0 shown 0") != std::string::npos,
		"a session starts with an empty stall slot");
	require(begun.size() < state_offset + 200);

	/* A slot of the wrong size changes nothing; the right one lands in its
	 * place alone. */
	uml::runtime::write_session_stall_slot(std::string(100, 'x'));
	uml::runtime::write_session_stall_slot(
		std::string(session_stall_slot_bytes + 1, 'x'));
	require(read_file(path) == begun, "a wrong-sized slot was written");
	std::string slot(session_stall_slot_bytes, 'x');
	slot.back() = '\n';
	uml::runtime::write_session_stall_slot(slot);
	std::string changed = read_file(path);
	require(changed.size() == begun.size() &&
		changed.substr(slot_bytes, session_stall_slot_bytes) == slot &&
		changed.substr(0, slot_bytes) == begun.substr(0, slot_bytes) &&
		changed.substr(state_offset) == begun.substr(state_offset),
		"the stall slot touched another part");

	/* A long run of checkpoints, stall slots and kernel output, and an end
	 * with a long message: the file stays within its bound. */
	const std::string noise(60000, 'k');
	for (unsigned round = 0; round < 40; ++round) {
		(void)uml::runtime::kernel_write(sink, noise.data(), noise.size());
		uml::runtime::checkpoint_session_memory();
		uml::runtime::write_session_stall_slot(slot);
		require(read_file(path).size() == begun.size(),
			"the running record grew");
	}
	uml::runtime::end_session_record(SessionEnd::failed, 1,
		std::string(100000, 'm'));
	const std::string ended = read_file(path);
	require(ended.size() <= session_record_max_bytes &&
		ended.size() > state_offset + 1000,
		"an end of a long message left a record of " +
		std::to_string(ended.size()) + " bytes");
	/* Nothing writes the slot once the session has ended. */
	uml::runtime::write_session_stall_slot(std::string(
		session_stall_slot_bytes, 'y'));
	require(read_file(path) == ended, "a slot was written after the end");

	/* The next session in the file starts empty, and the file shrinks. */
	require(uml::runtime::begin_session_record(path, arguments));
	const std::string next = read_file(path);
	require(next.size() < ended.size() && next.size() < state_offset + 200 &&
		next.substr(slot_bytes, 14) == "stalls 1 base ",
		"a session inherited the last one");
	uml::runtime::reset_session_record();
	(void)close(sink);
	unlink(path.c_str());
}

/* The counts a test's reader reports: a provider that has counted since the
 * test began, and a switch for a guest with no network. */
uml::runtime::SessionNetworkCounts reader_counts;
bool reader_present = true;

bool test_network_reader(uml::runtime::SessionNetworkCounts &counts) noexcept
{
	if (!reader_present)
		return false;
	counts = reader_counts;
	return true;
}

/* The network slot sits between the stall slot and the state: it holds this
 * session's share of the counts and the notice, rewritten at each checkpoint
 * and at the end, and it touches no other part. */
/* The network slot's lines up to the blanks that pad it. */
std::string network_slot_text(const std::string &slot)
{
	return slot.substr(0, slot.find_last_of('\n', slot.size() - 2) + 1);
}

/* The slot's text for fixed figures is the golden file AndariumCore parses
 * too (tests/unit/andarium/fixtures/network-slot-golden.txt). */
void the_network_slot_matches_the_golden_file(const char *golden_path)
{
	using uml::runtime::NetworkNotice;
	uml::runtime::reset_session_record();
	uml::runtime::set_network_notice(NetworkNotice::none);
	reader_counts = {};
	reader_present = true;
	uml::runtime::set_network_counts_reader(&test_network_reader);
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	reader_counts = {.dns_answered = 5, .dns_policy_denied = 3,
			 .dns_timed_out = 2, .dns_failed = 1,
			 .dhcp_discovers = 1, .dhcp_requests = 2,
			 .dhcp_acks = 1, .tcp_attempts = 12,
			 .tcp_established = 4, .tcp_unreachable = 2,
			 .tcp_refused = 1};
	uml::runtime::set_network_notice(NetworkNotice::cellular_denied);
	uml::runtime::set_network_notice(NetworkNotice::none);
	uml::runtime::set_network_notice(NetworkNotice::wifi_denied);
	uml::runtime::checkpoint_session_memory();
	const std::string text = network_slot_text(read_file(path).substr(
		uml::runtime::session_network_offset,
		uml::runtime::session_network_slot_bytes));
	const std::string golden = read_file(golden_path);
	require(!golden.empty(), "the golden network slot opens");
	if (golden != text) {
		std::fprintf(stderr, "golden:\n%s\nslot:\n%s\n", golden.c_str(),
			text.c_str());
		require(false, "the network slot differs from the golden file");
	}
	uml::runtime::set_network_counts_reader(nullptr);
	uml::runtime::set_network_notice(NetworkNotice::none);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

void test_network_slot()
{
	using uml::runtime::NetworkNotice;
	using uml::runtime::session_network_offset;
	using uml::runtime::session_network_slot_bytes;
	static_assert(session_network_offset == 8192 &&
			session_network_slot_bytes == 512 &&
			state_offset == session_network_offset +
				session_network_slot_bytes,
		"the network slot sits after the stall slot");
	const auto slot_of = [](const std::string &record) {
		return record.substr(session_network_offset,
			session_network_slot_bytes);
	};
	uml::runtime::reset_session_record();
	uml::runtime::set_network_notice(NetworkNotice::none);
	/* The provider counted before this session began. */
	reader_counts = {.dns_answered = 10, .dns_failed = 2, .tcp_attempts = 7,
			 .tcp_refused = 1};
	reader_present = true;
	uml::runtime::set_network_counts_reader(&test_network_reader);
	const std::string path = temporary_record();
	require(uml::runtime::begin_session_record(path, {}));
	std::string record = read_file(path);
	std::string slot = slot_of(record);
	require(slot.starts_with("network 1\nnet-dns answered 0 policy-denied 0 "
				 "timed-out 0 failed 0\n"
				 "net-dhcp discovers 0 requests 0 acks 0\n"
				 "net-tcp attempts 0 established 0 unreachable 0 "
				 "refused 0\nnet-notice shown 0 now none\n") &&
		slot.back() == '\n' && slot.size() == session_network_slot_bytes,
		"a session does not start from the provider's reading: " + slot);

	/* A checkpoint writes the share since the begin. */
	reader_counts.dns_answered = 15;
	reader_counts.dns_policy_denied = 3;
	reader_counts.dhcp_discovers = 1;
	reader_counts.dhcp_requests = 1;
	reader_counts.dhcp_acks = 1;
	reader_counts.tcp_attempts = 12;
	reader_counts.tcp_established = 4;
	reader_counts.tcp_unreachable = 2;
	uml::runtime::set_network_notice(NetworkNotice::cellular_denied);
	uml::runtime::checkpoint_session_memory();
	record = read_file(path);
	slot = slot_of(record);
	require(has(slot, "net-dns answered 5 policy-denied 3 timed-out 0 "
			  "failed 0\n") &&
		has(slot, "net-dhcp discovers 1 requests 1 acks 1\n") &&
		has(slot, "net-tcp attempts 5 established 4 unreachable 2 "
			  "refused 0\n") &&
		has(slot, "net-notice shown 1 now cellular\n"),
		"a checkpoint wrote the wrong network slot: " + slot);
	require(record.substr(state_offset).starts_with(
			"state running\nend running\n") &&
		record.substr(slot_bytes, 14) == "stalls 1 base ",
		"the network slot touched another part");

	/* The notice counts the denials that began, the one in force kept. */
	uml::runtime::set_network_notice(NetworkNotice::cellular_denied);
	uml::runtime::set_network_notice(NetworkNotice::none);
	uml::runtime::set_network_notice(NetworkNotice::wifi_denied);
	/* The end writes the last counts, which no checkpoint follows. */
	reader_counts.dns_timed_out = 9;
	uml::runtime::end_session_record(SessionEnd::powered_off, 0);
	record = read_file(path);
	slot = slot_of(record);
	require(has(slot, "timed-out 9 failed 0\n") &&
		has(slot, "net-notice shown 2 now wifi\n") &&
		has(record.substr(state_offset), "state ended\n"),
		"the end did not write the last network slot: " + slot);

	/* A denial under way when the next session begins is one it saw, and a
	 * provider made since is counted whole. */
	reader_counts = {.dns_answered = 3};
	require(uml::runtime::begin_session_record(path, {}));
	slot = slot_of(read_file(path));
	require(has(slot, "net-dns answered 0 ") &&
		has(slot, "net-notice shown 1 now wifi\n"),
		"the next session began wrong: " + slot);
	reader_counts = {.dns_answered = 1};
	uml::runtime::checkpoint_session_memory();
	slot = slot_of(read_file(path));
	require(has(slot, "net-dns answered 1 "),
		"a provider made since was not counted whole: " + slot);

	/* A guest with no network has the notice and no counts. */
	reader_present = false;
	uml::runtime::checkpoint_session_memory();
	slot = slot_of(read_file(path));
	require(slot.starts_with("network 1\nnet-notice shown 1 now wifi\n") &&
		!has(slot, "net-dns"),
		"a guest with no network was given counts: " + slot);

	uml::runtime::set_network_counts_reader(nullptr);
	uml::runtime::set_network_notice(NetworkNotice::none);
	uml::runtime::reset_session_record();
	unlink(path.c_str());
}

void test_unopenable_record()
{
	uml::runtime::reset_session_record();
	require(!uml::runtime::begin_session_record(
		"/nonexistent-directory/session", {}),
		"a record that cannot be opened was reported open");
	uml::runtime::end_session_record(SessionEnd::powered_off, 0);
}

} // namespace

int main(int argc, char **argv)
{
	try {
		/* The child forks before this process has a thread of its own. */
		test_process_exit_in_a_child();
		test_names();
		test_console_descriptors();
		test_panic_message();
		test_kept_output();
		test_record_lifecycle();
		test_memory_checkpoints();
		test_network_slot();
		if (argc == 2)
			the_network_slot_matches_the_golden_file(argv[1]);
		test_memory_terms();
		test_kernel_crash();
		test_host_fault();
		test_size_bound_and_stall_slot();
		test_unopenable_record();
	} catch (const std::exception &error) {
		std::fprintf(stderr, "session record test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
	std::puts("session record tests passed");
	return EXIT_SUCCESS;
}
