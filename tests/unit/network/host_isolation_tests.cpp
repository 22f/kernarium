// SPDX-License-Identifier: GPL-2.0-or-later
//
// The rule that keeps the guest off the host itself: the table of the host's
// addresses, which guest frames the provider refuses because they address one
// of them and what the refusal looks like on the wire, and which routing
// messages tell the provider to read the table again. Every rule is a pure
// function of bytes and of the table, so these tests need no socket and no
// guest.

#include "dns_frames.hpp"
#include "host_addresses.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <utility>
#include <vector>

namespace {

using uml::testing::require;
namespace dns = uml::network::dns;
using Bytes = std::vector<std::uint8_t>;
using dns::FrameAction;
using dns::Ipv4Address;
using uml::network::HostAddresses;

constexpr Ipv4Address guest{10, 0, 2, 15};
constexpr Ipv4Address lan_host{192, 168, 1, 5};
constexpr Ipv4Address lan_broadcast{192, 168, 1, 255};
constexpr Ipv4Address loopback{127, 0, 0, 1};
constexpr Ipv4Address other_machine{192, 168, 1, 77};
constexpr Ipv4Address gateway{10, 0, 2, 2};
constexpr dns::MacAddress guest_mac{0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
constexpr dns::MacAddress slirp_mac{0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
constexpr dns::Subnet virtual_network{{10, 0, 2, 0}, {255, 255, 255, 0}};

constexpr std::uint8_t protocol_icmp = 1;
constexpr std::uint8_t protocol_tcp = 6;
constexpr std::uint8_t protocol_udp = 17;
constexpr std::uint8_t tcp_syn = 0x02;
constexpr std::uint8_t tcp_rst = 0x04;
constexpr std::uint8_t tcp_ack = 0x10;

std::uint16_t get16(const Bytes &bytes, std::size_t offset)
{
	return static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
}

std::uint32_t get32(const Bytes &bytes, std::size_t offset)
{
	return (static_cast<std::uint32_t>(get16(bytes, offset)) << 16) |
		get16(bytes, offset + 2);
}

void put16(Bytes &bytes, std::size_t offset, std::uint16_t value)
{
	bytes[offset] = static_cast<std::uint8_t>(value >> 8);
	bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void put32(Bytes &bytes, std::size_t offset, std::uint32_t value)
{
	put16(bytes, offset, static_cast<std::uint16_t>(value >> 16));
	put16(bytes, offset + 2, static_cast<std::uint16_t>(value));
}

/* The Internet checksum of `bytes` after `sum`; zero over a valid packet. */
std::uint16_t checksum(const std::uint8_t *bytes, std::size_t size,
		       std::uint32_t sum = 0)
{
	for (std::size_t index = 0; index + 1 < size; index += 2)
		sum += static_cast<std::uint32_t>(bytes[index] << 8) |
			bytes[index + 1];
	if ((size & 1U) != 0)
		sum += static_cast<std::uint32_t>(bytes[size - 1]) << 8;
	while ((sum >> 16) != 0)
		sum = (sum & 0xffff) + (sum >> 16);
	return static_cast<std::uint16_t>(~sum);
}

std::span<const std::uint8_t> view(const Bytes &bytes)
{
	return {bytes.data(), bytes.size()};
}

/* An Ethernet frame from the guest. */
Bytes ip_frame(const Ipv4Address &destination, std::uint8_t protocol,
	       const Bytes &transport, std::uint16_t fragment = 0x4000)
{
	Bytes frame(14 + 20 + transport.size(), 0);
	std::copy(slirp_mac.begin(), slirp_mac.end(), frame.begin());
	std::copy(guest_mac.begin(), guest_mac.end(), frame.begin() + 6);
	put16(frame, 12, 0x0800);
	frame[14] = 0x45;
	put16(frame, 16, static_cast<std::uint16_t>(20 + transport.size()));
	put16(frame, 18, 0x1234);
	put16(frame, 20, fragment);
	frame[22] = 64;
	frame[23] = protocol;
	std::copy(guest.begin(), guest.end(), frame.begin() + 26);
	std::copy(destination.begin(), destination.end(), frame.begin() + 30);
	put16(frame, 24, checksum(&frame[14], 20));
	std::copy(transport.begin(), transport.end(), frame.begin() + 34);
	return frame;
}

Bytes udp_datagram(std::uint16_t source_port, std::uint16_t destination_port,
		   std::size_t payload = 11)
{
	Bytes udp(8 + payload, 0x61);
	put16(udp, 0, source_port);
	put16(udp, 2, destination_port);
	put16(udp, 4, static_cast<std::uint16_t>(udp.size()));
	put16(udp, 6, 0);
	return udp;
}

Bytes tcp_segment(std::uint16_t source_port, std::uint16_t destination_port,
		  std::uint32_t sequence, std::uint32_t acknowledgement,
		  std::uint8_t flags, std::size_t payload = 0)
{
	Bytes tcp(20 + payload, 0);
	put16(tcp, 0, source_port);
	put16(tcp, 2, destination_port);
	put32(tcp, 4, sequence);
	put32(tcp, 8, acknowledgement);
	tcp[12] = 5 << 4;
	tcp[13] = flags;
	put16(tcp, 14, 65535);
	return tcp;
}

HostAddresses host_with(std::vector<std::uint32_t> addresses)
{
	HostAddresses host;
	host.assign_ipv4(std::move(addresses));
	return host;
}

constexpr std::uint32_t value(const Ipv4Address &address)
{
	return (static_cast<std::uint32_t>(address[0]) << 24) |
		(static_cast<std::uint32_t>(address[1]) << 16) |
		(static_cast<std::uint32_t>(address[2]) << 8) | address[3];
}

HostAddresses the_host()
{
	return host_with({value(loopback), value(lan_host), value(lan_broadcast),
			  value(lan_host)});
}

dns::Classified classify(const Bytes &frame, const HostAddresses &host)
{
	return dns::classify_host_bound_frame(view(frame), host, virtual_network);
}

void theTableHoldsEachAddressOnceAndAnswersByLookup()
{
	const auto host = the_host();
	require(host.size() == 3);
	require(host.contains(loopback) && host.contains(lan_host) &&
		host.contains(lan_broadcast));
	require(!host.contains(other_machine) && !host.contains(gateway));
	require(HostAddresses{}.empty() && !HostAddresses{}.contains(lan_host));
	/* Neither of the two addresses SLiRP answers for is an interface's. */
	const auto odd = host_with({0, 0xffffffffU, value(lan_host)});
	require(odd.size() == 1 && !odd.contains({255, 255, 255, 255}) &&
		!odd.contains({0, 0, 0, 0}));
	/* A table assigned again replaces what it held. */
	auto replaced = the_host();
	replaced.assign_ipv4({value(other_machine)});
	require(replaced.contains(other_machine) && !replaced.contains(lan_host));
}

void theSystemListsTheLoopbackAddress()
{
	const auto host = uml::network::read_host_addresses();
	require(host.has_value());
	require(host->contains(loopback));
	require(!host->contains({255, 255, 255, 255}));
}

void aSynToAHostAddressIsReset()
{
	const auto frame = ip_frame(lan_host, protocol_tcp,
		tcp_segment(40000, 5555, 1000, 0, tcp_syn));
	const auto classified = classify(frame, the_host());
	require(classified.action == FrameAction::reset);
	require(classified.endpoints.server_address == lan_host);
	require(classified.endpoints.guest_address == guest);
	require(classified.endpoints.guest_port == 40000 &&
		classified.endpoints.server_port == 5555);
	require(classified.endpoints.guest_mac == guest_mac);
	const auto reset = dns::tcp_reset_frame(classified, 77);
	require(reset.size() == 14 + 20 + 20);
	require(std::equal(guest_mac.begin(), guest_mac.end(), reset.begin()));
	require(std::equal(lan_host.begin(), lan_host.end(), reset.begin() + 26));
	require(std::equal(guest.begin(), guest.end(), reset.begin() + 30));
	require(checksum(&reset[14], 20) == 0);
	require(get16(reset, 34) == 5555 && get16(reset, 36) == 40000);
	/* RFC 9293: a reset to a SYN acknowledges it. */
	require(reset[47] == (tcp_rst | tcp_ack) && get32(reset, 42) == 1001);
}

void everyAddressTheHostHoldsIsRefused()
{
	const auto host = the_host();
	for (const auto &address : {loopback, lan_host, lan_broadcast})
		require(classify(ip_frame(address, protocol_tcp,
					  tcp_segment(40000, 80, 1, 0, tcp_syn)),
				 host).action == FrameAction::reset);
}

void aSegmentOfAConnectionThatIsNotThereIsResetWithItsAcknowledgement()
{
	const auto frame = ip_frame(lan_host, protocol_tcp,
		tcp_segment(40001, 80, 500, 9000, tcp_ack, 100));
	const auto classified = classify(frame, the_host());
	require(classified.action == FrameAction::reset);
	const auto reset = dns::tcp_reset_frame(classified, 1);
	require(reset[47] == tcp_rst && get32(reset, 38) == 9000);
}

void aResetIsNeverAnswered()
{
	const auto frame = ip_frame(lan_host, protocol_tcp,
		tcp_segment(40002, 80, 500, 0, tcp_rst));
	const auto classified = classify(frame, the_host());
	require(classified.action == FrameAction::reset);
	require(dns::tcp_reset_frame(classified, 1).empty());
}

void aDatagramToAHostAddressGetsAPortUnreachable()
{
	const auto frame = ip_frame(lan_host, protocol_udp, udp_datagram(41000, 5353));
	const auto classified = classify(frame, the_host());
	require(classified.action == FrameAction::unreachable);
	require(classified.quote.size() == 28);
	const auto icmp = dns::icmp_unreachable_frame(classified, 9);
	require(icmp.size() == 14 + 20 + 8 + 28);
	require(std::equal(guest_mac.begin(), guest_mac.end(), icmp.begin()));
	require(icmp[23] == protocol_icmp);
	/* From the address the datagram was sent to, back to the guest. */
	require(std::equal(lan_host.begin(), lan_host.end(), icmp.begin() + 26));
	require(std::equal(guest.begin(), guest.end(), icmp.begin() + 30));
	require(checksum(&icmp[14], 20) == 0);
	/* Destination unreachable, port unreachable, a valid checksum. */
	require(icmp[34] == 3 && icmp[35] == 3);
	require(checksum(&icmp[34], icmp.size() - 34) == 0);
	/* The quote is the datagram's IP header and its first eight bytes, which
	 * hold the ports a socket matches the error by. */
	require(std::equal(frame.begin() + 14, frame.begin() + 14 + 28,
			   icmp.begin() + 42));
	require(get16(icmp, 42 + 20) == 41000 && get16(icmp, 42 + 22) == 5353);
}

void whateverElseIsAddressedToTheHostIsDropped()
{
	const auto host = the_host();
	Bytes echo(8, 0);
	echo[0] = 8;
	put16(echo, 2, checksum(echo.data(), echo.size()));
	require(classify(ip_frame(lan_host, protocol_icmp, echo), host).action ==
		FrameAction::drop);
	/* A fragment has no ports to answer by. */
	require(classify(ip_frame(lan_host, protocol_udp, udp_datagram(1, 2),
				  0x2000), host).action == FrameAction::drop);
	require(classify(ip_frame(lan_host, protocol_udp, udp_datagram(1, 2),
				  0x00b9), host).action == FrameAction::drop);
	/* A packet SLiRP would refuse is no reason to pass it on. */
	auto bad_checksum = ip_frame(lan_host, protocol_tcp,
		tcp_segment(1, 2, 3, 4, tcp_syn));
	bad_checksum[24] ^= 0x5a;
	require(classify(bad_checksum, host).action == FrameAction::drop);
	auto cut = ip_frame(lan_host, protocol_tcp,
		tcp_segment(1, 2, 3, 4, tcp_syn));
	cut.resize(cut.size() - 4);
	put16(cut, 16, static_cast<std::uint16_t>(cut.size() - 14));
	put16(cut, 24, 0);
	put16(cut, 24, checksum(&cut[14], 20));
	require(classify(cut, host).action == FrameAction::drop);
	auto short_datagram = ip_frame(lan_host, protocol_udp, Bytes(6, 0));
	require(classify(short_datagram, host).action == FrameAction::drop);
	require(classify(ip_frame(lan_host, 47, Bytes(16, 0)), host).action ==
		FrameAction::drop);
}

void everyOtherAddressGoesToSlirp()
{
	const auto host = the_host();
	require(classify(ip_frame(other_machine, protocol_tcp,
				  tcp_segment(40000, 80, 1, 0, tcp_syn)),
			 host).action == FrameAction::pass);
	require(classify(ip_frame(other_machine, protocol_udp, udp_datagram(1, 2)),
			 host).action == FrameAction::pass);
	/* The two addresses SLiRP answers for are not the host's, and neither
	 * is the broadcast address the guest's DHCP goes to. */
	require(classify(ip_frame({255, 255, 255, 255}, protocol_udp,
				  udp_datagram(68, 67)), host).action ==
		FrameAction::pass);
	/* An empty table refuses nothing. */
	require(classify(ip_frame(lan_host, protocol_tcp,
				  tcp_segment(40000, 80, 1, 0, tcp_syn)),
			 HostAddresses{}).action == FrameAction::pass);
}

void theVirtualNetworkIsNeverTheHost()
{
	/* A host whose own interface sits in 10.0.2.0/24 is no reason to refuse
	 * the guest its gateway and its name server. */
	const auto host = host_with({value(gateway), value({10, 0, 2, 3}),
				     value({10, 0, 2, 77})});
	for (const auto &address : {gateway, Ipv4Address{10, 0, 2, 3},
				    Ipv4Address{10, 0, 2, 77}}) {
		require(classify(ip_frame(address, protocol_tcp,
					  tcp_segment(40000, 80, 1, 0, tcp_syn)),
				 host).action == FrameAction::pass);
		require(classify(ip_frame(address, protocol_udp, udp_datagram(1, 2)),
				 host).action == FrameAction::pass);
	}
}

void framesThatAreNotIpv4AreLeftToSlirp()
{
	Bytes arp(42, 0);
	std::copy(guest_mac.begin(), guest_mac.end(), arp.begin() + 6);
	put16(arp, 12, 0x0806);
	std::copy(lan_host.begin(), lan_host.end(), arp.begin() + 38);
	require(classify(arp, the_host()).action == FrameAction::pass);
	require(classify(Bytes(10, 0), the_host()).action == FrameAction::pass);
}

void aRoutingMessageSaysWhenTheAddressesChange()
{
	/* rtm_msglen, rtm_version, rtm_type: RTM_NEWADDR, RTM_DELADDR,
	 * RTM_IFINFO, RTM_IFANNOUNCE, RTM_IFINFO2. */
	for (const std::uint8_t type : {0x0c, 0x0d, 0x0e, 0x11, 0x12}) {
		const Bytes message{0x20, 0x00, 0x05, type, 0, 0, 0, 0};
		require(uml::network::is_address_change(view(message)));
	}
	/* RTM_ADD, RTM_DELETE, RTM_GET, RTM_MISS: routes, not addresses. */
	for (const std::uint8_t type : {0x01, 0x02, 0x04, 0x05, 0x06, 0x0f}) {
		const Bytes message{0x20, 0x00, 0x05, type, 0, 0, 0, 0};
		require(!uml::network::is_address_change(view(message)));
	}
	require(!uml::network::is_address_change(view(Bytes{0x20, 0x00, 0x05})));
	require(!uml::network::is_address_change({}));
}

} // namespace

int main()
{
	theTableHoldsEachAddressOnceAndAnswersByLookup();
	theSystemListsTheLoopbackAddress();
	aSynToAHostAddressIsReset();
	everyAddressTheHostHoldsIsRefused();
	aSegmentOfAConnectionThatIsNotThereIsResetWithItsAcknowledgement();
	aResetIsNeverAnswered();
	aDatagramToAHostAddressGetsAPortUnreachable();
	whateverElseIsAddressedToTheHostIsDropped();
	everyOtherAddressGoesToSlirp();
	theVirtualNetworkIsNeverTheHost();
	framesThatAreNotIpv4AreLeftToSlirp();
	aRoutingMessageSaysWhenTheAddressesChange();
	return EXIT_SUCCESS;
}
