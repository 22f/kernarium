// SPDX-License-Identifier: GPL-2.0-or-later
//
// The guest's Wi-Fi address as a function of eth0's
// (android/lineage-24/overlay/tools/kernarium_wifi_mac.h): the golden vectors
// that the guest runner checks the guest against too, the properties every
// result has, and the rounds that refuse a candidate.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "kernarium_wifi_mac.h"
#include "require.hpp"

using kernarium_net::DeriveWifiMac;
using kernarium_net::Mac;
using kernarium_net::PackMac;
using uml::testing::require;

namespace {

Mac parse(const std::string &text)
{
	require(text.size() == 17, "an address is 17 characters: " + text);
	Mac mac{};
	for (std::size_t index = 0; index < mac.size(); ++index)
		mac[index] = static_cast<unsigned char>(
			std::stoul(text.substr(index * 3, 2), nullptr, 16));
	return mac;
}

std::string text(const Mac &mac)
{
	char buffer[18];
	std::snprintf(buffer, sizeof(buffer), "%02x:%02x:%02x:%02x:%02x:%02x",
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return buffer;
}

Mac unpack(std::uint64_t packed)
{
	Mac mac{};
	for (std::size_t index = 0; index < mac.size(); ++index)
		mac[index] = static_cast<unsigned char>(packed >> (40 - 8 * index));
	return mac;
}

/* What the guest tool must never give: eth0's own address, the address
 * Android shows applications, or one of the host's virtual router. */
void require_valid(const Mac &wifi, const Mac &ethernet)
{
	const std::string where = " for " + text(ethernet);
	require((wifi[0] & 0x01) == 0, "multicast" + where);
	require((wifi[0] & 0x02) != 0, "not locally administered" + where);
	require(PackMac(wifi) != PackMac(ethernet), "eth0's own address" + where);
	require(PackMac(wifi) != 0x020000000000ULL, "02:00:00:00:00:00" + where);
	require((PackMac(wifi) >> 8) != 0x52550a0002ULL, "a router address" + where);
}

void test_golden_vectors(const char *path)
{
	std::ifstream file(path);
	require(file.is_open(), std::string{"no golden vectors: "} + path);
	std::string line;
	int checked = 0;
	while (std::getline(file, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		std::istringstream fields(line);
		std::string ethernet, wifi;
		require(static_cast<bool>(fields >> ethernet >> wifi), "a vector line: " + line);
		const Mac derived = DeriveWifiMac(parse(ethernet));
		require(text(derived) == wifi, "the derivation of " + ethernet + " changed");
		require_valid(derived, parse(ethernet));
		++checked;
	}
	require(checked >= 10, "too few golden vectors");
}

/* A cheap deterministic stream of addresses, locally administered unicast
 * like the host's own and then any six bytes. */
void test_properties()
{
	std::uint64_t state = 0x9e3779b97f4a7c15ULL;
	const auto next = [&state] {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return state;
	};
	std::set<std::uint64_t> wifi_seen;
	constexpr int count = 100000;
	for (int index = 0; index < count; ++index) {
		Mac ethernet = unpack(next() & 0xffffffffffffULL);
		if (index % 2 == 0)
			ethernet[0] = static_cast<unsigned char>((ethernet[0] | 0x02) & 0xfe);
		const Mac wifi = DeriveWifiMac(ethernet);
		require_valid(wifi, ethernet);
		require(DeriveWifiMac(ethernet) == wifi, "not a function");
		wifi_seen.insert(PackMac(wifi));
	}
	/* About 2^46 results for 10^5 inputs: a collision here is a defect of the mix. */
	require(wifi_seen.size() == static_cast<std::size_t>(count),
		"two eth0 addresses share a Wi-Fi address");
	/* Installations differ in the one when they differ in the other, even by a bit. */
	const Mac base = parse("0a:11:22:33:44:55");
	for (std::size_t byte = 0; byte < base.size(); ++byte)
		for (int bit = 0; bit < 8; ++bit) {
			if (byte == 0 && bit < 2)
				continue;
			Mac other = base;
			other[byte] = static_cast<unsigned char>(other[byte] ^ (1U << bit));
			require(DeriveWifiMac(other) != DeriveWifiMac(base),
				"one flipped bit gives the same Wi-Fi address");
		}
	/* The old default of a host without per-installation addresses is no special case. */
	require_valid(DeriveWifiMac(parse("52:54:00:12:34:56")), parse("52:54:00:12:34:56"));
}

constexpr std::uint64_t inverse_of(std::uint64_t odd)
{
	std::uint64_t inverse = odd;
	for (int step = 0; step < 6; ++step)
		inverse *= 2 - odd * inverse;
	return inverse;
}

constexpr std::uint64_t unmix64(std::uint64_t value)
{
	value ^= value >> 33;
	value *= inverse_of(0xc4ceb9fe1a85ec53ULL);
	value ^= value >> 33;
	value *= inverse_of(0xff51afd7ed558ccdULL);
	value ^= value >> 33;
	return value;
}

static_assert(kernarium_net::Mix64(unmix64(0x0123456789abcdefULL)) == 0x0123456789abcdefULL);

/* The key under which round 0 of |ethernet| gives |target|. */
std::uint64_t key_for(const Mac &ethernet, const Mac &target)
{
	const std::uint64_t mixed = PackMac(target) << 16;
	const std::uint64_t key = PackMac(ethernet) ^ unmix64(unmix64(mixed));
	require(kernarium_net::WifiMacCandidate(ethernet, key, 0) == target,
		"the test's key does not give its target");
	return key;
}

void test_refused_candidates()
{
	const Mac ethernet = parse("0a:11:22:33:44:55");
	for (const char *refused : {"0a:11:22:33:44:55", "02:00:00:00:00:00",
				    "52:55:0a:00:02:02", "52:55:0a:00:02:ff", "52:55:0a:00:02:00"}) {
		const std::uint64_t key = key_for(ethernet, parse(refused));
		const Mac first = kernarium_net::WifiMacCandidate(ethernet, key, 0);
		require(!kernarium_net::WifiMacAllowed(first, ethernet),
			std::string{"round 0 may not give "} + refused);
		/* The next round's candidate is the answer. */
		const Mac derived = DeriveWifiMac(ethernet, key);
		require(derived == kernarium_net::WifiMacCandidate(ethernet, key, 1),
			std::string{"the next round was not tried after "} + refused);
		require_valid(derived, ethernet);
	}
	/* Neighbours of the refused families are fine. */
	require(kernarium_net::WifiMacAllowed(parse("52:55:0a:00:03:02"), ethernet));
	require(kernarium_net::WifiMacAllowed(parse("52:55:0a:01:02:02"), ethernet));
	require(kernarium_net::WifiMacAllowed(parse("02:00:00:00:00:01"), ethernet));
	/* No round at all: the function stays total, and never eth0's own address. */
	require(text(DeriveWifiMac(ethernet, kernarium_net::kWifiMacKey, 0)) == "02:15:b2:00:00:00");
	require(text(DeriveWifiMac(parse("02:15:b2:00:00:00"), kernarium_net::kWifiMacKey, 0)) ==
		"02:15:b2:00:00:01");
}

} // namespace

int main(int argc, char *argv[])
{
	require(argc == 2, "the golden vectors' path is the argument");
	test_golden_vectors(argv[1]);
	test_properties();
	test_refused_candidates();
	std::puts("wifi mac tests passed");
	return 0;
}
