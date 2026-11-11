// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "guest_statistics.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace uml::runtime {

/*
 * The record of how the process's guest session ended, for the next launch.
 *
 * A launch that names a record file (--session-record=) has the run write
 * "running" into it before the kernel starts, and every way the session can
 * end overwrites that with the end: the run's own return, a guest kernel
 * panic or other kernel crash (darwin_uml_abort), an exit the kernel drives
 * (darwin_uml_exit), a hibernated guest whose RAM cannot be restored, a resume
 * that finds a guest disk changed (failed), and any exit() of the process
 * while the guest runs. A record still "running" at the next launch therefore
 * says that something outside the process ended it, without letting it run
 * another instruction: a SIGKILL from the system, such as a memory-limit end,
 * or from the person.
 *
 * The kernel's console and diagnostic output goes through write(), which the
 * loader resolves to kernel_write(): the record keeps the last few KiB of it,
 * so a crash's record carries the panic message and the lines before it. A
 * fault on a host thread that owns no vCPU does not reach the kernel's fault
 * path: the kernel's handler reports it on stderr and lets the fault end the
 * process through the default disposition, which writes the crash report.
 * kernel_write records that end as host_fault when it sees the report.
 *
 * Every memory_checkpoint_interval_ms while a session runs, a thread of the
 * record's own writes the process's footprint, its peak and what its memory
 * limit leaves into a fixed slot at the start of the file, so a record that
 * is still "running" says how close to its limit the process was. With a
 * terms reader (set_memory_terms_reader) the slot also holds the footprint's
 * large terms (MemoryTerms) now and at the session's highest checkpoint, and
 * their history: every checkpoint of the last memory_history_recent, and one
 * in memory_history_older_stride before those, memory_history_older of them.
 * Reading malloc's term stalls every thread that allocates meanwhile, so a
 * checkpoint asks for it only when memory_malloc_reading_due says so and
 * otherwise repeats the last reading with its age. The other terms are read
 * every time, and so is the footprint less them ("other"), which follows
 * malloc and what no term names between readings.
 *
 * Everything that runs at an end only formats into static storage and makes
 * system calls: it takes no lock and allocates nothing, so a panic, a signal
 * handler or a process whose other threads are suspended can run it.
 *
 * The file is text of a fixed layout and of a size it never exceeds
 * (session_record_max_bytes), whatever the session does: the memory slot, the
 * stall slot, the network slot, then the state with the kernel output it
 * quotes. Each part is rewritten in place by its own writer, so none grows
 * with the session's length. The stall slot is the stall watch's
 * (runtime/stall_record.hpp). The network slot holds the counts of the guest's
 * user network (set_network_counts_reader) and whether the application showed
 * its notice that iOS denies it the network (set_network_notice): numbers and
 * words only, never an address, a port or a name. The first line names the
 * layout: "uml-session 5" has the network slot between the stall slot and the
 * state; "uml-session 4" has the stall slot between the memory slot and the
 * state; the earlier layouts (1 to 3) have the state where the stall slot is
 * now.
 */
inline constexpr unsigned memory_checkpoint_interval_ms = 2000;
/* The bytes of the memory slot at the start of the file, of the stall slot
 * after it, of the network slot after that, and where the session's state
 * begins. */
inline constexpr std::size_t session_memory_slot_bytes = 4096;
inline constexpr std::size_t session_stall_slot_bytes = 4096;
inline constexpr std::size_t session_network_slot_bytes = 512;
inline constexpr std::size_t session_network_offset =
	session_memory_slot_bytes + session_stall_slot_bytes;
inline constexpr std::size_t session_state_offset =
	session_network_offset + session_network_slot_bytes;
/* The kernel output the state quotes, and the most the state ever takes: its
 * lines, a message and that output. */
inline constexpr std::size_t session_kernel_output_bytes = 4096;
inline constexpr std::size_t session_state_max_bytes =
	session_kernel_output_bytes + 1024;
/* The most the file ever holds. */
inline constexpr std::size_t session_record_max_bytes =
	session_state_offset + session_state_max_bytes;
inline constexpr unsigned memory_history_recent = 24;
inline constexpr unsigned memory_history_older = 24;
inline constexpr unsigned memory_history_older_stride = 15;

/* A checkpoint reads malloc's term at a session's first checkpoint and when
 * the footprint passes the highest one a reading was taken at by
 * memory_malloc_peak_step_bytes: readings gather where the footprint climbs,
 * as at boot and while a game loads, a steady footprint takes none, and a
 * footprint that climbs to the limit has a reading from at most one step
 * below its top. */
inline constexpr std::uint64_t memory_malloc_peak_step_bytes =
	std::uint64_t{64} << 20;

/* The rule above; `read_peak` is the highest footprint a reading was taken
 * at. */
[[nodiscard]] constexpr bool memory_malloc_reading_due(bool read_before,
	std::uint64_t footprint, std::uint64_t read_peak) noexcept
{
	return !read_before ||
		footprint >= read_peak + memory_malloc_peak_step_bytes;
}

/* What the checkpoint asks for the footprint's terms, malloc's included when
 * `read_malloc`; any thread. */
using MemoryTermsReader = MemoryTerms (*)(bool read_malloc) noexcept;

/* The reader the memory checkpoints use from now on; null for none, which
 * leaves the slot with the footprint alone. */
void set_memory_terms_reader(MemoryTermsReader reader) noexcept;

/* The counts of the guest's user network at one moment, each since the
 * provider was made (device/network/slirp/network_counters.hpp says what each
 * one counts). The record writes the difference between the reading at the
 * session's start and the reading at each checkpoint and at its end. */
struct SessionNetworkCounts final {
	std::uint64_t dns_answered{};
	std::uint64_t dns_policy_denied{};
	std::uint64_t dns_timed_out{};
	std::uint64_t dns_failed{};
	std::uint64_t dhcp_discovers{};
	std::uint64_t dhcp_requests{};
	std::uint64_t dhcp_acks{};
	std::uint64_t tcp_attempts{};
	std::uint64_t tcp_established{};
	std::uint64_t tcp_unreachable{};
	std::uint64_t tcp_refused{};
};

/* What the checkpoints and the end ask for the network's counts: false when
 * the guest has no network. Any thread, a signal handler included: it takes no
 * lock and allocates nothing. */
using SessionNetworkReader = bool (*)(SessionNetworkCounts &) noexcept;

/* The reader the sessions use from now on; null for none, which leaves the
 * network slot without counts. */
void set_network_counts_reader(SessionNetworkReader reader) noexcept;

/* Which iOS denial the application's notice is up for, as the application
 * states it (runtime API 36): none, or cellular or Wi-Fi access denied. The
 * record keeps how many times a denial began during the session, the session's
 * start included when one was under way, and the one in force. Any thread. */
enum class NetworkNotice : std::uint8_t {
	none,
	cellular_denied,
	wifi_denied,
};

void set_network_notice(NetworkNotice notice) noexcept;

enum class SessionEnd : std::uint8_t {
	running,
	powered_off,
	restarted,
	stopped,
	failed,
	guest_panic,
	kernel_abort,
	kernel_exit,
	hibernation_restore_failed,
	process_exit,
	host_fault,
};

/* The record's word for `end`. */
[[nodiscard]] std::string_view session_end_name(SessionEnd end) noexcept;

/* The descriptors that kernel arguments name as console output (con, conN,
 * ssl or sslN set to fd:IN,fd:OUT or fd:BOTH). */
[[nodiscard]] std::size_t console_output_descriptors(
	std::span<const std::string> kernel_arguments, std::span<int> descriptors)
	noexcept;

/* Opens the record at `path` for the session that is about to boot and
 * writes "running"; the console output the kernel writes to the descriptors
 * the arguments name is kept from here on. An empty path keeps no record.
 * Returns false, after one log line, when the file cannot be opened. */
bool begin_session_record(std::string_view path,
	std::span<const std::string> kernel_arguments) noexcept;

/* Ends the session with `end` and `status`, the run's result or the exit
 * status. The first end of a session wins; a later one changes nothing. */
void end_session_record(SessionEnd end, int status,
	std::string_view message = {}) noexcept;

/* The guest kernel crashed and the process is about to abort: records a
 * guest_panic with the kernel's panic message when its output carries one,
 * otherwise a kernel_abort, and names the calling thread after the panic, so
 * that the crash report, which shows thread names, says why. */
void record_kernel_crash() noexcept;

/* Writes the memory slot now; the checkpoint thread calls it. */
void checkpoint_session_memory() noexcept;

/* Whether a session is writing to a record file. */
[[nodiscard]] bool session_record_open() noexcept;

/* Replaces the stall slot with `slot`, which must be exactly
 * session_stall_slot_bytes of text; anything else changes nothing. Any
 * thread; it takes no lock, allocates nothing and does nothing without an
 * open session. */
void write_session_stall_slot(std::string_view slot) noexcept;

/* The loader's resolution of the kernel's write import. */
ssize_t kernel_write(int descriptor, const void *buffer, std::size_t length)
	noexcept;

/* kernel_write for the import named `name` (write, with or without the
 * leading underscore the Darwin SDK's asm labels add), else nullptr. */
[[nodiscard]] void *kernel_output_import(const char *name) noexcept;

/* What the record holds of the kernel's output: the last bytes, oldest first,
 * copied into `buffer`. For tests. */
std::size_t kernel_output_tail(std::span<char> buffer) noexcept;

/* The message of the last "Darwin UML panic: " line in `output`, or empty. */
[[nodiscard]] std::string_view panic_message(std::string_view output) noexcept;

/* The registers of the last host-fault report in `output` (the text after
 * "Darwin host fault: " on its "signal=" line), or empty. */
[[nodiscard]] std::string_view host_fault_message(std::string_view output)
	noexcept;

/* Forgets the session: points the record's descriptor at /dev/null and drops
 * the kept output. For tests. */
void reset_session_record() noexcept;

} // namespace uml::runtime
