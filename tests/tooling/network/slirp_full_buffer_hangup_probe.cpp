/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A host peer that hangs up while the guest is not taking the data SLiRP
 * holds for it must not abort the process. SLiRP reads a host socket into
 * its buffer for the guest only while that buffer is less than half full,
 * so a guest that stops reading leaves the buffer full and the socket unread.
 * The socket is still polled for writing while the guest has data to send,
 * and poll() reports a hang-up whether or not it was asked for. libslirp
 * handed that to soread(), which had no room to read into and asserted. The
 * iOS prefixes keep assertions and abort; the macOS prefix is built without
 * them, and soread() there read into an uninitialised buffer, which this
 * probe sees only in what the guest is told.
 *
 * Each scenario runs in its own process. The guest closes its window, a host
 * peer floods the connection until SLiRP's buffer is full and the rest of
 * the data waits in the host socket, and then the host side fails:
 *
 *   reset          the peer resets the connection and the guest then sends;
 *   reset-pending  the guest's own data is stuck in SLiRP when the peer
 *                  resets, so the error is still pending when it is polled;
 *   fin-then-data  the peer closes, as a server does an idle connection, and
 *                  the guest sends twice, the second time into the reset the
 *                  first write provoked;
 *   fin-idle       the peer closes and the guest sends nothing: no hang-up
 *                  is reported, and the guest still gets everything and then
 *                  the FIN once it reads;
 *   parked         the host reports a hang-up on a connection that is still
 *                  up (injected, no real socket does this): it is left
 *                  alone until the guest reads.
 *
 * A connection the host lost must reach the guest as a reset and release
 * SLiRP's socket. In every scenario the event loop stays quiet afterwards:
 * a hang-up that is only ignored is reported again on every pass.
 */
#include "slirp_tcp_harness.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

namespace {

using namespace slirp_harness;

constexpr std::uint16_t guest_port = 40000;
constexpr std::uint32_t guest_iss = 1000;
/*
 * SLiRP's buffer for the guest holds 128 KiB rounded up to whole 1460-octet
 * segments, and it reads a host socket in whole 1440-octet segments, the size
 * a connection has before any segment size applies. A read therefore leaves
 * the buffer full to its last octet only when the octets in it already leave
 * a whole number of segments free. The host peer sends in two parts for that:
 * the first leaves the buffer holding what the second needs to fill it.
 */
constexpr std::size_t buffer_size = 90 * 1460;
constexpr std::size_t read_size = 1440;
constexpr std::size_t first_part =
	buffer_size % read_size + 20 * read_size;
/* More than the buffer holds, less than the host socket can hold unread. */
constexpr std::size_t flood_bytes = 200000;
constexpr int quiet_passes = 30;
/* An idle loop finds nothing ready; a spinning one finds something on
 * every pass. */
constexpr std::uint64_t quiet_ready_limit = 3;

enum class Scenario { reset, reset_pending, fin_then_data, fin_idle, parked };
enum class Seen { absent, required };

struct Expectation {
	const char *name;
	Scenario scenario;
	Seen rst;
	Seen fin;
	/* What the guest has received at the end; zero where that is moot. */
	std::size_t bytes;
	/* Whether SLiRP's host socket is still open at the end. */
	bool socket_open;
};

constexpr std::array<Expectation, 5> expectations{{
	{"reset", Scenario::reset, Seen::required, Seen::absent, 0, false},
	{"reset-pending", Scenario::reset_pending, Seen::required, Seen::absent,
	 0, false},
	{"fin-then-data", Scenario::fin_then_data, Seen::required, Seen::absent,
	 0, false},
	{"fin-idle", Scenario::fin_idle, Seen::absent, Seen::required,
	 flood_bytes, true},
	{"parked", Scenario::parked, Seen::absent, Seen::absent, flood_bytes,
	 true},
}};

bool wait_for(const std::function<bool()> &condition, int timeout_ms)
{
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::milliseconds{timeout_ms};
	while (!condition()) {
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds{5});
	}
	return true;
}

std::size_t unread_bytes(int descriptor)
{
	int count = 0;
	return ioctl(descriptor, FIONREAD, &count) == 0 && count > 0 ?
		static_cast<std::size_t>(count) : 0;
}

/* SLiRP's end of the loopback connection is the only socket of this process
 * whose peer is the listener's port. */
int find_slirp_socket(std::uint16_t host_port)
{
	for (int descriptor = 3; descriptor < 1024; ++descriptor) {
		sockaddr_in peer{};
		socklen_t length = sizeof(peer);
		if (getpeername(descriptor, reinterpret_cast<sockaddr *>(&peer),
				&length) == 0 &&
		    peer.sin_family == AF_INET && ntohs(peer.sin_port) == host_port)
			return descriptor;
	}
	return -1;
}

std::uint16_t local_port_of(int descriptor)
{
	sockaddr_in local{};
	socklen_t length = sizeof(local);
	if (getsockname(descriptor, reinterpret_cast<sockaddr *>(&local),
			&length) != 0 || local.sin_family != AF_INET)
		return 0;
	return ntohs(local.sin_port);
}

bool socket_open_on(std::uint16_t port)
{
	for (int descriptor = 3; descriptor < 1024; ++descriptor)
		if (local_port_of(descriptor) == port)
			return true;
	return false;
}

/* The host side of the connection: a listener the probe owns, standing in for
 * the Internet, and the peer socket it accepted. SLiRP's own socket is found
 * by scanning and stays SLiRP's. */
struct Host {
	int listener{-1};
	int peer{-1};
	int slirp{-1};
	std::uint16_t port{};
	std::uint16_t slirp_port{};

	~Host()
	{
		if (listener >= 0)
			close(listener);
		if (peer >= 0)
			close(peer);
	}

	bool listen_on_loopback(int receive_buffer)
	{
		listener = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		socklen_t length = sizeof(address);
		if (listener < 0 ||
		    (receive_buffer != 0 &&
		     setsockopt(listener, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
				sizeof(receive_buffer)) != 0) ||
		    bind(listener, reinterpret_cast<const sockaddr *>(&address),
			 sizeof(address)) != 0 ||
		    listen(listener, 1) != 0 ||
		    getsockname(listener, reinterpret_cast<sockaddr *>(&address),
				&length) != 0)
			return false;
		port = ntohs(address.sin_port);
		return true;
	}

	void accept_if_pending()
	{
		pollfd entry{listener, POLLIN, 0};
		if (peer < 0 && poll(&entry, 1, 0) == 1)
			peer = accept(listener, nullptr, nullptr);
	}

	/* A close that sends a reset instead of a FIN. */
	void reset()
	{
		const linger abort_close{1, 0};
		(void)setsockopt(peer, SOL_SOCKET, SO_LINGER, &abort_close,
				 sizeof(abort_close));
		close(peer);
		peer = -1;
	}

	void close_peer()
	{
		close(peer);
		peer = -1;
	}
};

/* The guest as the probe writes it: what SLiRP has sent it so far, and the
 * sequence numbers to answer with. Its receive window is whatever the
 * probe says, zero until it reads. */
struct Guest {
	Network &network;
	std::uint16_t host_port{};
	std::uint32_t next{guest_iss + 1};
	std::uint32_t receive_next{};
	std::uint32_t received{};
	/* SLiRP's latest acknowledgement of the guest's data, and the window
	 * it offered. */
	std::uint32_t slirp_ack{};
	std::uint16_t slirp_window{};
	bool syn{};
	bool fin{};
	bool rst{};

	void observe()
	{
		while (!network.frames().empty()) {
			const auto segment = parse_tcp(network.frames().front());
			network.frames().pop_front();
			if (!segment || segment->destination_port != guest_port ||
			    segment->source_port != host_port)
				continue;
			if ((segment->flags & tcp_syn) != 0) {
				syn = true;
				receive_next = segment->seq + 1;
			} else if (syn && segment->seq == receive_next) {
				receive_next += static_cast<std::uint32_t>(
					segment->payload);
				received += static_cast<std::uint32_t>(
					segment->payload);
				if ((segment->flags & tcp_fin) != 0) {
					fin = true;
					++receive_next;
				}
			}
			if ((segment->flags & tcp_rst) != 0)
				rst = true;
			if ((segment->flags & tcp_ack) != 0) {
				slirp_ack = segment->ack;
				slirp_window = segment->window;
			}
		}
	}

	void pump(int wait_ms)
	{
		network.pump(1, wait_ms);
		observe();
	}

	void acknowledge(std::uint16_t window)
	{
		network.input(tcp_frame(guest_ip, gateway_ip, guest_port,
					host_port, next, receive_next, tcp_ack,
					window));
	}

	void send(std::size_t size)
	{
		network.input(tcp_frame(guest_ip, gateway_ip, guest_port,
					host_port, next, receive_next,
					tcp_ack | tcp_psh, 0, size));
		next += static_cast<std::uint32_t>(size);
		observe();
	}
};

/* Connects the guest to the listener through SLiRP and closes its window,
 * so that nothing SLiRP reads from the host reaches it. */
bool connect_guest(Guest &guest, Host &host)
{
	guest.host_port = host.port;
	guest.network.input(tcp_frame(guest_ip, gateway_ip, guest_port,
				      host.port, guest_iss, 0, tcp_syn, 65535));
	for (int pass = 0; pass < 300 && !(guest.syn && host.peer >= 0); ++pass) {
		guest.pump(10);
		host.accept_if_pending();
	}
	if (!guest.syn || host.peer < 0) {
		fprintf(stderr, "SLiRP did not connect the guest to the host\n");
		return false;
	}
	guest.acknowledge(0);
	guest.pump(10);
	host.slirp = find_slirp_socket(host.port);
	if (host.slirp < 0) {
		fprintf(stderr, "could not find SLiRP's host socket\n");
		return false;
	}
	host.slirp_port = local_port_of(host.slirp);
	return true;
}

bool send_all(int peer, std::size_t size)
{
	const std::vector<char> data(size, 'x');
	std::size_t sent = 0;
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds{5};
	while (sent < size && std::chrono::steady_clock::now() < deadline) {
		const auto count = send(peer, data.data() + sent, size - sent,
					MSG_DONTWAIT);
		if (count > 0) {
			sent += static_cast<std::size_t>(count);
		} else if (count < 0 && errno == EAGAIN) {
			pollfd entry{peer, POLLOUT, 0};
			(void)poll(&entry, 1, 50);
		} else {
			break;
		}
	}
	return sent == size;
}

/* The peer sends until SLiRP has filled its buffer for the guest to the last
 * octet and the rest of the data waits unread in SLiRP's socket. */
bool fill_buffer(Guest &guest, Host &host)
{
	const int large = 1 << 20;
	(void)setsockopt(host.slirp, SOL_SOCKET, SO_RCVBUF, &large, sizeof(large));
	(void)setsockopt(host.peer, SOL_SOCKET, SO_SNDBUF, &large, sizeof(large));
	/* Each part is all there before SLiRP reads, so that it reads the part
	 * it asks for. */
	const std::size_t second_part = flood_bytes - first_part;
	if (!send_all(host.peer, first_part) ||
	    !wait_for([&] { return unread_bytes(host.slirp) == first_part; },
		      5000)) {
		fprintf(stderr, "the first part did not reach SLiRP's socket\n");
		return false;
	}
	for (int pass = 0; pass < 10 && unread_bytes(host.slirp) != 0; ++pass)
		guest.pump(20);
	if (unread_bytes(host.slirp) != 0 || !send_all(host.peer, second_part) ||
	    !wait_for([&] { return unread_bytes(host.slirp) == second_part; },
		      5000)) {
		fprintf(stderr, "SLiRP did not read the first part, or the second "
			"did not reach it\n");
		return false;
	}
	for (int pass = 0; pass < 10; ++pass)
		guest.pump(20);
	const std::size_t taken = flood_bytes - unread_bytes(host.slirp);
	if (taken != buffer_size) {
		fprintf(stderr, "SLiRP took %zu of %zu bytes, not the %zu its "
			"buffer holds\n", taken, flood_bytes, buffer_size);
		return false;
	}
	return true;
}

/* The guest sends within SLiRP's window until the host socket and SLiRP's
 * buffer for the guest's data are full and the window stays closed, so the
 * socket is polled for writing from here on. */
bool stick_upload(Guest &guest)
{
	int closed = 0;
	for (int pass = 0; pass < 4000 && closed < 50; ++pass) {
		const std::uint32_t edge = guest.slirp_ack + guest.slirp_window;
		while (static_cast<std::int32_t>(edge - guest.next) > 0) {
			const std::uint32_t room = edge - guest.next;
			guest.send(room < 1460 ? room : 1460);
		}
		guest.pump(1);
		closed = guest.slirp_window == 0 && guest.slirp_ack == guest.next ?
			closed + 1 : 0;
	}
	if (closed < 50) {
		fprintf(stderr, "SLiRP never closed the window for the guest's "
			"data\n");
		return false;
	}
	return true;
}

/* Passes of an event loop that has nothing to do must not find a socket
 * ready. */
std::uint64_t ready_while_quiet(Guest &guest)
{
	const auto before = guest.network.ready_polls();
	for (int pass = 0; pass < quiet_passes; ++pass)
		guest.pump(20);
	return guest.network.ready_polls() - before;
}

/* The guest opens its window and reads until all the data is in, and the
 * FIN too if the host closed. */
void read_everything(Guest &guest, bool until_fin)
{
	for (int pass = 0; pass < 3000; ++pass) {
		const auto before = guest.receive_next;
		guest.pump(5);
		if (guest.receive_next != before || pass == 0)
			guest.acknowledge(65535);
		if (guest.received >= flood_bytes && (guest.fin || !until_fin))
			return;
	}
}

struct Outcome {
	bool rst{};
	bool fin{};
	std::size_t bytes{};
	bool socket_open{};
	std::uint64_t ready{};
};

bool run_scenario(Scenario scenario, Outcome &outcome)
{
	Network network;
	if (network.slirp() == nullptr || !network.resolve()) {
		fprintf(stderr, "SLiRP did not start or answer ARP\n");
		return false;
	}
	const bool stuck = scenario == Scenario::reset_pending ||
		scenario == Scenario::parked;
	Host host;
	/* A peer that never reads and has a small buffer stops taking the
	 * guest's data soon. */
	if (!host.listen_on_loopback(stuck ? 4096 : 0)) {
		fprintf(stderr, "could not listen on loopback\n");
		return false;
	}
	Guest guest{network};
	if (!connect_guest(guest, host) || !fill_buffer(guest, host) ||
	    (stuck && !stick_upload(guest)))
		return false;

	switch (scenario) {
	case Scenario::reset:
		host.reset();
		std::this_thread::sleep_for(std::chrono::milliseconds{100});
		guest.send(100);
		break;
	case Scenario::reset_pending:
		host.reset();
		std::this_thread::sleep_for(std::chrono::milliseconds{100});
		break;
	case Scenario::fin_then_data:
		host.close_peer();
		std::this_thread::sleep_for(std::chrono::milliseconds{100});
		guest.send(100);
		std::this_thread::sleep_for(std::chrono::milliseconds{100});
		guest.send(100);
		break;
	case Scenario::fin_idle:
		host.close_peer();
		std::this_thread::sleep_for(std::chrono::milliseconds{100});
		break;
	case Scenario::parked:
		network.inject_revents(POLLHUP);
		break;
	}
	/* A lost connection is reset toward the guest soon; the others stay as
	 * they are. */
	const bool lost = scenario != Scenario::fin_idle &&
		scenario != Scenario::parked;
	for (int pass = 0; pass < (lost ? 50 : 5) && !guest.rst; ++pass)
		guest.pump(lost ? 100 : 20);
	outcome.ready = ready_while_quiet(guest);
	if (!lost)
		read_everything(guest, scenario == Scenario::fin_idle);
	outcome.rst = guest.rst;
	outcome.fin = guest.fin;
	outcome.bytes = guest.received;
	outcome.socket_open = socket_open_on(host.slirp_port);
	return true;
}

bool seen_as_expected(Seen expected, bool seen)
{
	return (expected == Seen::required) == seen;
}

/* One scenario in a child process, so that an abort in libslirp is a failure
 * of that scenario and the others still run. */
bool run_in_child(const Expectation &expected)
{
	fflush(stdout);
	fflush(stderr);
	const pid_t child = fork();
	if (child < 0) {
		fprintf(stderr, "FAIL %s: could not fork\n", expected.name);
		return false;
	}
	if (child == 0) {
		alarm(60);
		Outcome outcome;
		if (!run_scenario(expected.scenario, outcome)) {
			fprintf(stderr, "FAIL %s: the scenario did not start\n",
				expected.name);
			_exit(2);
		}
		printf("%s: rst_to_guest=%d fin_to_guest=%d bytes_to_guest=%zu "
		       "socket_open=%d ready_polls=%llu\n",
		       expected.name, outcome.rst ? 1 : 0, outcome.fin ? 1 : 0,
		       outcome.bytes, outcome.socket_open ? 1 : 0,
		       static_cast<unsigned long long>(outcome.ready));
		bool passed = true;
		if (outcome.ready > quiet_ready_limit) {
			fprintf(stderr, "FAIL %s: %llu of %d passes found a "
				"descriptor ready, the loop is spinning\n",
				expected.name,
				static_cast<unsigned long long>(outcome.ready),
				quiet_passes);
			passed = false;
		}
		if (!seen_as_expected(expected.rst, outcome.rst)) {
			fprintf(stderr, "FAIL %s: the guest got %s\n",
				expected.name,
				expected.rst == Seen::required ? "no RST" : "a RST");
			passed = false;
		}
		if (!seen_as_expected(expected.fin, outcome.fin)) {
			fprintf(stderr, "FAIL %s: the guest got %s\n",
				expected.name,
				expected.fin == Seen::required ? "no FIN" : "a FIN");
			passed = false;
		}
		if (expected.bytes != 0 && outcome.bytes != expected.bytes) {
			fprintf(stderr, "FAIL %s: the guest read %zu bytes, "
				"expected %zu\n", expected.name, outcome.bytes,
				expected.bytes);
			passed = false;
		}
		if (outcome.socket_open != expected.socket_open) {
			fprintf(stderr, "FAIL %s: SLiRP's host socket is %s\n",
				expected.name,
				outcome.socket_open ? "still open" : "closed");
			passed = false;
		}
		fflush(stdout);
		fflush(stderr);
		_exit(passed ? 0 : 1);
	}
	int status = 0;
	if (waitpid(child, &status, 0) != child) {
		fprintf(stderr, "FAIL %s: lost the scenario's process\n",
			expected.name);
		return false;
	}
	if (WIFSIGNALED(status)) {
		fprintf(stderr, "FAIL %s: libslirp died on signal %d\n",
			expected.name, WTERMSIG(status));
		return false;
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

int main(void)
{
	/* libslirp writes its host sockets with plain send(). */
	(void)signal(SIGPIPE, SIG_IGN);
	bool passed = true;
	for (const auto &expected : expectations)
		passed = run_in_child(expected) && passed;
	if (!passed)
		return 1;
	printf("PASS: a host socket that hangs up while SLiRP's buffer for the "
	       "guest is full is reset toward the guest and closed, or left "
	       "alone, and never aborts or spins the event loop\n");
	return 0;
}
