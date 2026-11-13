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
  `umlandroid17` or the user network's default address;
- eth0 is configured and the host reaches adbd through it, so the user network
  answers the guest under its own address;
- a guest reboot (the restart loop passes the address on) and a new launch of
  the same instance keep the values, and the file is untouched;
- a second instance gets a serial number and an address of its own.

The values are random per installation and are never printed: a failure names
what differed, not what the value was. The guest's Wi-Fi address is derived in
the guest from eth0's by a later release and is not checked here.

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
import slot_ports  # noqa: E402

DEFAULT_ADB_PORT = 5575
IDENTITY_FILE = "identity.json"
LEGACY_SERIAL = "umlandroid17"
DEFAULT_ETHERNET_MAC = "52:54:00:12:34:56"
STOP_SECONDS = 120
SETTLE_SECONDS = 60
TERMINATION_SIGNALS = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)
MAC_PATTERN = re.compile(r"^[0-9a-f]{2}(:[0-9a-f]{2}){5}$")


class Failure(RuntimeError):
    pass


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
        self.device = Device(options.adb, f"127.0.0.1:{options.adb_port}")

    def start(self) -> None:
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
        process, self.process = self.process, None
        if process is not None and process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=STOP_SECONDS)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=30)
        self.device.run("disconnect", self.device.serial, timeout=30)

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
    if not MAC_PATTERN.match(guest.ethernet) or guest.ethernet != members["ethernet_mac"]:
        raise Failure("eth0's address is not the identity file's")
    if guest.ethernet == DEFAULT_ETHERNET_MAC:
        raise Failure("eth0 kept the user network's default address")
    return guest


def check_network(device: Device) -> None:
    """eth0 is configured under its own address; adbd was reached through it."""
    if " inet " not in device.shell("ip -4 addr show dev eth0"):
        raise Failure("eth0 has no IPv4 address")


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
    return parser


def main() -> int:
    options = build_parser().parse_args()
    options.adb = shutil.which(options.adb) or options.adb
    for name in TERMINATION_SIGNALS:
        signal.signal(name, terminate_on_signal)
    guests: list[Guest] = []
    with tempfile.TemporaryDirectory(prefix="android-installation-identity-") as scratch_name:
        scratch = pathlib.Path(scratch_name)
        log_path = scratch / "launcher.log"
        try:
            first = Guest(options, scratch / "first", log_path)
            guests.append(first)
            first.start()
            identity = check_guest_matches_file(first.device, first.instance)
            check_network(first.device)
            file_before, _ = read_identity_file(first.instance)
            print("first installation: its own serial number and eth0 address, network up")

            first.reboot()
            if not check_guest_matches_file(first.device, first.instance).same_as(identity):
                raise Failure("a guest reboot changed the identity")
            check_network(first.device)
            print("reboot: identity kept")

            first.stop()
            first.start()
            if not check_guest_matches_file(first.device, first.instance).same_as(identity):
                raise Failure("a new launch of the instance changed the identity")
            if read_identity_file(first.instance)[0] != file_before:
                raise Failure(f"a new launch rewrote {IDENTITY_FILE}")
            print("new launch of the same instance: identity kept, file untouched")
            first.stop()

            second = Guest(options, scratch / "second", log_path)
            guests.append(second)
            second.start()
            other = check_guest_matches_file(second.device, second.instance)
            check_network(second.device)
            if other.serial == identity.serial:
                raise Failure("the second installation has the first one's serial number")
            if other.ethernet == identity.ethernet:
                raise Failure("the second installation has the first one's eth0 address")
            print("second installation: a serial number and an eth0 address of its own")
            print("PASS")
            return 0
        except (Failure, subprocess.TimeoutExpired, OSError) as problem:
            print(f"FAIL: {problem}", file=sys.stderr)
            if log_path.is_file():
                print("launcher log tail:\n" + "\n".join(log_path.read_text(errors="replace").splitlines()[-30:]),
                      file=sys.stderr)
            return 1
        finally:
            ignore_termination()
            for guest in guests:
                guest.stop()


if __name__ == "__main__":
    raise SystemExit(main())
