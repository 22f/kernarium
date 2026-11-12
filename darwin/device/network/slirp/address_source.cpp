// SPDX-License-Identifier: GPL-2.0-or-later
#include "address_source.hpp"
#include "host_addresses.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>

namespace uml::network {

bool AddressChangeSource::open() noexcept
{
	socket_.reset(::socket(PF_ROUTE, SOCK_RAW, 0));
	if (socket_.get() < 0)
		return false;
	using uml::platform::configureDescriptor;
	if (configureDescriptor(socket_.get(),
				{.closeOnExec = true, .nonblocking = true}))
		return true;
	socket_.reset();
	return false;
}

bool AddressChangeSource::drain() noexcept
{
	bool changed = false;
	std::array<std::uint8_t, 2048> message{};
	for (;;) {
		const auto length = ::read(socket_.get(), message.data(),
					   message.size());
		if (length < 0 && errno == EINTR)
			continue;
		if (length < 0 && errno == EAGAIN)
			return changed;
		/* A socket that fails is not polled again, which would spin on it;
		 * the provider falls back to reading on a timer. */
		if (length <= 0) {
			socket_.reset();
			return true;
		}
		changed = is_address_change(
			{message.data(), static_cast<std::size_t>(length)}) ||
			changed;
	}
}

} // namespace uml::network
