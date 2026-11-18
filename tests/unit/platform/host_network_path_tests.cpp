// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The host's network path (platform/host_network_path.hpp). The state starts
 * usable, and every report replaces it, so a path that is back clears an
 * outage. The system's monitor reports the current path once as it starts,
 * whatever the path is, and again at each start; a path change itself cannot
 * be made here, so the report is checked as an event and the stand-in state
 * of the resolver's tests carries the two directions. A monitor that has
 * ended leaves nothing behind that writes to the state.
 */
#include "platform/host_network_path.hpp"
#include "require.hpp"
#include "wait_until.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

namespace {

using uml::platform::HostNetworkPathMonitor;
using uml::platform::HostNetworkPathState;
using uml::testing::require;
using uml::testing::waitUntil;

void a_state_nobody_reported_to_is_usable_and_each_report_replaces_it()
{
	HostNetworkPathState state;
	require(state.usable(), "a state no one reported to is an outage");
	require(state.reports() == 0, "a report was counted before any");

	state.report(false);
	require(!state.usable(), "a report of no path left the path usable");
	state.report(false);
	require(!state.usable(), "a second report of no path cleared the outage");
	state.report(true);
	require(state.usable(), "a report of a path did not clear the outage");
	state.report(false);
	state.report(true);
	require(state.usable() && state.reports() == 5,
		"the transitions were not each counted");
}

void the_monitor_reports_the_path_at_each_start()
{
	HostNetworkPathState state;
	for (std::uint64_t start = 1; start <= 3; ++start) {
		std::unique_ptr<HostNetworkPathMonitor> monitor =
			uml::platform::makeAppleHostNetworkPathMonitor(state);
		require(monitor != nullptr, "no monitor");
		require(waitUntil([&] { return state.reports() >= start; },
				  std::chrono::seconds{10}),
			"the system did not report the path at a start");
	}
}

void a_monitor_that_ended_writes_nothing()
{
	/* Each state ends right after its monitor, before or after the first
	 * report: a report in flight must find a lock and no state. */
	for (int round = 0; round < 200; ++round) {
		auto state = std::make_unique<HostNetworkPathState>();
		auto monitor =
			uml::platform::makeAppleHostNetworkPathMonitor(*state);
		require(monitor != nullptr, "no monitor");
		if (round % 2 == 1)
			(void)waitUntil([&] { return state->reports() >= 1; },
					std::chrono::seconds{10});
		monitor.reset();
		state.reset();
	}

	HostNetworkPathState state;
	{
		auto monitor =
			uml::platform::makeAppleHostNetworkPathMonitor(state);
		require(waitUntil([&] { return state.reports() >= 1; },
				  std::chrono::seconds{10}),
			"the system did not report the path");
	}
	const auto reports = state.reports();
	std::this_thread::sleep_for(std::chrono::milliseconds{200});
	require(state.reports() == reports,
		"a monitor that ended still wrote to the state");
}

} // namespace

int main()
{
	a_state_nobody_reported_to_is_usable_and_each_report_replaces_it();
	the_monitor_reports_the_path_at_each_start();
	a_monitor_that_ended_writes_nothing();
	std::puts("host network path tests passed");
	return 0;
}
