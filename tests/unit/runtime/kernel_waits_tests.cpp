// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The kernel's blocking imports, marked (runtime/kernel_waits): each wrapper
 * publishes what the calling thread waits for in its registry slot for the
 * length of the call, makes the system's call and leaves the slot as it was
 * but for the count of marks. A call that cannot block is not marked.
 */
#include "kernel_threads.hpp"
#include "kernel_waits.hpp"
#include "platform/host_wait.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"
#include "thread_nap.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <mach/mach.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/select.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using uml::platform::HostWait;
using uml::runtime::kernel_wait_import;
using uml::testing::nap;
using uml::testing::require;

struct Job final {
	std::function<void()> run;
	std::atomic<mach_port_t> port{MACH_PORT_NULL};
	std::atomic<bool> finished{false};
};

void *job_main(void *argument)
{
	auto *job = static_cast<Job *>(argument);
	job->port.store(mach_thread_self(), std::memory_order_release);
	job->run();
	job->finished.store(true, std::memory_order_release);
	return nullptr;
}

/* What the thread whose port this is says it waits for, or 0. */
std::uint64_t word_of(mach_port_t port)
{
	uml::runtime::KernelThreadSnapshot snapshot{};
	uml::runtime::snapshot_kernel_threads(snapshot);
	for (std::size_t index = 0; index < snapshot.thread_count; ++index)
		if (snapshot.threads[index] == port)
			return snapshot.waits[index];
	return 0;
}

bool within(std::chrono::milliseconds limit, const std::function<bool()> &done)
{
	const auto deadline = std::chrono::steady_clock::now() + limit;
	while (!done()) {
		if (std::chrono::steady_clock::now() > deadline)
			return false;
		nap(2000);
	}
	return true;
}

/* Runs `call` on a registered thread and returns its port, once it has one. */
struct Running final {
	Job job;
	pthread_t thread{};

	explicit Running(std::function<void()> call)
	{
		job.run = std::move(call);
		require(uml::runtime::kernel_pthread_create(&thread, nullptr, job_main,
			&job) == 0);
		require(within(std::chrono::seconds{3}, [this] {
			return job.port.load(std::memory_order_acquire) != MACH_PORT_NULL;
		}));
	}

	[[nodiscard]] mach_port_t port() const
	{
		return job.port.load(std::memory_order_acquire);
	}

	void join()
	{
		require(pthread_join(thread, nullptr) == 0);
	}
};

/* A blocking call is marked with `kind` while it blocks, and the thread
 * leaves the call with no mark but one more in its count. */
void marks_while_blocked(HostWait kind, const std::function<void()> &call,
	const std::function<void()> &release)
{
	Running running{call};
	require(within(std::chrono::seconds{3}, [&] {
		return uml::platform::wait_kind(word_of(running.port())) == kind;
	}), "the call was never seen waiting");
	const auto during = word_of(running.port());
	require(uml::platform::wait_sequence(during) >= 1);
	release();
	require(within(std::chrono::seconds{3}, [&] {
		return running.job.finished.load(std::memory_order_acquire);
	}), "the call never ended");
	running.join();
	const auto after = word_of(running.port());
	require(uml::platform::wait_kind(after) == HostWait::none &&
		uml::platform::wait_sequence(after) ==
		uml::platform::wait_sequence(during));
}

template<typename Function>
Function import_of(const char *name)
{
	void *const wrapper = kernel_wait_import(name);
	require(wrapper != nullptr, name);
	return reinterpret_cast<Function>(wrapper);
}

void blocking_calls_are_marked_for_as_long_as_they_block()
{
	using Read = ssize_t (*)(int, void *, size_t);
	using Poll = int (*)(pollfd *, nfds_t, int);
	using Pselect = int (*)(int, fd_set *, fd_set *, fd_set *, const timespec *,
		const sigset_t *);
	using Recvfrom = ssize_t (*)(int, void *, size_t, int, sockaddr *,
		socklen_t *);
	using CondWait = int (*)(pthread_cond_t *, pthread_mutex_t *);

	int pipe_fds[2];
	require(pipe(pipe_fds) == 0);
	const auto wake = [&] { (void)!write(pipe_fds[1], "x", 1); };

	/* The same wrapper under the names the kernel uses for it. */
	require(kernel_wait_import("read") == kernel_wait_import("_read"));
	require(kernel_wait_import("pselect$DARWIN_EXTSN") ==
		kernel_wait_import("_pselect$DARWIN_EXTSN"));

	marks_while_blocked(HostWait::receive, [&] {
		char byte;
		(void)import_of<Read>("_read")(pipe_fds[0], &byte, 1);
	}, wake);

	marks_while_blocked(HostWait::answer, [&] {
		pollfd descriptor{pipe_fds[0], POLLIN, 0};
		(void)import_of<Poll>("poll")(&descriptor, 1, -1);
	}, wake);
	/* A poll leaves what it waited for in the pipe. */
	char leftover;
	(void)!read(pipe_fds[0], &leftover, 1);

	marks_while_blocked(HostWait::idle, [&] {
		fd_set readable;
		FD_ZERO(&readable);
		FD_SET(pipe_fds[0], &readable);
		(void)import_of<Pselect>("_pselect$DARWIN_EXTSN")(pipe_fds[0] + 1,
			&readable, nullptr, nullptr, nullptr, nullptr);
	}, wake);
	(void)!read(pipe_fds[0], &leftover, 1);

	int pair[2];
	require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	marks_while_blocked(HostWait::receive, [&] {
		char byte;
		(void)import_of<Recvfrom>("recvfrom")(pair[0], &byte, 1, 0, nullptr,
			nullptr);
	}, [&] { (void)!write(pair[1], "y", 1); });

	/* A condition wait is how a parked vCPU idles. */
	static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
	static std::atomic<bool> signalled{false};
	signalled = false;
	marks_while_blocked(HostWait::idle, [&] {
		pthread_mutex_lock(&mutex);
		while (!signalled.load())
			(void)import_of<CondWait>("pthread_cond_wait")(&condition, &mutex);
		pthread_mutex_unlock(&mutex);
	}, [&] {
		pthread_mutex_lock(&mutex);
		signalled = true;
		pthread_cond_broadcast(&condition);
		pthread_mutex_unlock(&mutex);
	});
	close(pair[0]);
	close(pair[1]);
	close(pipe_fds[0]);
	close(pipe_fds[1]);
}

/* A call that waits for nothing, or cannot be seen waiting, still counts one
 * mark, except an immediate poll or select. */
void other_calls_are_marked_and_immediate_ones_are_not()
{
	using Pread = ssize_t (*)(int, void *, size_t, off_t);
	using Pwrite = ssize_t (*)(int, const void *, size_t, off_t);
	using Fsync = int (*)(int);
	using Poll = int (*)(pollfd *, nfds_t, int);
	using Pselect = int (*)(int, fd_set *, fd_set *, fd_set *, const timespec *,
		const sigset_t *);

	const uml::testing::ScratchSubdirectory directory{"uml-kernel-waits.XXXXXX"};
	const std::string path = (directory.path() / "file").string();
	const int file = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	require(file >= 0);
	(void)unlink(path.c_str());

	std::atomic<std::uint64_t> sequence_after_marked{0};
	std::atomic<std::uint64_t> sequence_after_immediate{0};
	std::atomic<std::uint64_t> sequence_before{0};
	Running running{[&] {
		const auto word = [] {
			return uml::platform::current_wait_cell->word();
		};
		sequence_before = uml::platform::wait_sequence(word());
		char data[16] = "0123456789abcde";
		require(import_of<Pwrite>("pwrite")(file, data, 16, 0) == 16);
		char back[16];
		require(import_of<Pread>("pread")(file, back, 16, 0) == 16);
		require(std::memcmp(data, back, 16) == 0);
		require(import_of<Fsync>("fsync")(file) == 0);
		sequence_after_marked = uml::platform::wait_sequence(word());
		pollfd descriptor{file, POLLIN, 0};
		require(import_of<Poll>("poll")(&descriptor, 1, 0) >= 0);
		const timespec none{0, 0};
		require(import_of<Pselect>("pselect")(0, nullptr, nullptr, nullptr,
			&none, nullptr) == 0);
		sequence_after_immediate = uml::platform::wait_sequence(word());
	}};
	require(within(std::chrono::seconds{3}, [&] {
		return running.job.finished.load(std::memory_order_acquire);
	}));
	running.join();
	require(sequence_after_marked == sequence_before + 3,
		"pwrite, pread and fsync are one mark each");
	require(sequence_after_immediate == sequence_after_marked,
		"an immediate poll or select was marked");
	(void)close(file);
}

void calls_the_loader_does_not_hand_over_are_not_wrapped()
{
	require(kernel_wait_import("malloc") == nullptr);
	require(kernel_wait_import("write") == nullptr);
	require(kernel_wait_import("recvmsg") == nullptr);
	require(kernel_wait_import("no_such_call") == nullptr);
	require(kernel_wait_import(nullptr) == nullptr);
}

/* A thread the registry does not list marks nothing and is not harmed. */
void a_thread_without_a_cell_marks_nothing()
{
	using Read = ssize_t (*)(int, void *, size_t);
	require(uml::platform::current_wait_cell == nullptr);
	int fds[2];
	require(pipe(fds) == 0);
	(void)!write(fds[1], "z", 1);
	char byte = 0;
	require(import_of<Read>("read")(fds[0], &byte, 1) == 1 && byte == 'z');
	close(fds[0]);
	close(fds[1]);
}

} // namespace

/* A signal handler's marked write inside a wait it interrupted gives the
 * thread back to that wait, as a new sighting of it. */
void a_mark_inside_another_gives_the_thread_back_to_the_outer_wait()
{
	using uml::platform::ScopedHostWait;
	using uml::platform::WaitCell;
	using uml::platform::wait_kind;
	using uml::platform::wait_sequence;
	WaitCell cell;
	uml::platform::current_wait_cell = &cell;
	{
		const ScopedHostWait outer{HostWait::idle};
		require(wait_kind(cell.word()) == HostWait::idle);
		const std::uint64_t outer_sequence = wait_sequence(cell.word());
		{
			const ScopedHostWait inner{HostWait::send};
			require(wait_kind(cell.word()) == HostWait::send);
			{
				const ScopedHostWait innermost{HostWait::disk_write};
				require(wait_kind(cell.word()) == HostWait::disk_write);
			}
			require(wait_kind(cell.word()) == HostWait::send);
		}
		require(wait_kind(cell.word()) == HostWait::idle);
		/* The sequence only goes forward, or a reader would take a later wait
		 * for the one it saw. */
		require(wait_sequence(cell.word()) == outer_sequence + 2);
	}
	require(wait_kind(cell.word()) == HostWait::none);
	uml::platform::current_wait_cell = nullptr;
}

int main()
{
	a_mark_inside_another_gives_the_thread_back_to_the_outer_wait();
	a_thread_without_a_cell_marks_nothing();
	calls_the_loader_does_not_hand_over_are_not_wrapped();
	blocking_calls_are_marked_for_as_long_as_they_block();
	other_calls_are_marked_and_immediate_ones_are_not();
	std::puts("kernel waits tests passed");
	return 0;
}
