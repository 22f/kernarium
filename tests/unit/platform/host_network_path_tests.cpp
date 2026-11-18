// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The host's network path (platform/host_network_path.hpp). The state starts
 * usable, and every report replaces it, so a path that is back clears an
 * outage. The system's monitor reports the current path once as it starts,
 * whatever the path is, and again at each start; a path change itself cannot
 * be made here, so the report is checked as an event and the stand-in state
 * of the resolver's tests carries the two directions. What the system's
 * status and reason mean as an outage is a function of the two values, checked
 * for each of them. The monitor ties no Mach port of the process to the thread
 * that made it, whichever thread that is: the system's start-up would, and a
 * guest's restart ends the thread that booted it (guest-restart). A monitor
 * that has ended leaves nothing behind that writes to the state.
 */
#include "platform/host_network_path.hpp"
#include "host_network_path_status.hpp"
#include "require.hpp"
#include "wait_until.hpp"

#include <mach/mach.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <thread>

namespace {

using uml::platform::HostNetworkPathMonitor;
using uml::platform::HostNetworkPathState;
using uml::testing::require;
using uml::testing::waitUntil;

/* The rights the process holds that are both send and receive and name no
 * kernel object: what a run loop or a framework's start-up makes for a thread.
 */
std::size_t portsOfThreadKind()
{
	mach_port_name_array_t names = nullptr;
	mach_msg_type_number_t nameCount = 0;
	mach_port_type_array_t types = nullptr;
	mach_msg_type_number_t typeCount = 0;
	require(mach_port_names(mach_task_self(), &names, &nameCount, &types,
				&typeCount) == KERN_SUCCESS,
		"the process's ports cannot be listed");
	std::size_t count = 0;
	for (mach_msg_type_number_t index = 0; index != typeCount; ++index) {
		ipc_info_object_type_t type{};
		mach_vm_address_t address = 0;
		const bool noObject =
			mach_port_kobject(mach_task_self(), names[index], &type,
					  &address) != KERN_SUCCESS ||
			static_cast<int>(type) == -1;
		if (noObject && types[index] ==
				(MACH_PORT_TYPE_SEND | MACH_PORT_TYPE_RECEIVE))
			++count;
	}
	(void)vm_deallocate(mach_task_self(),
			    reinterpret_cast<vm_address_t>(names),
			    nameCount * sizeof(mach_port_name_t));
	(void)vm_deallocate(mach_task_self(),
			    reinterpret_cast<vm_address_t>(types),
			    typeCount * sizeof(mach_port_type_t));
	return count;
}

/* The first monitor of the process, made on a thread that ends while the
 * monitor lives, as a guest's restart ends the thread that booted it: the
 * process holds the same ports after the thread is gone as while it ran. This
 * runs before every other test, because the system's start-up happens once. */
void the_first_monitor_ties_no_port_to_the_thread_that_made_it()
{
	HostNetworkPathState state;
	std::unique_ptr<HostNetworkPathMonitor> monitor;
	std::size_t whileTheThreadRuns = 0;
	std::thread maker{[&] {
		monitor = uml::platform::makeAppleHostNetworkPathMonitor(state);
		require(monitor != nullptr, "no monitor");
		require(waitUntil([&] { return state.reports() >= 1; },
				  std::chrono::seconds{10}),
			"the system did not report the path");
		whileTheThreadRuns = portsOfThreadKind();
	}};
	maker.join();
	std::this_thread::sleep_for(std::chrono::milliseconds{300});
	require(portsOfThreadKind() == whileTheThreadRuns,
		"the process lost a port when the thread that made the monitor "
		"ended");
}

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

void only_an_unsatisfied_path_is_an_outage_and_a_local_network_denial_is_not()
{
	using uml::platform::hostNetworkPathIsOutage;
	const nw_path_unsatisfied_reason_t reasons[] = {
		nw_path_unsatisfied_reason_not_available,
		nw_path_unsatisfied_reason_cellular_denied,
		nw_path_unsatisfied_reason_wifi_denied,
		nw_path_unsatisfied_reason_local_network_denied,
		nw_path_unsatisfied_reason_vpn_inactive};
	for (const auto reason : reasons) {
		/* The reason means something only for an unsatisfied path. */
		require(!hostNetworkPathIsOutage(nw_path_status_satisfied, reason),
			"a satisfied path is an outage");
		require(!hostNetworkPathIsOutage(nw_path_status_satisfiable, reason),
			"a path that needs a connection first is an outage");
		require(!hostNetworkPathIsOutage(nw_path_status_invalid, reason),
			"an invalid path is an outage");
		require(hostNetworkPathIsOutage(nw_path_status_unsatisfied, reason) ==
				(reason != nw_path_unsatisfied_reason_local_network_denied),
			"an unsatisfied path is not an outage, or a local network "
			"denial alone is one");
	}
	require(hostNetworkPathIsOutage(nw_path_status_unsatisfied,
					nw_path_unsatisfied_reason_not_available),
		"no network is not an outage");
	require(!hostNetworkPathIsOutage(
			nw_path_status_unsatisfied,
			nw_path_unsatisfied_reason_local_network_denied),
		"a local network denial alone is an outage");
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
	the_first_monitor_ties_no_port_to_the_thread_that_made_it();
	a_state_nobody_reported_to_is_usable_and_each_report_replaces_it();
	only_an_unsatisfied_path_is_an_outage_and_a_local_network_denial_is_not();
	the_monitor_reports_the_path_at_each_start();
	a_monitor_that_ended_writes_nothing();
	std::puts("host network path tests passed");
	return 0;
}
