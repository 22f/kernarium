// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * The addresses of the host's own network interfaces, for the rule that keeps
 * the guest off the host itself. SLiRP connects a guest's flow with a socket
 * of the application, so a guest that addresses one of these addresses would
 * reach the application's own machine, services bound to every address
 * included. The provider refuses those flows (dns_frames.hpp) and reads this
 * table, not the system, for each frame: the table is read again when the
 * routing socket reports a change of an address or an interface.
 *
 * A family is its own sorted table. IPv4 is the only one the guest has; an
 * IPv6 table with its `contains` and its case in `read_host_addresses` adds
 * the second family.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace uml::network {

using HostIpv4 = std::array<std::uint8_t, 4>;

class HostAddresses final {
public:
	/* Replaces the IPv4 table with `addresses` (host byte order), in any
	 * order and with repeats. 0.0.0.0 and 255.255.255.255 are not kept. */
	void assign_ipv4(std::vector<std::uint32_t> addresses);

	/* Whether an interface of the host holds `address` or, for an interface
	 * with a broadcast address, that broadcast address, which a local
	 * socket bound to every address receives as well. */
	[[nodiscard]] bool contains(const HostIpv4 &address) const noexcept;

	[[nodiscard]] bool empty() const noexcept { return ipv4_.empty(); }
	[[nodiscard]] std::size_t size() const noexcept { return ipv4_.size(); }

	[[nodiscard]] bool operator==(const HostAddresses &) const noexcept = default;

private:
	std::vector<std::uint32_t> ipv4_;
};

/* Every address the system lists for an interface now, up or down: a local
 * address is local whatever the state of its interface. Nothing when the
 * system cannot list them. */
[[nodiscard]] std::optional<HostAddresses> read_host_addresses();

/* Whether a message from the routing socket (a `struct rt_msghdr`, whose
 * fifth byte is the type) says that an interface address was added or
 * removed, or that an interface came, went, or changed state. */
[[nodiscard]] bool is_address_change(
	std::span<const std::uint8_t> message) noexcept;

} // namespace uml::network
