/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A guest cannot reach the host it runs on. The probe drives the provider the
 * way a guest does, with sockets of its own on the host's loopback and on the
 * host's LAN address, and requires that a guest's TCP and UDP to the gateway
 * address (which SLiRP would map to the host's 127.0.0.1), to the broadcast
 * address, to the host's own addresses and to 127.0.0.1 itself are refused at
 * once and arrive at no host socket.
 *
 * What the guest is meant to reach keeps working in the same configuration:
 * DNS at the name server (over UDP and TCP), DHCP, an ICMP echo to the
 * gateway, an inbound host forward (also when the host's own client comes
 * through one of the host's addresses), and a destination that is neither
 * the host nor the virtual network, which goes to SLiRP. The provider looks
 * the host's addresses up when a frame that needs them finds the table older
 * than a second, never otherwise; the hooks of the test build take the place
 * of the system's list and of the clock.
 *
 * Every listener is the probe's own: on 127.0.0.1 and on the LAN address, at
 * a port the host picks, closed at the end.
 */
#include "network_counters.hpp"
#include "provider.hpp"
#include "slirp_tcp_harness.hpp"

#include <dns_sd.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace slirp_harness;
using Address = std::array<std::uint8_t, 4>;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
using uml::network::HostAddresses;

constexpr Address gateway{10, 0, 2, 2};
constexpr Address name_server{10, 0, 2, 3};
constexpr Address broadcast{255, 255, 255, 255};
constexpr Address loopback{127, 0, 0, 1};
/* TEST-NET-1: no host holds it; it stands for another machine. */
constexpr Address elsewhere{192, 0, 2, 77};

/* How long a refusal may take, and how long the probe watches a host socket
 * for something that must not come. */
constexpr int prompt_ms = 2000;
constexpr int quiet_ms = 250;

int failures;

void fail(const char *label, const std::string &message)
{
	std::fprintf(stderr, "%s: %s\n", label, message.c_str());
	failures++;
}

std::uint32_t value(const Address &address)
{
	return (static_cast<std::uint32_t>(address[0]) << 24) |
		(static_cast<std::uint32_t>(address[1]) << 16) |
		(static_cast<std::uint32_t>(address[2]) << 8) | address[3];
}

std::string text(const Address &address)
{
	return std::to_string(address[0]) + "." + std::to_string(address[1]) +
		"." + std::to_string(address[2]) + "." + std::to_string(address[3]);
}

Bytes udp_frame(const Address &source, const Address &destination,
		std::uint16_t source_port, std::uint16_t destination_port,
		const Bytes &payload)
{
	const std::size_t udp_length = 8 + payload.size();
	Bytes frame(14 + 20 + udp_length, 0);
	for (std::size_t index = 0; index < 6; ++index) {
		frame[index] = 0xff;
		frame[6 + index] = guest_mac[index];
	}
	put16(frame, 12, 0x0800);
	frame[14] = 0x45;
	put16(frame, 16, static_cast<std::uint16_t>(20 + udp_length));
	put16(frame, 20, 0x4000);
	frame[22] = 64;
	frame[23] = IPPROTO_UDP;
	for (std::size_t index = 0; index < 4; ++index) {
		frame[26 + index] = source[index];
		frame[30 + index] = destination[index];
	}
	put16(frame, 24, checksum_fold(checksum_add(0, &frame[14], 20)));
	put16(frame, 34, source_port);
	put16(frame, 36, destination_port);
	put16(frame, 38, static_cast<std::uint16_t>(udp_length));
	std::copy(payload.begin(), payload.end(), frame.begin() + 42);
	std::uint32_t sum = checksum_add(0, &frame[26], 8);
	sum += IPPROTO_UDP;
	sum += static_cast<std::uint32_t>(udp_length);
	sum = checksum_add(sum, &frame[34], udp_length);
	const auto folded = checksum_fold(sum);
	put16(frame, 40, folded == 0 ? 0xffff : folded);
	return frame;
}

Bytes icmp_echo_frame(const Address &destination)
{
	Bytes frame(14 + 20 + 8 + 4, 0);
	for (std::size_t index = 0; index < 6; ++index) {
		frame[index] = 0xff;
		frame[6 + index] = guest_mac[index];
	}
	put16(frame, 12, 0x0800);
	frame[14] = 0x45;
	put16(frame, 16, 32);
	put16(frame, 20, 0x4000);
	frame[22] = 64;
	frame[23] = IPPROTO_ICMP;
	for (std::size_t index = 0; index < 4; ++index) {
		frame[26 + index] = guest_ip[index];
		frame[30 + index] = destination[index];
	}
	put16(frame, 24, checksum_fold(checksum_add(0, &frame[14], 20)));
	frame[34] = 8;
	put16(frame, 38, 0x4a17);
	put16(frame, 40, 1);
	put16(frame, 36, checksum_fold(checksum_add(0, &frame[34], 12)));
	return frame;
}

Bytes arp_request()
{
	Bytes frame(42, 0);
	for (std::size_t index = 0; index < 6; ++index) {
		frame[index] = 0xff;
		frame[6 + index] = guest_mac[index];
		frame[22 + index] = guest_mac[index];
	}
	put16(frame, 12, 0x0806);
	put16(frame, 14, 1);
	put16(frame, 16, 0x0800);
	frame[18] = 6;
	frame[19] = 4;
	put16(frame, 20, 1);
	std::copy(guest_ip.begin(), guest_ip.end(), frame.begin() + 28);
	std::copy(gateway.begin(), gateway.end(), frame.begin() + 38);
	return frame;
}

Bytes dhcp_discover()
{
	Bytes bootp(240, 0);
	bootp[0] = 1;
	bootp[1] = 1;
	bootp[2] = 6;
	put32(bootp, 4, 0x1badcafe);
	put16(bootp, 10, 0x8000);
	std::copy(guest_mac.begin(), guest_mac.end(), bootp.begin() + 28);
	put32(bootp, 236, 0x63825363U);
	bootp.insert(bootp.end(), {53, 1, 1, 255});
	return bootp;
}

Bytes dns_query(std::uint16_t id)
{
	Bytes message(12, 0);
	put16(message, 0, id);
	put16(message, 2, 0x0100);
	put16(message, 4, 1);
	for (const char *label : {"localhost"}) {
		message.push_back(static_cast<std::uint8_t>(std::strlen(label)));
		message.insert(message.end(), label, label + std::strlen(label));
	}
	message.push_back(0);
	message.insert(message.end(), {0, 1, 0, 1});
	return message;
}

/* What the guest sees come out of the provider. */
class Guest final {
public:
	explicit Guest(uml::network::SlirpProvider &provider)
		: provider_(provider)
	{
	}

	bool send(const Bytes &frame)
	{
		return provider_.write({reinterpret_cast<const std::byte *>(
						frame.data()),
					frame.size()}) == 0;
	}

	template <class Accept>
	std::optional<Bytes> receive(Accept accept, int timeout_ms = prompt_ms)
	{
		const auto deadline =
			Clock::now() + std::chrono::milliseconds{timeout_ms};
		std::array<std::byte, 2048> buffer{};
		while (Clock::now() < deadline) {
			const int length = provider_.read(buffer);
			if (length <= 0) {
				pollfd descriptor{provider_.irq_fd(), POLLIN, 0};
				(void)::poll(&descriptor, 1, 10);
				continue;
			}
			Bytes frame(
				reinterpret_cast<const std::uint8_t *>(buffer.data()),
				reinterpret_cast<const std::uint8_t *>(buffer.data()) +
					length);
			if (accept(frame))
				return frame;
		}
		return std::nullopt;
	}

private:
	uml::network::SlirpProvider &provider_;
};

/* An ICMP destination unreachable from `source` that quotes a packet of
 * `protocol` sent to `port`. */
struct Unreachable final {
	Address source{};
	std::uint8_t code{};
	std::uint16_t quoted_port{};
};

std::optional<Unreachable> parse_unreachable(const Bytes &frame,
					     std::uint8_t protocol)
{
	if (frame.size() < 14 + 20 + 8 + 28 || get16(frame, 12) != 0x0800 ||
	    frame[23] != IPPROTO_ICMP || frame[34] != 3)
		return std::nullopt;
	const std::size_t quoted = 42;
	if (frame[quoted + 9] != protocol)
		return std::nullopt;
	Unreachable result;
	std::copy_n(frame.begin() + 26, 4, result.source.begin());
	result.code = frame[35];
	result.quoted_port = get16(frame, quoted + 20 + 2);
	return result;
}

/* A listening TCP socket and a UDP socket the guest's traffic must not reach. */
class HostSockets final {
public:
	explicit HostSockets(const Address &address)
	{
		sockaddr_in where{};
		where.sin_family = AF_INET;
		where.sin_addr.s_addr = htonl(value(address));
		tcp_ = ::socket(AF_INET, SOCK_STREAM, 0);
		udp_ = ::socket(AF_INET, SOCK_DGRAM, 0);
		if (tcp_ < 0 || udp_ < 0)
			return;
		socklen_t length = sizeof(where);
		if (::bind(tcp_, reinterpret_cast<sockaddr *>(&where),
			   sizeof(where)) != 0 ||
		    ::listen(tcp_, 4) != 0 ||
		    ::getsockname(tcp_, reinterpret_cast<sockaddr *>(&where),
				  &length) != 0)
			return;
		tcp_port_ = ntohs(where.sin_port);
		where.sin_port = 0;
		length = sizeof(where);
		if (::bind(udp_, reinterpret_cast<sockaddr *>(&where),
			   sizeof(where)) != 0 ||
		    ::getsockname(udp_, reinterpret_cast<sockaddr *>(&where),
				  &length) != 0)
			return;
		udp_port_ = ntohs(where.sin_port);
	}

	~HostSockets()
	{
		if (tcp_ >= 0)
			::close(tcp_);
		if (udp_ >= 0)
			::close(udp_);
	}

	HostSockets(const HostSockets &) = delete;
	HostSockets &operator=(const HostSockets &) = delete;

	[[nodiscard]] bool ready() const noexcept
	{
		return tcp_port_ != 0 && udp_port_ != 0;
	}
	[[nodiscard]] std::uint16_t tcp_port() const noexcept { return tcp_port_; }
	[[nodiscard]] std::uint16_t udp_port() const noexcept { return udp_port_; }

	/* Whether a connection arrived within `wait_ms`. */
	[[nodiscard]] bool saw_connection(int wait_ms = quiet_ms) const
	{
		pollfd descriptor{tcp_, POLLIN, 0};
		return ::poll(&descriptor, 1, wait_ms) > 0;
	}

	/* Whether a datagram arrived within `wait_ms`. */
	[[nodiscard]] bool saw_datagram(int wait_ms = quiet_ms) const
	{
		pollfd descriptor{udp_, POLLIN, 0};
		return ::poll(&descriptor, 1, wait_ms) > 0;
	}

private:
	int tcp_{-1};
	int udp_{-1};
	std::uint16_t tcp_port_{};
	std::uint16_t udp_port_{};
};

/* A provider on the local-only resolver, opened, with the guest's address
 * known to SLiRP. */
struct Rig final {
	uml::network::SlirpProvider provider;
	Guest guest{provider};
	bool opened{};

	explicit Rig(std::function<void(uml::network::SlirpProvider &)> before = {})
	{
		std::array<unsigned char, 6> mac{};
		unsigned int mtu = 0;
		provider.set_dns_interface_for_testing(
			kDNSServiceInterfaceIndexLocalOnly);
		if (before)
			before(provider);
		opened = provider.open(mac, mtu) == 0;
		if (!opened) {
			fail("open", "the provider did not open");
			return;
		}
		(void)guest.send(arp_request());
		(void)guest.receive([](const Bytes &frame) {
			return frame.size() >= 14 && get16(frame, 12) == 0x0806;
		});
	}

	~Rig()
	{
		if (opened)
			provider.stop();
	}

	Rig(const Rig &) = delete;
	Rig &operator=(const Rig &) = delete;

	/* A SYN from the guest to `destination`:`port`. */
	void syn(const Address &destination, std::uint16_t port,
		 std::uint16_t source_port)
	{
		(void)guest.send(tcp_frame(guest_ip, destination, source_port, port,
					   1000, 0, tcp_syn, 65535));
	}

	/* The answer to that SYN: a reset, or an ICMP unreachable. */
	struct Refusal final {
		bool reset{};
		std::optional<Unreachable> unreachable;
	};

	std::optional<Refusal> refusal_of_syn(std::uint16_t port,
					      std::uint16_t source_port)
	{
		std::optional<Refusal> result;
		(void)guest.receive([&](const Bytes &frame) {
			if (const auto segment = parse_tcp(frame);
			    segment && segment->source_port == port &&
			    segment->destination_port == source_port &&
			    (segment->flags & tcp_rst) != 0) {
				result = Refusal{true, std::nullopt};
				return true;
			}
			if (const auto error = parse_unreachable(frame, IPPROTO_TCP);
			    error && error->quoted_port == port) {
				result = Refusal{false, error};
				return true;
			}
			return false;
		});
		return result;
	}

	void datagram(const Address &destination, std::uint16_t port,
		      std::uint16_t source_port)
	{
		(void)guest.send(udp_frame(guest_ip, destination, source_port, port,
					   {'p', 'r', 'o', 'b', 'e'}));
	}

	std::optional<Unreachable> refusal_of_datagram(std::uint16_t port)
	{
		std::optional<Unreachable> result;
		(void)guest.receive([&](const Bytes &frame) {
			const auto error = parse_unreachable(frame, IPPROTO_UDP);
			if (!error || error->quoted_port != port)
				return false;
			result = error;
			return true;
		});
		return result;
	}
};

/* The interface address the host's traffic leaves by, and its broadcast
 * address: the addresses a service bound to the LAN is reached at. */
struct Lan final {
	Address address{};
	std::optional<Address> broadcast;
};

std::optional<Lan> find_lan()
{
	ifaddrs *list = nullptr;
	if (::getifaddrs(&list) != 0)
		return std::nullopt;
	std::optional<Lan> found;
	for (const ifaddrs *entry = list; entry != nullptr && !found;
	     entry = entry->ifa_next) {
		if (entry->ifa_addr == nullptr ||
		    entry->ifa_addr->sa_family != AF_INET ||
		    (entry->ifa_flags & (IFF_UP | IFF_RUNNING)) !=
			    (IFF_UP | IFF_RUNNING) ||
		    (entry->ifa_flags & IFF_LOOPBACK) != 0)
			continue;
		const auto bytes = [](const sockaddr *address) {
			const auto raw = ntohl(reinterpret_cast<const sockaddr_in *>(
							       address)
						       ->sin_addr.s_addr);
			return Address{static_cast<std::uint8_t>(raw >> 24),
				       static_cast<std::uint8_t>(raw >> 16),
				       static_cast<std::uint8_t>(raw >> 8),
				       static_cast<std::uint8_t>(raw)};
		};
		Lan lan;
		lan.address = bytes(entry->ifa_addr);
		/* Link-local addresses are not a LAN the host serves. */
		if (lan.address[0] == 169 && lan.address[1] == 254)
			continue;
		if ((entry->ifa_flags & IFF_BROADCAST) != 0 &&
		    entry->ifa_dstaddr != nullptr)
			lan.broadcast = bytes(entry->ifa_dstaddr);
		found = lan;
	}
	::freeifaddrs(list);
	return found;
}

/* A flow to `destination` is refused at once, with a reset (TCP) or a port
 * unreachable (UDP), and no host socket hears of it. */
void expect_refused(Rig &rig, const char *label, const Address &destination,
		    const HostSockets &sockets, bool via_slirp,
		    std::uint16_t &next_source_port)
{
	const std::string name = std::string{label} + " " + text(destination);
	const std::uint16_t tcp_source = next_source_port++;
	rig.syn(destination, sockets.tcp_port(), tcp_source);
	if (const auto refusal =
		    rig.refusal_of_syn(sockets.tcp_port(), tcp_source);
	    !refusal) {
		fail(name.c_str(), "TCP: no refusal reached the guest");
	} else if (via_slirp) {
		/* SLiRP's own refusal of the gateway and the broadcast address. */
		if (refusal->reset || refusal->unreachable->code > 1)
			fail(name.c_str(), "TCP: not an ICMP net/host unreachable");
	} else if (!refusal->reset) {
		fail(name.c_str(), "TCP: not a reset");
	}
	if (sockets.saw_connection())
		fail(name.c_str(), "TCP: a host listener took a connection");

	rig.datagram(destination, sockets.udp_port(), next_source_port++);
	if (const auto error = rig.refusal_of_datagram(sockets.udp_port());
	    !error) {
		fail(name.c_str(), "UDP: no refusal reached the guest");
	} else if (!via_slirp && error->code != 3) {
		fail(name.c_str(), "UDP: not a port unreachable");
	} else if (via_slirp && error->code > 1) {
		fail(name.c_str(), "UDP: not an ICMP net/host unreachable");
	}
	if (sockets.saw_datagram())
		fail(name.c_str(), "UDP: a host socket received a datagram");
}

/* The gateway address and the broadcast address, which SLiRP used to map to
 * the host's loopback, and 127.0.0.1 itself, reach nothing. */
void check_loopback_is_closed()
{
	HostSockets sockets{loopback};
	if (!sockets.ready()) {
		fail("loopback sockets", "the probe could not listen");
		return;
	}
	Rig rig;
	if (!rig.opened)
		return;
	std::uint16_t source_port = 40000;
	expect_refused(rig, "the gateway", gateway, sockets, true, source_port);
	expect_refused(rig, "the broadcast address", broadcast, sockets, true,
		       source_port);
	expect_refused(rig, "loopback", loopback, sockets, false, source_port);
}

/* The host's own LAN address, and its broadcast address, reach nothing, while
 * a service bound to them is there to be reached. */
void check_host_addresses_are_closed()
{
	const auto lan = find_lan();
	if (!lan) {
		std::puts("no LAN address on this host: its own addresses are "
			  "checked against the injected table only");
		return;
	}
	HostSockets sockets{lan->address};
	if (!sockets.ready()) {
		fail("LAN sockets", "the probe could not listen");
		return;
	}
	Rig rig;
	if (!rig.opened)
		return;
	std::uint16_t source_port = 41000;
	expect_refused(rig, "the host's LAN address", lan->address, sockets,
		       false, source_port);
	if (lan->broadcast) {
		/* A socket bound to the LAN address does not receive a broadcast,
		 * but the refusal is the same wherever a service listens. */
		rig.datagram(*lan->broadcast, sockets.udp_port(), source_port++);
		const auto error = rig.refusal_of_datagram(sockets.udp_port());
		if (!error || error->code != 3)
			fail("the LAN broadcast address",
			     "UDP: no port unreachable reached the guest");
	}
}

/* What the guest is meant to reach, with the loopback closed. */
void check_what_still_works()
{
	Rig rig;
	if (!rig.opened)
		return;

	/* DHCP: an offer. */
	(void)rig.guest.send(udp_frame({0, 0, 0, 0}, broadcast, 68, 67,
				       dhcp_discover()));
	if (!rig.guest.receive([](const Bytes &frame) {
		    return frame.size() > 42 + 240 && frame[23] == IPPROTO_UDP &&
			    get16(frame, 34) == 67 && get16(frame, 36) == 68;
	    }))
		fail("DHCP", "no offer");

	/* DNS over UDP at the name server and at the gateway. */
	for (const auto &[server, port] :
	     {std::pair<Address, std::uint16_t>{name_server, 43000},
	      std::pair<Address, std::uint16_t>{gateway, 43001}}) {
		(void)rig.guest.send(udp_frame(guest_ip, server, port, 53,
					       dns_query(port)));
		if (!rig.guest.receive([&](const Bytes &frame) {
			    return frame.size() > 46 && frame[23] == IPPROTO_UDP &&
				    get16(frame, 34) == 53 &&
				    get16(frame, 36) == port &&
				    (frame[45] & 0x0f) == 0;
		    }))
			fail("DNS over UDP", "no answer from " + text(server));
	}

	/* DNS over TCP, through SLiRP's guest forward. */
	for (const auto &[server, port] :
	     {std::pair<Address, std::uint16_t>{name_server, 43002},
	      std::pair<Address, std::uint16_t>{gateway, 43003}}) {
		rig.syn(server, 53, port);
		const auto answer = rig.guest.receive([&](const Bytes &frame) {
			const auto segment = parse_tcp(frame);
			return segment && segment->source_port == 53 &&
				segment->destination_port == port;
		});
		if (!answer ||
		    (parse_tcp(*answer)->flags & (tcp_syn | tcp_ack)) !=
			    (tcp_syn | tcp_ack))
			fail("DNS over TCP", "no SYN-ACK from " + text(server));
	}

	/* An echo to the gateway is answered by SLiRP itself. */
	(void)rig.guest.send(icmp_echo_frame(gateway));
	if (!rig.guest.receive([](const Bytes &frame) {
		    return frame.size() >= 42 && frame[23] == IPPROTO_ICMP &&
			    frame[34] == 0;
	    }))
		fail("ping", "no echo reply from the gateway");
}

/* A host forward still delivers a host connection to the guest. */
void check_inbound_forward()
{
	/* A port the host picks, free again before the forward claims it. */
	std::uint16_t host_port = 0;
	{
		HostSockets reserve{loopback};
		host_port = reserve.tcp_port();
	}
	if (host_port == 0) {
		fail("forward", "no free loopback port");
		return;
	}
	constexpr std::uint16_t guest_port = 2222;
	Rig rig{[&](uml::network::SlirpProvider &provider) {
		uml::network::HostForward forward{};
		forward.host_port = host_port;
		forward.guest_port = guest_port;
		if (provider.add_forward(forward) != 0)
			fail("forward", "the forward was refused");
	}};
	if (!rig.opened)
		return;
	const int client = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in where{};
	where.sin_family = AF_INET;
	where.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	where.sin_port = htons(host_port);
	(void)::fcntl(client, F_SETFL, ::fcntl(client, F_GETFL) | O_NONBLOCK);
	(void)::connect(client, reinterpret_cast<sockaddr *>(&where),
			sizeof(where));
	const auto arrived = rig.guest.receive([&](const Bytes &frame) {
		const auto segment = parse_tcp(frame);
		return segment && segment->destination_port == guest_port &&
			(segment->flags & tcp_syn) != 0;
	});
	if (!arrived)
		fail("forward", "the host's connection did not reach the guest");
	::close(client);
}

/* A destination that is neither the host nor the virtual network goes to
 * SLiRP, which Darwin refuses for port 0 before it sends anything and leaves
 * the SYN unanswered. */
void check_other_machines_are_reached()
{
	Rig rig;
	if (!rig.opened)
		return;
	for (int attempt = 0; attempt < 3; ++attempt)
		rig.syn(elsewhere, 0, 44000);
	if (rig.guest.receive([](const Bytes &frame) {
		    const auto segment = parse_tcp(frame);
		    return segment && segment->destination_port == 44000;
	    }, 300))
		fail("another machine", "the provider answered the SYN itself");
	const auto counts = rig.provider.counters();
	if (counts.tcp_attempts != 1 || counts.tcp_unanswered() != 1)
		fail("another machine", "SLiRP was not handed the SYN");
}

/* A host client that connects through one of the host's own addresses to a
 * forward bound to every address is served: the guest's SYN-ACK and its data
 * go to that address, and the provider passes them to SLiRP instead of
 * resetting them as connect attempts to the host. */
void check_a_forward_is_reached_through_the_hosts_own_address()
{
	const auto lan = find_lan();
	if (!lan) {
		std::puts("no LAN address on this host: a forward is not tried "
			  "through its own address");
		return;
	}
	std::uint16_t host_port = 0;
	{
		HostSockets reserve{lan->address};
		host_port = reserve.tcp_port();
	}
	if (host_port == 0) {
		fail("own-address forward", "no free port");
		return;
	}
	constexpr std::uint16_t guest_port = 2223;
	Rig rig{[&](uml::network::SlirpProvider &provider) {
		uml::network::HostForward forward{};
		forward.host_address = "0.0.0.0";
		forward.host_port = host_port;
		forward.guest_port = guest_port;
		if (provider.add_forward(forward) != 0)
			fail("own-address forward", "the forward was refused");
	}};
	if (!rig.opened)
		return;
	const int client = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in where{};
	where.sin_family = AF_INET;
	where.sin_addr.s_addr = htonl(value(lan->address));
	where.sin_port = htons(host_port);
	(void)::fcntl(client, F_SETFL, ::fcntl(client, F_GETFL) | O_NONBLOCK);
	(void)::connect(client, reinterpret_cast<sockaddr *>(&where),
			sizeof(where));
	const auto finish = [&](const char *message) {
		fail("own-address forward", message);
		::close(client);
	};

	std::optional<Segment> syn;
	if (!rig.guest.receive([&](const Bytes &frame) {
		    const auto segment = parse_tcp(frame);
		    if (!segment || segment->destination_port != guest_port ||
			(segment->flags & tcp_syn) == 0)
			    return false;
		    syn = segment;
		    return true;
	    })) {
		finish("the host's connection did not reach the guest");
		return;
	}
	/* The guest answers the SYN-ACK to the address the SYN came from, which
	 * is an address of the host. */
	constexpr std::uint32_t guest_isn = 5000;
	(void)rig.guest.send(tcp_frame(guest_ip, syn->source, guest_port,
				       syn->source_port, guest_isn, syn->seq + 1,
				       tcp_syn | tcp_ack, 65535));
	pollfd connected{client, POLLOUT, 0};
	int error = -1;
	socklen_t length = sizeof(error);
	if (::poll(&connected, 1, 3000) <= 0 ||
	    ::getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &length) != 0 ||
	    error != 0) {
		finish("the host's client could not connect");
		return;
	}

	/* Bytes go both ways. */
	constexpr std::size_t to_guest = 5;
	constexpr std::size_t to_host = 7;
	const char greeting[to_guest] = {'h', 'e', 'l', 'l', 'o'};
	if (::send(client, greeting, sizeof(greeting), 0) !=
	    static_cast<ssize_t>(to_guest)) {
		finish("the host could not send");
		return;
	}
	std::optional<Segment> data;
	if (!rig.guest.receive([&](const Bytes &frame) {
		    const auto segment = parse_tcp(frame);
		    if (!segment || segment->destination_port != guest_port ||
			segment->payload != to_guest)
			    return false;
		    data = segment;
		    return true;
	    })) {
		finish("the host's bytes did not reach the guest");
		return;
	}
	(void)rig.guest.send(tcp_frame(guest_ip, syn->source, guest_port,
				       syn->source_port, guest_isn + 1,
				       data->seq + to_guest, tcp_ack | tcp_psh,
				       65535, to_host));
	pollfd readable{client, POLLIN, 0};
	char reply[16];
	if (::poll(&readable, 1, 3000) <= 0 ||
	    ::recv(client, reply, sizeof(reply), 0) !=
		    static_cast<ssize_t>(to_host))
		fail("own-address forward", "the guest's bytes did not reach the host");
	::close(client);
}

/* The provider refuses what the table names, looks the table up again for a
 * frame that is judged by it once the table is a second old, and does not
 * otherwise: not when the guest is quiet, not for the virtual network, not
 * for a segment of a flow, not for a burst inside the second. */
void check_the_table_is_read_for_the_frames_that_need_it()
{
	using Nanoseconds = std::chrono::nanoseconds;
	struct State final {
		std::mutex mutex;
		std::vector<std::uint32_t> addresses{value(loopback)};
		bool fail{};
		std::atomic<std::int64_t> now{1'000'000'000'000};
	};
	auto state = std::make_shared<State>();
	const auto set_addresses = [&](std::vector<std::uint32_t> addresses) {
		std::lock_guard lock{state->mutex};
		state->addresses = std::move(addresses);
	};
	const auto set_failing = [&](bool failing) {
		std::lock_guard lock{state->mutex};
		state->fail = failing;
	};
	const auto advance = [&](std::chrono::milliseconds by) {
		state->now.fetch_add(
			std::chrono::duration_cast<Nanoseconds>(by).count());
	};
	Rig rig{[&](uml::network::SlirpProvider &provider) {
		provider.set_host_address_reader_for_testing([state] {
			std::lock_guard lock{state->mutex};
			if (state->fail)
				return std::optional<HostAddresses>{};
			HostAddresses addresses;
			addresses.assign_ipv4(state->addresses);
			return std::optional<HostAddresses>{addresses};
		});
		provider.set_clock_for_testing([state] {
			return Clock::time_point{Nanoseconds{state->now.load()}};
		});
	}};
	if (!rig.opened)
		return;
	const auto reads = [&] {
		return rig.provider.host_address_reads_for_testing();
	};
	/* All the frames the guest sent so far have been looked at: a DNS query
	 * is answered after them, in order. */
	std::uint16_t marker = 47000;
	const auto drained = [&] {
		const std::uint16_t id = marker++;
		(void)rig.guest.send(
			udp_frame(guest_ip, name_server, id, 53, dns_query(id)));
		return rig.guest
			.receive([&](const Bytes &frame) {
				return frame.size() > 46 && frame[23] == IPPROTO_UDP &&
					get16(frame, 34) == 53 &&
					get16(frame, 36) == id;
			})
			.has_value();
	};
	const auto refused = [&](std::uint16_t port) {
		return rig.guest
			.receive(
				[&](const Bytes &frame) {
					const auto segment = parse_tcp(frame);
					return segment && segment->source_port == 0 &&
						segment->destination_port == port &&
						(segment->flags & tcp_rst) != 0;
				},
				400)
			.has_value();
	};
	const auto expect_reads = [&](const char *what, std::uint64_t expected) {
		if (!drained())
			fail(what, "the marker query was not answered");
		if (reads() != expected)
			fail(what, "expected " + std::to_string(expected) +
					   " reads of the host's addresses, found " +
					   std::to_string(reads()));
	};

	/* open() reads once, because it cannot come up without the table. A
	 * quiet guest causes no read, however long it is quiet. */
	expect_reads("open", 1);
	advance(std::chrono::seconds{30});
	(void)rig.guest.receive([](const Bytes &) { return false; }, 300);
	expect_reads("a quiet guest", 1);

	/* Frames the table is not asked about cause none either, though the
	 * table is old: the virtual network, a segment of a flow, an echo. */
	rig.syn(gateway, 53, 47100);
	(void)rig.guest.send(tcp_frame(guest_ip, elsewhere, 47101, 80, 7, 8,
				       tcp_ack, 65535));
	(void)rig.guest.send(icmp_echo_frame(gateway));
	expect_reads("frames the table does not judge", 1);

	/* The first frame that needs the table finds it old and reads it, and is
	 * judged by what it read: another machine's address is not the host's. */
	rig.syn(elsewhere, 0, 47200);
	if (refused(47200))
		fail("a connect attempt", "an address that is not the host's "
					  "was refused");
	expect_reads("a connect attempt after a quiet period", 2);

	/* A burst inside the second reads nothing more. */
	for (std::uint16_t port = 47210; port < 47310; ++port)
		rig.syn(elsewhere, 0, port);
	expect_reads("a burst", 2);

	/* The host gains the address. Nothing has asked yet, and inside the
	 * second the old table still stands. */
	set_addresses({value(loopback), value(elsewhere)});
	advance(std::chrono::milliseconds{400});
	rig.syn(elsewhere, 0, 47400);
	if (refused(47400))
		fail("inside the second", "the table changed before it was "
					  "read again");
	expect_reads("inside the second", 2);
	/* Past it, the next frame that needs the table reads the new one. */
	advance(std::chrono::milliseconds{700});
	rig.syn(elsewhere, 0, 47401);
	if (!refused(47401))
		fail("past the second", "the host's new address was not refused");
	expect_reads("past the second", 3);

	/* A read that fails keeps the table, and is not repeated for every
	 * frame: the next try waits for the second as well. */
	set_failing(true);
	advance(std::chrono::milliseconds{1100});
	rig.syn(elsewhere, 0, 47500);
	if (!refused(47500))
		fail("a failing read", "the table was lost with the failed read");
	expect_reads("a failing read", 4);
	for (std::uint16_t port = 47510; port < 47520; ++port)
		rig.syn(elsewhere, 0, port);
	expect_reads("a failing read, inside the second", 4);
	/* It succeeds the next time it is tried, and the host has lost the
	 * address meanwhile. */
	set_failing(false);
	set_addresses({value(loopback)});
	advance(std::chrono::milliseconds{1100});
	rig.syn(elsewhere, 0, 47600);
	if (refused(47600))
		fail("a retried read", "an address the host gave up was still "
				       "refused");
	expect_reads("a retried read", 5);
}

/* A network that cannot list the host's addresses does not come up: the
 * guest would otherwise reach the host. */
void check_the_network_needs_the_table()
{
	uml::network::SlirpProvider provider;
	provider.set_host_address_reader_for_testing(
		[] { return std::optional<HostAddresses>{}; });
	std::array<unsigned char, 6> mac{};
	unsigned int mtu = 0;
	if (provider.open(mac, mtu) == 0) {
		fail("no table", "the provider opened without the host's addresses");
		provider.stop();
	}
}

} // namespace

int main(void)
{
	check_loopback_is_closed();
	check_host_addresses_are_closed();
	check_what_still_works();
	check_inbound_forward();
	check_a_forward_is_reached_through_the_hosts_own_address();
	check_other_machines_are_reached();
	check_the_table_is_read_for_the_frames_that_need_it();
	check_the_network_needs_the_table();
	if (failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	std::puts("the guest reaches nothing of the host");
	return 0;
}
