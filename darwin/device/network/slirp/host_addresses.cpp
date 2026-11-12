// SPDX-License-Identifier: GPL-2.0-or-later
#include "host_addresses.hpp"

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <utility>

namespace uml::network {
namespace {

/* The types of the routing socket's messages (<net/route.h>), written out
 * because the header is private on some Apple SDKs. */
constexpr std::uint8_t rtm_newaddr = 0x0c;
constexpr std::uint8_t rtm_deladdr = 0x0d;
constexpr std::uint8_t rtm_ifinfo = 0x0e;
constexpr std::uint8_t rtm_ifannounce = 0x11;
constexpr std::uint8_t rtm_ifinfo2 = 0x12;
/* rtm_msglen (2 bytes), rtm_version, rtm_type. */
constexpr std::size_t rtm_type_offset = 3;

constexpr std::uint32_t ipv4_value(const HostIpv4 &address) noexcept
{
	return (static_cast<std::uint32_t>(address[0]) << 24) |
		(static_cast<std::uint32_t>(address[1]) << 16) |
		(static_cast<std::uint32_t>(address[2]) << 8) | address[3];
}

} // namespace

void HostAddresses::assign_ipv4(std::vector<std::uint32_t> addresses)
{
	/* The unspecified address and the limited broadcast address are no
	 * interface's: the guest's DHCP goes to the second, and SLiRP answers
	 * for it. */
	std::erase_if(addresses, [](std::uint32_t address) {
		return address == 0 || address == 0xffffffffU;
	});
	std::ranges::sort(addresses);
	addresses.erase(std::ranges::unique(addresses).begin(), addresses.end());
	ipv4_ = std::move(addresses);
}

bool HostAddresses::contains(const HostIpv4 &address) const noexcept
{
	return std::ranges::binary_search(ipv4_, ipv4_value(address));
}

std::optional<HostAddresses> read_host_addresses()
{
	ifaddrs *list = nullptr;
	if (::getifaddrs(&list) != 0)
		return std::nullopt;
	std::vector<std::uint32_t> ipv4;
	const auto add = [&](const sockaddr *address) {
		if (address == nullptr || address->sa_family != AF_INET)
			return;
		ipv4.push_back(ntohl(
			reinterpret_cast<const sockaddr_in *>(address)->sin_addr
				.s_addr));
	};
	for (const ifaddrs *entry = list; entry != nullptr;
	     entry = entry->ifa_next) {
		/* The family switch: an IPv6 address joins its own table here. */
		add(entry->ifa_addr);
		/* ifa_dstaddr is the broadcast address on a broadcast
		 * interface and the far end of a point-to-point link, which
		 * is not the host's. */
		if ((entry->ifa_flags & IFF_BROADCAST) != 0)
			add(entry->ifa_dstaddr);
	}
	::freeifaddrs(list);
	HostAddresses result;
	result.assign_ipv4(std::move(ipv4));
	return result;
}

bool is_address_change(std::span<const std::uint8_t> message) noexcept
{
	if (message.size() <= rtm_type_offset)
		return false;
	switch (message[rtm_type_offset]) {
	case rtm_newaddr:
	case rtm_deladdr:
	case rtm_ifinfo:
	case rtm_ifannounce:
	case rtm_ifinfo2:
		return true;
	default:
		return false;
	}
}

} // namespace uml::network
