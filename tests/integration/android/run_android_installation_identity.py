#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify the installation identity of an Android guest.

Every installation (one instance directory, one data disk) gives the guest its
own serial number and its own eth0 address instead of the release's fixed ones;
the host keeps them in `identity.json` beside the data disk
(darwin/device/block/android_identity.hpp). The guest is the release started
with the macOS launcher (tools/boot_android.py), and the runner checks that:

- the file exists, is private to its owner, and the guest's `ro.serialno`,
  `ro.boot.serialno` and eth0 address are the file's, not the release's
  `umlandroid17` or the user network's default address, and the kernel's command
  line carries one serial number, the file's;
- eth0 is configured and the host reaches adbd through it, so the user network
  answers the guest under its own address;
- a guest reboot (the restart loop passes the address on) and a new launch of
  the same instance keep the values, and the file is untouched;
- a second instance gets a serial number and an address of its own.

The values are random per installation and are never printed: a failure names
what differed, not what the value was, and the launcher's log tail that a
failure prints has every serial number and hardware address masked (`mask`).

Two options qualify what only a newer image carries (the accepted release has
neither, and the default run, which passes on it, checks neither):

- `--command-line FILE` starts the release with FILE as its kernel command line
  (a clone of the release directory: the file the next image will carry, the
  tracked android/lineage-24/support/boot/kernel-command-line.txt, which has no
  serial number) and the same checks hold: the launcher's serial number is the
  guest's, with none from the image to drop;
- `--image-inputs` expects an image built with this repository's overlay, and
  checks that wlan0's address is the one derived from eth0's
  (overlay/tools/kernarium_wifi_mac.h, here `derive_wifi_mac`, which the golden
  vectors tie to the C++), that it is not the old fixed one, that it is the
  same after a reboot and a new launch and differs between installations, that
  wlan0 holds an IPv4 lease (Wi-Fi connected), and that an application cannot
  read `ro.boot.kernarium.adb_key`. The last check starts the emulator through
  a wrapper that adds the key and a control argument to the kernel command line
  (the macOS launcher passes neither): the application reads the control
  property, which every application may, and the key, which it may not, while
  the kernel's command line shows the key was passed.

Every wait has a deadline; the guest's launcher is stopped through its own
process on any exit.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import json
import os
import pathlib
import re
import secrets
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
from typing import Callable

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
LAUNCHER = REPOSITORY / "tools" / "boot_android.py"
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import slot_ports  # noqa: E402
import property_probe_apk  # noqa: E402

DEFAULT_ADB_PORT = 5575
IDENTITY_FILE = "identity.json"
LEGACY_SERIAL = "umlandroid17"
DEFAULT_ETHERNET_MAC = "52:54:00:12:34:56"
LEGACY_WIFI_MAC = "02:15:b2:00:00:00"
DEFAULT_EMULATOR = REPOSITORY / ".build" / "presets" / "macos-android17" / "bin" / "uml-darwin-android17"
GOLDEN_VECTORS = REPOSITORY / "tests" / "unit" / "andarium" / "fixtures" / "wifi-mac-golden-vectors.txt"
KEY_PROPERTY = "ro.boot.kernarium.adb_key"
CONTROL_PROPERTY = "ro.boot.kernarium.identity_probe"
LEASE_SECONDS = 120
STOP_SECONDS = 120
SETTLE_SECONDS = 60
TERMINATION_SIGNALS = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)
MAC_PATTERN = re.compile(r"^[0-9a-f]{2}(:[0-9a-f]{2}){5}$")
ANY_MAC = re.compile(r"(?i)(?<![0-9a-z])(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}(?![0-9a-z])")
SERIAL_ASSIGNMENT = re.compile(r"(?i)(serialno\W{0,4})[0-9a-z]{6,20}")
MASKED = "<masked>"


class Failure(RuntimeError):
    pass


def mask(text: str, known: tuple[str, ...] = ()) -> str:
    """The text with every serial number and hardware address replaced: the values this
    run knows (the identity files'), any address, and whatever follows `serialno`."""
    for value in known:
        if value:
            text = text.replace(value, MASKED)
    return SERIAL_ASSIGNMENT.sub(r"\1" + MASKED, ANY_MAC.sub(MASKED, text))


def known_values(instances: list[pathlib.Path]) -> tuple[str, ...]:
    """The serial numbers and addresses of the identity files that exist, best effort."""
    values: list[str] = []
    for instance in instances:
        try:
            members = json.loads((instance / IDENTITY_FILE).read_text())
            values += [str(members.get("serialno", "")), str(members.get("ethernet_mac", ""))]
        except (OSError, ValueError, AttributeError):
            pass
    return tuple(values)


def ignore_termination() -> None:
    """Cleanup runs to its own deadlines; a second signal must not cut it short."""
    for name in TERMINATION_SIGNALS:
        signal.signal(name, signal.SIG_IGN)


def terminate_on_signal(signum: int, _frame: object) -> None:
    ignore_termination()
    raise SystemExit(128 + signum)


class Device:
    """The guest as the host's adb sees it, over the shared server."""

    def __init__(self, adb: str, serial: str) -> None:
        self.adb = adb
        self.serial = serial

    def run(self, *arguments: str, timeout: int = 30) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, *arguments], text=True, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
            check=False)

    def connect(self) -> bool:
        try:
            self.run("connect", self.serial, timeout=15)
            state = self.run("-s", self.serial, "get-state", timeout=15)
        except subprocess.TimeoutExpired:
            return False
        return state.returncode == 0 and state.stdout.strip() == "device"

    def shell(self, command: str, timeout: int = 30) -> str:
        result = self.run("-s", self.serial, "shell", command, timeout=timeout)
        if result.returncode != 0:
            raise Failure(f"'{command}' exited {result.returncode}")
        return result.stdout.strip()

    def getprop(self, name: str) -> str:
        return self.shell(f"getprop {name}")


def wait_for(description: str, probe: Callable[[], bool], seconds: int,
             guest: subprocess.Popen | None = None) -> None:
    """Poll the probe until it is true; a probe that raises is not true yet."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if guest is not None and guest.poll() is not None:
            raise Failure(f"the guest's launcher exited ({guest.returncode}) while waiting for {description}")
        try:
            if probe():
                return
        except (Failure, subprocess.TimeoutExpired):
            pass
        time.sleep(1)
    raise Failure(f"{description}: not within {seconds} s")


def boot_id(device: Device) -> str:
    return device.shell("cat /proc/sys/kernel/random/boot_id")


def wait_booted(device: Device, seconds: int, guest: subprocess.Popen | None,
                previous_boot: str | None = None) -> None:
    def booted() -> bool:
        if not device.connect() or device.getprop("sys.boot_completed") != "1":
            return False
        return previous_boot is None or boot_id(device) != previous_boot
    wait_for("the guest to be booted", booted, seconds, guest)


class Guest:
    """One launch of an instance: the launcher process and its adb connection."""

    def __init__(self, options: argparse.Namespace, instance: pathlib.Path, log: pathlib.Path) -> None:
        self.options = options
        self.instance = instance
        self.log = log
        self.process: subprocess.Popen | None = None
        self.connected = True
        self.device = Device(options.adb, f"127.0.0.1:{options.adb_port}")

    def start(self) -> None:
        self.connected = True
        options = self.options
        command = [sys.executable, str(LAUNCHER), "--instance", str(self.instance),
                   "--adb", options.adb, "--adb-port", str(options.adb_port), "--display", "offscreen",
                   "--cpus", str(options.cpus), "--boot-timeout", str(options.boot_timeout)]
        if options.release:
            command += ["--release", str(options.release)]
        if options.emulator:
            command += ["--emulator", str(options.emulator)]
        with open(self.log, "ab") as log:
            self.process = subprocess.Popen(
                command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
        wait_booted(self.device, options.boot_timeout, self.process)

    def stop(self) -> None:
        """Ends the launcher and drops the connection; neither a launcher that ignores SIGTERM
        nor an adb that times out keeps the other half from running."""
        process, self.process = self.process, None
        try:
            if process is not None and process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=STOP_SECONDS)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=30)
        finally:
            self.disconnect()

    def disconnect(self) -> None:
        """Best effort: a guest that is gone has nothing to disconnect from."""
        if not self.connected:
            return
        self.connected = False
        try:
            self.device.run("disconnect", self.device.serial, timeout=30)
        except (subprocess.TimeoutExpired, OSError):
            pass

    def reboot(self) -> None:
        previous = boot_id(self.device)
        self.device.run("-s", self.device.serial, "reboot", timeout=30)
        wait_booted(self.device, self.options.boot_timeout, self.process, previous)


class Identity:
    """What a guest reports about itself, and what the host's file says."""

    def __init__(self, serial: str, boot_serial: str, ethernet: str) -> None:
        self.serial = serial
        self.boot_serial = boot_serial
        self.ethernet = ethernet

    @staticmethod
    def of(device: Device) -> "Identity":
        return Identity(device.getprop("ro.serialno"), device.getprop("ro.boot.serialno"),
                        ethernet_address(device))

    def same_as(self, other: "Identity") -> bool:
        return (self.serial, self.boot_serial, self.ethernet) == (other.serial, other.boot_serial, other.ethernet)


def ethernet_address(device: Device) -> str:
    """eth0's hardware address as `ip addr` shows it: the shell may read neither sysfs nor
    a single link through netlink (`ip link show eth0` is refused), only the whole listing."""
    for line in device.shell("ip addr show eth0").splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] == "link/ether":
            return fields[1].lower()
    raise Failure("`ip addr` shows no hardware address for eth0")


def read_identity_file(instance: pathlib.Path) -> tuple[bytes, dict]:
    path = instance / IDENTITY_FILE
    if not path.is_file():
        raise Failure(f"the instance has no {IDENTITY_FILE}")
    mode = stat.S_IMODE(path.stat().st_mode)
    if mode & 0o077:
        raise Failure(f"{IDENTITY_FILE} is readable beyond its owner (mode {mode:o})")
    content = path.read_bytes()
    try:
        members = json.loads(content)
    except ValueError as problem:
        raise Failure(f"{IDENTITY_FILE} is not JSON: {problem}") from problem
    if set(members) != {"schema", "serialno", "ethernet_mac"} or members["schema"] != 1:
        raise Failure(f"{IDENTITY_FILE} has other members than schema 1's")
    return content, members


def check_guest_matches_file(device: Device, instance: pathlib.Path) -> Identity:
    """The guest reports the installation's identity, and not the release's own."""
    _, members = read_identity_file(instance)
    guest = Identity.of(device)
    if guest.serial != members["serialno"]:
        raise Failure("ro.serialno is not the identity file's serial number")
    if guest.boot_serial != members["serialno"]:
        raise Failure("ro.boot.serialno is not the identity file's serial number")
    if guest.serial == LEGACY_SERIAL:
        raise Failure("the guest kept the release's fixed serial number")
    # The release's own serial number is dropped from the command line, so the guest is given one.
    arguments = [token for token in device.shell("cat /proc/cmdline").split()
                 if token.startswith("androidboot.serialno=")]
    if arguments != [f"androidboot.serialno={members['serialno']}"]:
        raise Failure("the kernel command line does not carry exactly the installation's serial number")
    if not MAC_PATTERN.match(guest.ethernet) or guest.ethernet != members["ethernet_mac"]:
        raise Failure("eth0's address is not the identity file's")
    if guest.ethernet == DEFAULT_ETHERNET_MAC:
        raise Failure("eth0 kept the user network's default address")
    return guest


def check_network(device: Device) -> None:
    """eth0 is configured under its own address; adbd was reached through it."""
    if " inet " not in device.shell("ip -4 addr show dev eth0"):
        raise Failure("eth0 has no IPv4 address")


# ---- What only a newer image carries (--command-line, --image-inputs).

MASK64 = (1 << 64) - 1
WIFI_KEY = 0x6B726E5F77696669  # "krn_wifi"
WIFI_ROUNDS = 16


def mix64(value: int) -> int:
    """The finalizer of MurmurHash3's 64-bit hash, as in kernarium_wifi_mac.h."""
    value ^= value >> 33
    value = value * 0xFF51AFD7ED558CCD & MASK64
    value ^= value >> 33
    value = value * 0xC4CEB9FE1A85EC53 & MASK64
    return value ^ value >> 33


def format_mac(raw: bytes) -> str:
    return ":".join(f"{byte:02x}" for byte in raw)


def derive_wifi_mac(ethernet: str, key: int = WIFI_KEY, rounds: int = WIFI_ROUNDS) -> str:
    """wlan0's address as a function of eth0's: the port of DeriveWifiMac in
    android/lineage-24/overlay/tools/kernarium_wifi_mac.h. The golden vectors that the C++
    unit test checks too are what ties the two."""
    packed = int(ethernet.replace(":", ""), 16)
    for round_number in range(rounds):
        mixed = mix64(mix64(packed ^ key) + round_number & MASK64)
        candidate = bytearray(mixed.to_bytes(8, "big")[:6])
        candidate[0] = (candidate[0] | 0x02) & 0xFE
        value = int.from_bytes(candidate, "big")
        if value not in (packed, 0x020000000000) and value >> 8 != 0x52550A0002:
            return format_mac(bytes(candidate))
    return "02:15:b2:00:00:01" if packed == 0x0215B2000000 else LEGACY_WIFI_MAC


def golden_vectors(path: pathlib.Path = GOLDEN_VECTORS) -> list[tuple[str, str]]:
    vectors = []
    for line in path.read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            ethernet, wifi = line.split()
            vectors.append((ethernet, wifi))
    return vectors


def link_address(device: Device, name: str) -> str:
    """A device's hardware address as `ip addr` shows it (see ethernet_address)."""
    for line in device.shell(f"ip addr show {name}").splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] == "link/ether":
            return fields[1].lower()
    raise Failure(f"`ip addr` shows no hardware address for {name}")


class Probes:
    """What `--image-inputs` passes to the guest and asks it about: a key and a control value
    for the kernel command line, and the application that reads them as an application."""

    def __init__(self, scratch: pathlib.Path, emulator: pathlib.Path) -> None:
        # The key is a placeholder: the macOS launcher fixes ADB authorization off, so adbd
        # never compares it with a host's.
        self.key = "probe" + secrets.token_hex(8)
        self.control = secrets.token_hex(8)
        self.emulator = scratch / "emulator-with-probes"
        self.emulator.write_text(
            "#!/bin/sh\n"
            f"exec {shlex.quote(str(emulator))} \"$@\" "
            f"androidboot.kernarium.adb_key={self.key} "
            f"androidboot.kernarium.identity_probe={self.control}\n")
        self.emulator.chmod(0o700)
        self.apk = property_probe_apk.build_apk(scratch / "property-probe.apk")


def check_wifi(device: Device, ethernet: str) -> str:
    """wlan0 has the address derived from eth0's, is not the image's old fixed one, and holds
    its lease (the framework has joined the virtual network). Returns the address."""
    wifi = link_address(device, "wlan0")
    if wifi != derive_wifi_mac(ethernet):
        raise Failure("wlan0's address is not the one derived from eth0's")
    if wifi in (LEGACY_WIFI_MAC, ethernet):
        raise Failure("wlan0 has the image's old fixed address or eth0's own")
    wait_for("wlan0 to hold an IPv4 address", lambda: " inet " in device.shell("ip -4 addr show dev wlan0"),
             LEASE_SECONDS)
    return wifi


def check_key_property(device: Device, probes: Probes) -> None:
    """An application reads the control property and not the key, which the kernel was given."""
    if f"androidboot.kernarium.adb_key={probes.key}" not in device.shell("cat /proc/cmdline"):
        raise Failure("the kernel command line does not carry the probe key: the test cannot tell")
    if "package:" not in device.shell(f"pm path {property_probe_apk.PACKAGE} || true"):
        result = device.run("-s", device.serial, "install", str(probes.apk), timeout=120)
        if result.returncode != 0 or "Success" not in result.stdout:
            raise Failure("the property probe application did not install")
    package = property_probe_apk.PACKAGE
    if device.shell(f"run-as {package} getprop {CONTROL_PROPERTY}") != probes.control:
        raise Failure(f"an application cannot read {CONTROL_PROPERTY}: the test cannot tell")
    if device.shell(f"run-as {package} getprop {KEY_PROPERTY}"):
        raise Failure(f"an application can read {KEY_PROPERTY}")


def check_image(device: Device, ethernet: str, probes: Probes) -> str:
    wifi = check_wifi(device, ethernet)
    check_key_property(device, probes)
    return wifi


def prepared_release(release: pathlib.Path, command_line: pathlib.Path, scratch: pathlib.Path) -> pathlib.Path:
    """A clone of the release directory whose kernel command line is `command_line`. The clone is
    copy-on-write (APFS), so the release itself is never touched."""
    clone = scratch / "release"
    result = subprocess.run(["cp", "-c", "-R", str(release), str(clone)], text=True, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    if result.returncode != 0:
        raise Failure(f"cannot clone the release: {result.stdout.strip()}")
    target = clone / "boot" / "kernel-command-line.txt"
    target.unlink()
    shutil.copyfile(command_line, target)
    return clone


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--release", type=pathlib.Path, help="release directory (default: the accepted release)")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--emulator", type=pathlib.Path, help="the Android macOS executable (the launcher's default)")
    parser.add_argument("--adb-port", type=int, default=slot_ports.slot_port(slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_ADB_PORT})")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--boot-timeout", type=int, default=900)
    parser.add_argument("--scratch-dir", type=pathlib.Path,
                        help="where the run's instances (and, with --command-line, its release clone) live "
                        "(default: the system's temporary directory; the clone needs the release's volume)")
    parser.add_argument("--command-line", type=pathlib.Path,
                        help="start the release with this kernel command line (needs --release); "
                        "the tracked file of the next image carries no serial number")
    parser.add_argument("--image-inputs", action="store_true",
                        help="also check what an image built with this repository's overlay carries: "
                        "the derived Wi-Fi address and the restricted adb key property")
    return parser


def stop_all(guests: list[Guest]) -> None:
    """Every guest is stopped, whatever an earlier stop raises: the emulator runs in a process
    group of its own, so the runner's exit does not end it."""
    for guest in guests:
        try:
            guest.stop()
        except (subprocess.TimeoutExpired, OSError) as problem:
            print(f"cleanup: a guest did not stop cleanly: {type(problem).__name__}", file=sys.stderr)


def main() -> int:
    options = build_parser().parse_args()
    options.adb = shutil.which(options.adb) or options.adb
    for name in TERMINATION_SIGNALS:
        signal.signal(name, terminate_on_signal)
    guests: list[Guest] = []
    if options.command_line is not None and options.release is None:
        print("FAIL: --command-line needs --release", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="android-installation-identity-", dir=options.scratch_dir) as scratch_name:
        scratch = pathlib.Path(scratch_name)
        log_path = scratch / "launcher.log"
        try:
            probes = None
            if options.command_line is not None:
                options.release = prepared_release(options.release, options.command_line, scratch)
                print("release started with the given kernel command line")
            if options.image_inputs:
                probes = Probes(scratch, options.emulator or DEFAULT_EMULATOR)
                options.emulator = probes.emulator

            def image_inputs(guest: Guest, identity: Identity) -> str | None:
                return check_image(guest.device, identity.ethernet, probes) if probes else None

            first = Guest(options, scratch / "first", log_path)
            guests.append(first)
            first.start()
            identity = check_guest_matches_file(first.device, first.instance)
            check_network(first.device)
            wifi = image_inputs(first, identity)
            file_before, _ = read_identity_file(first.instance)
            print("first installation: its own serial number and eth0 address, network up")
            if probes:
                print("first installation: wlan0 derived from eth0, joined, the adb key unreadable by an application")

            first.reboot()
            if not check_guest_matches_file(first.device, first.instance).same_as(identity):
                raise Failure("a guest reboot changed the identity")
            check_network(first.device)
            if image_inputs(first, identity) != wifi:
                raise Failure("a guest reboot changed wlan0's address")
            print("reboot: identity kept")

            first.stop()
            first.start()
            if not check_guest_matches_file(first.device, first.instance).same_as(identity):
                raise Failure("a new launch of the instance changed the identity")
            if read_identity_file(first.instance)[0] != file_before:
                raise Failure(f"a new launch rewrote {IDENTITY_FILE}")
            if image_inputs(first, identity) != wifi:
                raise Failure("a new launch changed wlan0's address")
            print("new launch of the same instance: identity kept, file untouched")
            first.stop()

            second = Guest(options, scratch / "second", log_path)
            guests.append(second)
            second.start()
            other = check_guest_matches_file(second.device, second.instance)
            check_network(second.device)
            other_wifi = image_inputs(second, other)
            if other.serial == identity.serial:
                raise Failure("the second installation has the first one's serial number")
            if other.ethernet == identity.ethernet:
                raise Failure("the second installation has the first one's eth0 address")
            if other_wifi is not None and other_wifi == wifi:
                raise Failure("the second installation has the first one's wlan0 address")
            print("second installation: a serial number and an eth0 address of its own"
                  + (", and a wlan0 address" if probes else ""))
            print("PASS")
            return 0
        except (Failure, subprocess.TimeoutExpired, OSError) as problem:
            known = known_values([scratch / "first", scratch / "second"])
            print(f"FAIL: {mask(str(problem), known)}", file=sys.stderr)
            if log_path.is_file():
                tail = "\n".join(log_path.read_text(errors="replace").splitlines()[-30:])
                print("launcher log tail (serial numbers and addresses masked):\n" + mask(tail, known),
                      file=sys.stderr)
            return 1
        finally:
            ignore_termination()
            stop_all(guests)


if __name__ == "__main__":
    raise SystemExit(main())
