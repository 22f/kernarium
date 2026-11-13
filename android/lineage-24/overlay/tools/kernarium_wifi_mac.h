// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The virtual Wi-Fi's hardware address, derived from eth0's.
 *
 * The host gives every installation its own eth0 address
 * (darwin/device/block/android_identity.hpp). The guest's Wi-Fi address is
 * a function of it, so installations differ in both and no new value travels
 * on the kernel command line: an `androidboot.*` argument becomes a `ro.boot.*`
 * property that every application can read, while eth0's address is visible to
 * the shell and to privileged callers only.
 *
 * The function is a keyed mix of the six bytes: the finalizer of MurmurHash3
 * (a bijection on 64 bits) over the address and a key, mixed again with a
 * round number. The first six bytes of the result are the candidate, with the
 * multicast bit cleared and the locally administered bit set. A candidate is
 * refused when it is eth0's own address, 02:00:00:00:00:00 (the address
 * Android reports to applications that may not see the real one), or an
 * address of the host's virtual router (52:55:0a:00:02:xx,
 * darwin/device/network/slirp/provider.hpp), and the next round is tried. Every
 * input has a result, an address that is not eth0's own included (the old
 * default 52:54:00:12:34:56 of a host without per-installation addresses), and
 * the same input always has the same result.
 *
 * tests/unit/andarium/wifi_mac_tests.cpp runs this header against the golden
 * vectors that tests/integration/android/run_android_installation_identity.py
 * checks against the guest too.
 */

#ifndef KERNARIUM_WIFI_MAC_H_
#define KERNARIUM_WIFI_MAC_H_

#include <stddef.h>
#include <stdint.h>

#include <array>

namespace kernarium_net {

using Mac = std::array<unsigned char, 6>;

// "krn_wifi".
constexpr uint64_t kWifiMacKey = 0x6b726e5f77696669ULL;
// The rounds tried before the fallback; a round is refused about once in 2^45.
constexpr uint64_t kWifiMacRounds = 16;

// The finalizer of MurmurHash3's 64-bit hash: a bijection.
constexpr uint64_t Mix64(uint64_t value) {
  value ^= value >> 33;
  value *= 0xff51afd7ed558ccdULL;
  value ^= value >> 33;
  value *= 0xc4ceb9fe1a85ec53ULL;
  value ^= value >> 33;
  return value;
}

constexpr uint64_t PackMac(const Mac& mac) {
  uint64_t packed = 0;
  for (size_t index = 0; index < mac.size(); ++index) {
    packed = packed << 8 | mac[index];
  }
  return packed;
}

// The candidate of one round: the first six bytes of the mix, as a unicast,
// locally administered address.
constexpr Mac WifiMacCandidate(const Mac& ethernet, uint64_t key, uint64_t round) {
  const uint64_t mixed = Mix64(Mix64(PackMac(ethernet) ^ key) + round);
  Mac candidate = {};
  for (size_t index = 0; index < candidate.size(); ++index) {
    candidate[index] = static_cast<unsigned char>(mixed >> (56 - 8 * index));
  }
  candidate[0] = static_cast<unsigned char>((candidate[0] | 0x02) & 0xfe);
  return candidate;
}

// Whether a candidate may be the Wi-Fi address of a guest whose eth0 is
// |ethernet|. The comparisons are on the packed values: std::array's operator==
// is constexpr only from C++20.
constexpr bool WifiMacAllowed(const Mac& candidate, const Mac& ethernet) {
  const uint64_t packed = PackMac(candidate);
  if (packed == PackMac(ethernet)) return false;
  if (packed == 0x020000000000ULL) return false;
  // The host's virtual router and name server: 52:55:0a:00:02:xx.
  return (packed >> 8) != 0x52550a0002ULL;
}

// |key| and |rounds| are the tests' to change.
constexpr Mac DeriveWifiMac(const Mac& ethernet, uint64_t key = kWifiMacKey,
                            uint64_t rounds = kWifiMacRounds) {
  for (uint64_t round = 0; round < rounds; ++round) {
    const Mac candidate = WifiMacCandidate(ethernet, key, round);
    if (WifiMacAllowed(candidate, ethernet)) return candidate;
  }
  // Out of reach for any input, but the function stays total: the address the
  // image used for every guest, or its neighbour when that is eth0's own.
  if (PackMac(ethernet) == 0x0215b2000000ULL) return Mac{0x02, 0x15, 0xb2, 0x00, 0x00, 0x01};
  return Mac{0x02, 0x15, 0xb2, 0x00, 0x00, 0x00};
}

}  // namespace kernarium_net

#endif  // KERNARIUM_WIFI_MAC_H_
