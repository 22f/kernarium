// SPDX-License-Identifier: GPL-2.0-or-later
// Runs the Darwin port's real idle wait (time.c) and IPI code (smp.c) on this
// thread as CPU0, with the host calls that matter counted, and checks what an
// idle cycle does with the IPI wake pipe: no look at it when the wait found it
// empty and no vector is pending, one drain at most, and no vector lost or
// delayed when a sender races the wait.
#include <atomic>
#include <cassert>
#include <cerrno>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/select.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

extern "C" {
void os_init_smp(void);
void os_idle_sleep(void);
int os_send_ipi(int cpu, int vector);
void os_raise_local_ipi(int vector);
int os_timer_create(void);
int os_timer_one_shot(int cpu, unsigned long long nsecs);
long long os_nsecs(void);
}

// The host calls the idle exit makes, counted while the tracked thread runs.
static thread_local bool tracked = false;
static std::atomic<int> polls{0}, reads{0}, selects{0};
// Run once, on the tracked thread, as the next wait returns or as the next
// vectors are handed over: a sender that races exactly there.
static std::function<void()> after_wait, after_exchange;

template <typename T> static T next_symbol(const char *name) {
	return reinterpret_cast<T>(dlsym(RTLD_NEXT, name));
}
extern "C" {
int poll(struct pollfd *fds, nfds_t count, int timeout) {
	static auto real = next_symbol<int (*)(struct pollfd *, nfds_t, int)>("poll");
	if (tracked) polls++;
	return real(fds, count, timeout);
}
ssize_t read(int fd, void *buffer, size_t size) {
	static auto real = next_symbol<ssize_t (*)(int, void *, size_t)>("read");
	if (tracked) reads++;
	return real(fd, buffer, size);
}
int pselect(int count, fd_set *in, fd_set *out, fd_set *error, const struct timespec *timeout, const sigset_t *mask) {
	static auto real = next_symbol<int (*)(int, fd_set *, fd_set *, fd_set *, const struct timespec *, const sigset_t *)>("pselect");
	if (tracked) selects++;
	int result = real(count, in, out, error, timeout, mask);
	if (tracked && after_wait) {
		int saved = errno;
		auto hook = std::move(after_wait);
		after_wait = nullptr;
		hook();
		errno = saved;
	}
	return result;
}
}
// A sender held between setting its vector and writing its byte: it parks in
// its write until released, which the test does once the idle exit has run.
static thread_local bool hold_write = false;
static std::atomic<bool> held{false}, release{false};
extern "C" ssize_t write(int fd, const void *buffer, size_t size) {
	static auto real = next_symbol<ssize_t (*)(int, const void *, size_t)>("write");
	if (hold_write) {
		hold_write = false;
		held = true;
		while (!release) std::this_thread::yield();
	}
	return real(fd, buffer, size);
}
static ssize_t raw_read(int fd, void *buffer, size_t size) {
	static auto real = next_symbol<ssize_t (*)(int, void *, size_t)>("read");
	return real(fd, buffer, size);
}

// What the kernel provides around the two files.
struct Handled { long long ns; unsigned mask; };
static std::vector<Handled> handled;
static std::atomic<bool> timer_fired{false};
static std::jmp_buf init_escape;
static bool initializing = false;
static int stop_pipe[2] = {-1, -1};
extern "C" {
int uml_ncpus = 2;
void panic(const char *format, ...) {
	// os_init_smp() ends by asking the carrier machinery, which this test
	// does not start, to name CPU0. Everything it sets up has been done by then.
	if (initializing && strstr(format, "fixed carrier")) std::longjmp(init_escape, 1);
	std::fprintf(stderr, "panic: %s\n", format);
	std::abort();
}
void os_warn(const char *format, ...) { std::fprintf(stderr, "warning: %s", format); }
int um_set_signals(int) { return 1; }
int um_get_signals(void) { return 1; }
void um_trace_signals_on(void) {}
void um_trace_signals_off(void) {}
void unblock_signals(void) {}
int darwin_timers_cooperative(void) { return 1; }
void timer_handler(int, void *, struct uml_pt_regs *) { timer_fired = true; }
int darwin_uml_stop_fd(void) { return stop_pipe[0]; }
// A stop request is a byte on the host's stop descriptor; the checkpoint takes it.
void darwin_host_stop_checkpoint(void) {
	char byte;
	if (stop_pipe[0] >= 0) (void)!raw_read(stop_pipe[0], &byte, 1);
}
int darwin_task_shutdown_requested(void) { return 0; }
int darwin_reserve_embedded_thread(void) { return 0; }
void darwin_release_embedded_thread(void) {}
void darwin_timer_kernel_checkpoint(void) {}
int darwin_irq_idle_begin(void) { return 0; }
void darwin_irq_idle_end(void) {}
int darwin_irq_dispatch(int, unsigned long long, struct uml_pt_regs *) { return 0; }
int os_set_fd_block(int fd, int blocking) {
	int flags = fcntl(fd, F_GETFL);
	return fcntl(fd, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
}
int os_set_exec_close(int fd) { return fcntl(fd, F_SETFD, FD_CLOEXEC); }
void set_sigstack(void *, int) {}
void new_thread(void *, jmp_buf *, void (*)(void)) {}
void new_thread_handler(void) {}
void uml_finishsetup(void) {}
void uml_start_secondary(void *) {}
// The idle exit hands the vectors it found to the kernel here, after it took
// them from the pending mask.
void uml_ipi_handler_mask(unsigned mask) {
	handled.push_back({os_nsecs(), mask});
	if (after_exchange) {
		auto hook = std::move(after_exchange);
		after_exchange = nullptr;
		hook();
	}
}
}

struct Cycle {
	double ms;
	int polls, reads;
	bool timer;
	unsigned mask;
};
// One idle cycle with a clock deadline `deadline_ms` away.
static Cycle cycle(double deadline_ms) {
	os_timer_one_shot(0, (unsigned long long)(deadline_ms * 1e6));
	handled.clear();
	timer_fired = false;
	polls = reads = selects = 0;
	long long start = os_nsecs();
	tracked = true;
	os_idle_sleep();
	tracked = false;
	Cycle result{(double)(os_nsecs() - start) / 1e6, polls, reads, timer_fired, 0};
	for (const Handled &entry : handled) result.mask |= entry.mask;
	return result;
}
static bool quiet(const Cycle &c, double deadline_ms) {
	// Woken by the clock only: nothing drained, nothing handled.
	return c.timer && c.polls == 0 && c.reads == 0 && c.mask == 0 && c.ms >= deadline_ms * 0.5;
}
static void sleep_us(long us) { timespec t{us / 1000000, (us % 1000000) * 1000L}; nanosleep(&t, nullptr); }

int main() {
	assert(pipe(stop_pipe) == 0 && os_set_fd_block(stop_pipe[0], 0) == 0);
	initializing = true;
	if (setjmp(init_escape) == 0) os_init_smp();
	initializing = false;
	assert(os_timer_create() == 0);
	pthread_t self = pthread_self();

	// A wake by the clock finds the pipe empty and, with no vector pending,
	// looks at it no further.
	for (int i = 0; i < 20; i++) {
		Cycle c = cycle(2);
		std::printf("deadline wake: %.2f ms, polls %d, reads %d\n", c.ms, c.polls, c.reads);
		assert(quiet(c, 2));
	}
	// So does a wake by the host's stop descriptor, which is not the pipe.
	assert(write(stop_pipe[1], "s", 1) == 1);
	{
		Cycle c = cycle(5000);
		assert(c.ms < 2500 && !c.timer && c.polls == 0 && c.reads == 0 && c.mask == 0);
		assert(quiet(cycle(2), 2));
	}

	// A vector sent before the wait: the wait must not sleep, drains the byte
	// once and hands the vector over once.
	assert(os_send_ipi(0, 3) == 0);
	{
		Cycle c = cycle(5000);
		assert(c.ms < 2500 && c.mask == 1u << 3 && c.polls >= 1 && c.reads == 1);
		assert(quiet(cycle(2), 2));
	}
	// A pending vector which has no byte behind it (a local one) ends the wait
	// at once as well; the wait found the pipe empty, so the poll that hands
	// the vector over drains it, once, and finds nothing.
	os_raise_local_ipi(11);
	{
		Cycle c = cycle(5000);
		assert(c.ms < 2500 && c.mask == 1u << 11 && c.polls == 1 && c.reads == 0);
		assert(quiet(cycle(2), 2));
	}

	// A vector sent during the wait ends it, and the byte is drained.
	{
		std::thread sender([] { sleep_us(20000); os_send_ipi(0, 5); });
		Cycle c = cycle(5000);
		sender.join();
		assert(c.ms < 2500 && c.mask == 1u << 5 && c.polls >= 1 && c.reads == 1);
		assert(quiet(cycle(2), 2));
	}

	// A vector sent after the wait looked at the pipe and before the exit took
	// the pending mask is not lost: the exit hands it over, and drains its byte
	// with the poll that finds the vector pending, so nothing is left behind.
	after_wait = [] { assert(os_send_ipi(0, 7) == 0); };
	{
		Cycle c = cycle(2);
		std::printf("sent after the wait: mask %#x, polls %d, reads %d\n", c.mask, c.polls, c.reads);
		assert(c.mask == 1u << 7 && c.polls == 2 && c.reads == 1);
		assert(quiet(cycle(2), 2));
	}
	// A sender held between its vector and its byte until the exit has taken
	// the mask: the vector is handed over, and the byte which follows ends the
	// next wait at once and is drained there: one spurious cycle, then quiet.
	{
		std::thread sender([] { hold_write = true; os_send_ipi(0, 6); });
		for (int spin = 0; spin < 5000 && !held; spin++) sleep_us(1000);
		assert(held);
		Cycle c = cycle(2);
		assert(c.mask == 1u << 6 && c.polls == 1 && c.reads == 0);
		release = true;
		sender.join();
		Cycle spurious = cycle(5000);
		std::printf("byte after the exit: spurious cycle %.2f ms, polls %d, reads %d\n", spurious.ms, spurious.polls, spurious.reads);
		assert(spurious.ms < 2500 && spurious.mask == 0 && !spurious.timer && spurious.reads == 1);
		assert(quiet(cycle(2), 2));
	}
	// A vector sent after the exit took the mask is not lost either: the next
	// wait finds it pending and the byte readable.
	after_exchange = [] { assert(os_send_ipi(0, 9) == 0); };
	os_raise_local_ipi(1);
	{
		Cycle c = cycle(5000);
		assert(c.ms < 2500 && c.mask == 1u << 1);
		Cycle next = cycle(5000);
		assert(next.ms < 2500 && next.mask == 1u << 9 && next.reads == 1);
		assert(quiet(cycle(2), 2));
	}

	// A signal ends the wait with an error, which leaves the result set
	// undefined, so that cycle drains as it always did.
	{
		std::atomic<bool> finished{false};
		std::thread sender([&] { while (!finished) { pthread_kill(self, SIGUSR1); sleep_us(5000); } });
		Cycle c = cycle(5000);
		finished = true;
		sender.join();
		assert(c.ms < 2500 && c.polls == 1 && c.mask == 0 && !c.timer);
		assert(quiet(cycle(2), 2));
	}

	// Sends at every offset against a wait which ends by the clock about then:
	// each is handed over promptly, whichever side of the wait it lands on, and
	// costs at most one spurious cycle, so that a lost wake (which would cost
	// the whole deadline) and a pile of stale bytes both show.
	{
		std::atomic<int> request{0};
		std::atomic<bool> stop{false};
		std::atomic<long> offset_ns{0};
		std::thread sender([&] {
			int served = 0;
			while (!stop) {
				if (request.load() == served) { std::this_thread::yield(); continue; }
				served = request.load();
				for (long long until = os_nsecs() + offset_ns; os_nsecs() < until;) {}
				os_send_ipi(0, 1 + served % 8);
			}
		});
		int spurious = 0, rounds = 400;
		unsigned seed = 7;
		for (int round = 1; round <= rounds; round++) {
			seed = seed * 1103515245u + 12345u;
			offset_ns = (long)((seed >> 16) % 300) * 1000;
			unsigned want = 1u << (1 + round % 8);
			long long start = os_nsecs();
			request = round;
			int cycles = 0;
			unsigned got = 0;
			// The first wait ends about when the send lands, by the clock.
			double deadline = (double)((seed >> 5) % 400) / 1000;
			while (!(got & want)) {
				Cycle c = cycle(deadline);
				deadline = 1000;
				got |= c.mask;
				assert(++cycles <= 3);
				// A wake by neither the clock nor a vector: a stale byte.
				if (!c.mask && !c.timer) spurious++;
			}
			assert(os_nsecs() - start < 500000000);
		}
		stop = true;
		sender.join();
		std::printf("%d sends against the wait: %d spurious cycles\n", rounds, spurious);
		assert(spurious <= rounds);
		// Whatever was left behind is gone after at most one more cycle.
		Cycle c = cycle(2);
		if (c.mask || !c.timer) c = cycle(2);
		assert(quiet(c, 2));
	}
	std::puts("ipi drain ok");
	return 0;
}
