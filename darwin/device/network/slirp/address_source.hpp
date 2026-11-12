// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * What tells the provider that the host's addresses changed: a routing socket,
 * which the kernel writes a message to whenever an interface address is added
 * or removed or an interface comes, goes or changes state. It is readable only
 * then, so a quiet network costs the provider no wake-up and no system call.
 * A system that refuses the socket leaves `active()` false, and the provider
 * reads the addresses on a timer instead.
 */

#include "platform/descriptor.hpp"

namespace uml::network {

class AddressChangeSource final {
public:
	/* Opens the socket, non-blocking and close-on-exec; false when the
	 * system refuses it. */
	[[nodiscard]] bool open() noexcept;
	void close() noexcept { socket_.reset(); }

	[[nodiscard]] bool active() const noexcept { return socket_.get() >= 0; }
	/* The descriptor to poll for POLLIN; -1 while closed, which poll skips. */
	[[nodiscard]] int descriptor() const noexcept { return socket_.get(); }

	/* Reads every message waiting; true when any of them reports a change of
	 * an address or an interface. */
	[[nodiscard]] bool drain() noexcept;

private:
	uml::platform::UniqueDescriptor socket_;
};

} // namespace uml::network
