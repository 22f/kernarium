// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace uml::platform {

/*
 * Whether the host has a network path, as the system last said. A reader on
 * any thread asks at the moment it needs the answer; a source of reports
 * (the monitor below, or a test) writes it.
 *
 * A state no one has reported to is usable: a monitor that has not spoken yet
 * must not turn a working network into an outage.
 */
class HostNetworkPathState final {
public:
	/* False only while the last report said the host has no path at all. A
	 * path that needs a connection first (an on-demand VPN) is usable: the
	 * attempt itself brings it up. */
	[[nodiscard]] bool usable() const noexcept
	{
		return usable_.load(std::memory_order_acquire);
	}
	/* Every report sets the state, so a path that is back clears the
	 * outage. */
	void report(bool usable) noexcept
	{
		usable_.store(usable, std::memory_order_release);
		reports_.fetch_add(1, std::memory_order_release);
	}
	/* Reports so far; a source that has reported once has said what the
	 * system's path is. */
	[[nodiscard]] std::uint64_t reports() const noexcept
	{
		return reports_.load(std::memory_order_acquire);
	}

private:
	std::atomic<bool> usable_{true};
	std::atomic<std::uint64_t> reports_{0};
};

/* The system's path monitor, reporting into a state while it lives. It is
 * event driven: the system reports the current path once at the start and
 * then each change, from a thread of its own, and nothing else wakes the
 * process. It opens no connection. The monitor makes its own thread for the
 * system's start-up, so that no Mach port of the process is tied to the
 * thread that asked for it; and it is meant to live as long as the process
 * that wants the state does (a guest's restart ends the thread that booted
 * it, and a monitor made again after it would not leave the process as the
 * first one did). */
class HostNetworkPathMonitor {
public:
	virtual ~HostNetworkPathMonitor() = default;
};

/* Starts a monitor that reports into `state`, which must outlive it. Once the
 * monitor's destructor returns, no report is running or will start. A system
 * that gives no monitor leaves the state as it is. Throws std::bad_alloc when
 * the monitor's own storage cannot be had; the caller cleans up as for any
 * other resource it could not get. */
[[nodiscard]] std::unique_ptr<HostNetworkPathMonitor>
makeAppleHostNetworkPathMonitor(HostNetworkPathState &state);

} // namespace uml::platform
