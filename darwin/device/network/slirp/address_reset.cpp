// SPDX-License-Identifier: GPL-2.0-or-later
#include "address_reset.hpp"

#include <netinet/in.h>

namespace uml::network {

bool local_address_is_gone(const sockaddr_storage &local,
			   const HostAddresses &table) noexcept
{
	if (local.ss_family != AF_INET)
		return false;
	const auto address = ntohl(
		reinterpret_cast<const sockaddr_in &>(local).sin_addr.s_addr);
	if (address == INADDR_ANY)
		return false;
	return !table.contains({static_cast<std::uint8_t>(address >> 24),
				static_cast<std::uint8_t>(address >> 16),
				static_cast<std::uint8_t>(address >> 8),
				static_cast<std::uint8_t>(address)});
}

} // namespace uml::network
