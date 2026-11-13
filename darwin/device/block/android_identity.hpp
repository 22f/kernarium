// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef UML_DEVICE_BLOCK_ANDROID_IDENTITY_HPP
#define UML_DEVICE_BLOCK_ANDROID_IDENTITY_HPP

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

/* What tells one installation of the Android guest from another: the serial
 * number the guest reads as ro.serialno and the hardware address of its eth0.
 * Both are chosen at random once for an installation and kept beside the
 * instance's data disk, in a small file the host owns, so they follow the data
 * disk through an image update, a storage resize, a snapshot restore and the
 * guest's own factory reset, and go with it when the instance is erased.
 *
 * The values are never logged and are not a tracked fixture: a test makes its
 * own. They reach the guest as the kernel argument `androidboot.serialno=`
 * (readKernelCommandLine drops the release's own) and as the hardware address
 * the user network gives eth0. The guest's Wi-Fi address is derived from
 * eth0's inside the guest and is not stored here. */
namespace uml::android {

using MacAddress = std::array<std::uint8_t, 6>;

/* The identity file's name; it sits in the directory of the data disk. */
inline constexpr std::string_view identityFileName{"identity.json"};

/* The file that holds the identity of the installation whose data disk is
 * `dataDisk`. */
std::filesystem::path identityFile(const std::filesystem::path &dataDisk);

/* The hardware address the user network gives eth0 when nothing sets one, and
 * the guest tool's constant Wi-Fi address: neither is ever generated. */
inline constexpr MacAddress defaultEthernetMac{
	0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
inline constexpr MacAddress legacyWifiMac{0x02, 0x15, 0xb2, 0x00, 0x00, 0x00};

/* What a launcher resolved for one run: the serial number for the kernel
 * command line and the address for eth0. A member that is empty leaves the
 * guest with what it has without one (the release's serial, the user
 * network's default address). */
struct InstallationIdentity {
	std::optional<std::string> serial;
	std::optional<MacAddress> ethernetMac;

	bool operator==(const InstallationIdentity &) const = default;
};

/* Whether `serial` can be a guest's serial number: 6 to 20 letters and digits
 * (the form the platform's own tests require of Build.SERIAL), and not
 * "unknown", the value an application that may not read it is given. */
[[nodiscard]] bool validSerial(std::string_view serial) noexcept;

/* Whether `mac` can be eth0's address: unicast, locally administered, and none
 * of 02:00:00:00:00:00 (the platform's "no address" value), the user network's
 * default or the legacy Wi-Fi address. */
[[nodiscard]] bool validEthernetMac(const MacAddress &mac) noexcept;

/* `aa:bb:cc:dd:ee:ff`, lower case. */
[[nodiscard]] std::string formatMac(const MacAddress &mac);

/* The address in the text of `aa:bb:cc:dd:ee:ff` (either case), or empty when
 * the text is anything else. */
[[nodiscard]] std::optional<MacAddress> parseMac(std::string_view text) noexcept;

/* Fills the span with random bytes. */
using RandomFill = std::function<void(std::span<std::uint8_t>)>;

/* The system's secure random source. */
void systemRandom(std::span<std::uint8_t> bytes);

/* A new identity: a serial number of 16 digits `0-9a-f` (64 random bits) and a
 * locally administered unicast address of 46 random bits that validEthernetMac
 * accepts. */
[[nodiscard]] InstallationIdentity generateIdentity(const RandomFill &random);

/* The identity file's content, or empty when `file` does not exist. Throws
 * std::invalid_argument for a file that is not a valid identity (schema 1,
 * exactly the three members, valid values) and std::system_error when it
 * cannot be read. */
[[nodiscard]] std::optional<InstallationIdentity> readIdentityFile(
	const std::filesystem::path &file);

/* Replaces `file` with `identity`, which must hold both members valid: the
 * new content is written beside it, flushed and renamed over it. Throws
 * std::system_error. */
void writeIdentityFile(const std::filesystem::path &file,
		       const InstallationIdentity &identity);

/* What a launcher states about the identity. A value in `explicit_` is used as
 * given and nothing is read or written; the other member then stays unset. */
struct IdentityRequest {
	/* The identity file, or empty for none: the launcher then keeps the
	 * guest's fixed values unless a value is stated. */
	std::filesystem::path file;
	/* The data disk was created by this start, so no earlier identity
	 * belongs to it. */
	bool diskCreated = false;
	InstallationIdentity stated;
	RandomFill random = systemRandom;
};

enum class IdentityOutcome {
	/* Nothing to resolve: no file and no stated value. */
	fixed,
	/* The stated values, used as they are. */
	stated,
	/* The file's identity. */
	kept,
	/* A new identity, written: no file, or a new data disk. */
	created,
	/* A new identity, written over a file that was not a valid one. */
	replaced,
	/* A new identity that could not be written, used for this start only. */
	unsaved,
};

struct ResolvedIdentity {
	InstallationIdentity identity;
	IdentityOutcome outcome = IdentityOutcome::fixed;
};

/* The identity this start gives the guest. A stated value wins and touches no
 * file. Otherwise the file decides: its identity is kept, and a new one is
 * written when the file is absent or invalid, or the data disk is new. A file
 * that cannot be written never fails the start: the new identity is used and
 * the outcome says so. */
[[nodiscard]] ResolvedIdentity resolveIdentity(const IdentityRequest &request);

/* The kernel argument that gives the guest its serial number. Android init
 * imports it as ro.boot.serialno and ro.serialno, so a launcher that resolved
 * a serial sets it, and a release's command line may carry only the legacy
 * argument below, which readKernelCommandLine() then drops. */
inline constexpr std::string_view serialPrefix{"androidboot.serialno="};

/* The argument every release published before the launchers set the serial
 * carries. */
inline constexpr std::string_view legacySerialArgument{
	"androidboot.serialno=umlandroid17"};

/* `androidboot.serialno=` followed by `serial`. */
[[nodiscard]] std::string serialArgument(std::string_view serial);

} // namespace uml::android

#endif
