// SPDX-License-Identifier: GPL-2.0-or-later
//
// When a guest connection bound to an address the host has lost is reset
// (address_reset.hpp): the hold that a report of a carried path starts, what
// ends it without a reset, and which host sockets count as bound to a lost
// address. Both are functions of values and of the times a test hands in, so
// these tests need no socket, no clock and no guest.

#include "address_reset.hpp"
#include "require.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using namespace std::chrono_literals;
using uml::network::AddressResetHold;
using uml::network::HostAddresses;
using uml::network::local_address_is_gone;
using uml::testing::require;
using Clock = AddressResetHold::Clock;

const Clock::time_point t0{Clock::duration{1000s}};

void a_carried_path_starts_a_hold_that_ends_in_one_reset()
{
	AddressResetHold hold{8s};
	require(!hold.pending() && !hold.remaining(t0),
		"a hold was running before any report");
	hold.reported(true, true, t0);
	require(hold.pending() && hold.remaining(t0 + 3s) == 5s,
		"a carried path did not start a hold of its length");
	require(!hold.take_due(true, t0 + 7s + 999ms),
		"the hold ended before its time");
	require(hold.pending(), "asking early ended the hold");
	require(hold.take_due(true, t0 + 8s),
		"the hold did not end in a reset when its time came");
	require(!hold.pending() && !hold.take_due(true, t0 + 20s),
		"a hold ended in more than one reset");
}

void a_path_that_is_not_carried_cancels_the_hold()
{
	AddressResetHold hold{8s};
	hold.reported(true, true, t0);
	hold.reported(false, false, t0 + 2s);
	require(!hold.pending(), "an outage left the hold running");
	require(!hold.take_due(true, t0 + 100s),
		"a hold that an outage cancelled reset connections");
	/* And an outage alone starts nothing. */
	hold.reported(false, true, t0 + 200s);
	require(!hold.pending(), "an outage started a hold");
}

void a_report_that_changes_nothing_leaves_the_hold_running()
{
	AddressResetHold hold{8s};
	hold.reported(true, true, t0);
	hold.reported(true, false, t0 + 5s);
	require(hold.remaining(t0 + 5s) == 3s,
		"a report with the same addresses moved the hold's end");
	require(hold.take_due(true, t0 + 8s),
		"a report with the same addresses cancelled the hold");
}

void a_report_that_leaves_no_hold_running_starts_one_even_unchanged()
{
	AddressResetHold hold{8s};
	hold.reported(true, false, t0);
	require(hold.pending(),
		"a carried path with nothing changed started no hold");
}

void an_address_set_that_changes_during_the_hold_starts_it_again()
{
	AddressResetHold hold{8s};
	hold.reported(true, true, t0);
	hold.reported(true, true, t0 + 5s);
	require(!hold.take_due(true, t0 + 8s),
		"an address that came back did not move the hold's end");
	require(hold.remaining(t0 + 8s) == 5s,
		"the hold did not run its full length after the change");
	require(hold.take_due(true, t0 + 13s),
		"the restarted hold never ended");
}

void the_path_must_still_be_carried_when_the_hold_ends()
{
	AddressResetHold hold{8s};
	hold.reported(true, true, t0);
	require(!hold.take_due(false, t0 + 8s),
		"a hold ended in a reset on a path that is no longer carried");
	require(!hold.pending(), "an elapsed hold stayed pending");
}

void a_cleared_hold_never_ends_in_a_reset()
{
	AddressResetHold hold{8s};
	hold.reported(true, true, t0);
	hold.clear();
	require(!hold.pending() && !hold.take_due(true, t0 + 60s),
		"a cleared hold reset connections");
}

void an_elapsed_hold_has_no_time_left_and_the_default_is_eight_seconds()
{
	AddressResetHold hold{8s};
	hold.reported(true, true, t0);
	require(hold.remaining(t0 + 30s) == Clock::duration::zero(),
		"the time left of an elapsed hold is not zero");
	AddressResetHold standard;
	standard.reported(true, true, t0);
	require(standard.remaining(t0) == 8s,
		"the default hold is not eight seconds");
}

sockaddr_storage inet_address(const char *text)
{
	sockaddr_storage storage{};
	auto &address = reinterpret_cast<sockaddr_in &>(storage);
	address.sin_family = AF_INET;
	require(inet_pton(AF_INET, text, &address.sin_addr) == 1,
		"the test address does not parse");
	return storage;
}

HostAddresses table_of(std::uint32_t first, std::uint32_t second)
{
	HostAddresses table;
	table.assign_ipv4({first, second});
	return table;
}

void a_socket_is_bound_to_a_lost_address_when_no_interface_holds_it()
{
	/* 192.0.2.10 and 127.0.0.1. */
	const auto table = table_of(0xc000020aU, 0x7f000001U);
	require(!local_address_is_gone(inet_address("192.0.2.10"), table),
		"an address an interface holds is lost");
	require(!local_address_is_gone(inet_address("127.0.0.1"), table),
		"the loopback address is lost");
	require(local_address_is_gone(inet_address("192.0.2.77"), table),
		"an address no interface holds is not lost");
	require(local_address_is_gone(inet_address("198.51.100.40"),
				      HostAddresses{}),
		"no address of an empty table is lost");
	require(!local_address_is_gone(inet_address("192.0.2.77"),
				       HostAddresses::everything()),
		"an address is lost while every address counts as the host's");
}

void only_a_specific_internet_address_can_be_lost()
{
	const HostAddresses table;
	require(!local_address_is_gone(inet_address("0.0.0.0"), table),
		"the unspecified address of a socket not yet connected is lost");

	sockaddr_storage family6{};
	auto &six = reinterpret_cast<sockaddr_in6 &>(family6);
	six.sin6_family = AF_INET6;
	six.sin6_addr = in6addr_loopback;
	require(!local_address_is_gone(family6, table),
		"an IPv6 address is lost");

	sockaddr_storage unix_domain{};
	auto &path = reinterpret_cast<sockaddr_un &>(unix_domain);
	path.sun_family = AF_UNIX;
	std::strncpy(path.sun_path, "/tmp/x", sizeof(path.sun_path) - 1);
	require(!local_address_is_gone(unix_domain, table),
		"a Unix-domain socket is bound to a lost address");

	require(!local_address_is_gone(sockaddr_storage{}, table),
		"a socket of no family is bound to a lost address");
}

} // namespace

int main()
{
	a_carried_path_starts_a_hold_that_ends_in_one_reset();
	a_path_that_is_not_carried_cancels_the_hold();
	a_report_that_changes_nothing_leaves_the_hold_running();
	a_report_that_leaves_no_hold_running_starts_one_even_unchanged();
	an_address_set_that_changes_during_the_hold_starts_it_again();
	the_path_must_still_be_carried_when_the_hold_ends();
	a_cleared_hold_never_ends_in_a_reset();
	an_elapsed_hold_has_no_time_left_and_the_default_is_eight_seconds();
	a_socket_is_bound_to_a_lost_address_when_no_interface_holds_it();
	only_a_specific_internet_address_can_be_lost();
	std::puts("address reset rule tests passed");
	return 0;
}
