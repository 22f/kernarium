#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The installation identity runner's reading of the guest and of the host's file.

The runner (tests/integration/android/run_android_installation_identity.py)
compares what the guest reports with `identity.json` beside the data disk. The
values here are synthetic: a test never holds a real installation's.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[2] / "integration/android/run_android_installation_identity.py"
SPEC = importlib.util.spec_from_file_location("run_android_installation_identity", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)

SERIAL = "0123456789abcdef"
ETHERNET = "02:aa:bb:cc:dd:ee"
OTHER_ETHERNET = "02:aa:bb:cc:dd:ef"


def listing(address: str) -> str:
    """What `ip addr show eth0` prints, with the other lines a real one has."""
    return (
        "9: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc pfifo_fast state UNKNOWN group default qlen 1000\n"
        f"    link/ether {address.upper()} brd ff:ff:ff:ff:ff:ff\n"
        "    inet 10.0.2.15/24 brd 10.0.2.255 scope global eth0\n"
        "       valid_lft forever preferred_lft forever\n")


class FakeDevice:
    def __init__(self, serial: str = SERIAL, boot_serial: str = SERIAL, address: str = ETHERNET) -> None:
        self.properties = {"ro.serialno": serial, "ro.boot.serialno": boot_serial}
        self.address = address
        self.command_line = f"quiet androidboot.serialno={boot_serial} console=ttyS0"

    def getprop(self, name: str) -> str:
        return self.properties[name]

    def shell(self, command: str, timeout: int = 30) -> str:
        if command == "ip addr show eth0":
            return listing(self.address).strip()
        if command == "cat /proc/cmdline":
            return self.command_line
        raise runner.Failure(f"'{command}' exited 1")


class InstanceDirectory:
    def __init__(self, test: unittest.TestCase) -> None:
        self.scratch = tempfile.TemporaryDirectory()
        test.addCleanup(self.scratch.cleanup)
        self.path = Path(self.scratch.name)

    def write(self, members: dict | None = None, mode: int = 0o600) -> Path:
        members = members if members is not None else {
            "schema": 1, "serialno": SERIAL, "ethernet_mac": ETHERNET}
        file = self.path / runner.IDENTITY_FILE
        file.write_text(json.dumps(members))
        os.chmod(file, mode)
        return file


class EthernetAddressTests(unittest.TestCase):
    def test_the_address_is_the_one_on_eth0s_ether_line_in_lower_case(self) -> None:
        self.assertEqual(runner.ethernet_address(FakeDevice(address="02:AA:BB:CC:DD:EE")), ETHERNET)

    def test_a_listing_without_an_address_fails(self) -> None:
        device = FakeDevice()
        device.shell = lambda command, timeout=30: "9: eth0: <UP>\n    inet 10.0.2.15/24"  # type: ignore[method-assign]
        with self.assertRaises(runner.Failure):
            runner.ethernet_address(device)


class IdentityFileTests(unittest.TestCase):
    def test_a_private_file_of_schema_one_is_read(self) -> None:
        directory = InstanceDirectory(self)
        directory.write()
        content, members = runner.read_identity_file(directory.path)
        self.assertEqual((members["serialno"], members["ethernet_mac"]), (SERIAL, ETHERNET))
        self.assertEqual(json.loads(content), members)

    def test_what_the_runner_refuses(self) -> None:
        refused = {
            "no file": None,
            "readable by the group": (None, 0o640),
            "readable by everyone": (None, 0o644),
            "another schema": ({"schema": 2, "serialno": SERIAL, "ethernet_mac": ETHERNET}, 0o600),
            "another member": ({"schema": 1, "serialno": SERIAL, "ethernet_mac": ETHERNET, "x": 1}, 0o600),
            "a missing member": ({"schema": 1, "serialno": SERIAL}, 0o600),
        }
        for name, case in refused.items():
            with self.subTest(name):
                directory = InstanceDirectory(self)
                if case is not None:
                    members, mode = case
                    directory.write(members, mode)
                with self.assertRaises(runner.Failure):
                    runner.read_identity_file(directory.path)

    def test_a_file_that_is_not_json_is_refused(self) -> None:
        directory = InstanceDirectory(self)
        file = directory.write()
        file.write_text("{")
        with self.assertRaises(runner.Failure):
            runner.read_identity_file(directory.path)


class GuestMatchesFileTests(unittest.TestCase):
    def check(self, device: FakeDevice, members: dict | None = None) -> runner.Identity:
        directory = InstanceDirectory(self)
        directory.write(members)
        return runner.check_guest_matches_file(device, directory.path)

    def test_a_guest_that_reports_the_files_values_passes(self) -> None:
        identity = self.check(FakeDevice())
        self.assertEqual((identity.serial, identity.boot_serial, identity.ethernet), (SERIAL, SERIAL, ETHERNET))

    def test_every_difference_fails(self) -> None:
        cases = {
            "ro.serialno": FakeDevice(serial="fedcba9876543210"),
            "ro.boot.serialno": FakeDevice(boot_serial="fedcba9876543210"),
            "eth0's address": FakeDevice(address=OTHER_ETHERNET),
        }
        for name, command_line in {
            "no serial number on the command line": "quiet console=ttyS0",
            "the release's serial number beside the installation's":
                f"androidboot.serialno={runner.LEGACY_SERIAL} androidboot.serialno={SERIAL}",
            "another serial number on the command line": "androidboot.serialno=fedcba9876543210",
            "the serial number twice": f"androidboot.serialno={SERIAL} androidboot.serialno={SERIAL}",
        }.items():
            device = FakeDevice()
            device.command_line = command_line
            cases[name] = device
        for name, device in cases.items():
            with self.subTest(name), self.assertRaises(runner.Failure):
                self.check(device)

    def test_the_releases_own_values_fail_even_when_the_file_holds_them(self) -> None:
        legacy_serial = {"schema": 1, "serialno": runner.LEGACY_SERIAL, "ethernet_mac": ETHERNET}
        with self.assertRaises(runner.Failure):
            self.check(FakeDevice(serial=runner.LEGACY_SERIAL, boot_serial=runner.LEGACY_SERIAL), legacy_serial)
        default_address = {"schema": 1, "serialno": SERIAL, "ethernet_mac": runner.DEFAULT_ETHERNET_MAC}
        with self.assertRaises(runner.Failure):
            self.check(FakeDevice(address=runner.DEFAULT_ETHERNET_MAC), default_address)

    def test_identities_compare_on_all_three_values(self) -> None:
        base = runner.Identity(SERIAL, SERIAL, ETHERNET)
        self.assertTrue(base.same_as(runner.Identity(SERIAL, SERIAL, ETHERNET)))
        self.assertFalse(base.same_as(runner.Identity("fedcba9876543210", SERIAL, ETHERNET)))
        self.assertFalse(base.same_as(runner.Identity(SERIAL, "fedcba9876543210", ETHERNET)))
        self.assertFalse(base.same_as(runner.Identity(SERIAL, SERIAL, OTHER_ETHERNET)))


class MaskTests(unittest.TestCase):
    def test_serial_numbers_and_addresses_are_masked_wherever_they_stand(self) -> None:
        text = ("Kernel command line: quiet androidboot.serialno=feedface12345678 console=ttyS0\n"
                "[ro.serialno]: [feedface12345678]\n"
                "    link/ether 02:AA:BB:CC:DD:EE brd ff:ff:ff:ff:ff:ff\n"
                "mac 02-aa-bb-cc-dd-ef\n")
        masked = runner.mask(text)
        for value in ("feedface12345678", "02:AA:BB:CC:DD:EE", "02-aa-bb-cc-dd-ef", "ff:ff:ff:ff:ff:ff"):
            self.assertNotIn(value, masked)
        self.assertIn("quiet", masked)
        self.assertIn("console=ttyS0", masked)

    def test_a_value_the_runner_knows_is_masked_where_no_pattern_finds_it(self) -> None:
        masked = runner.mask("Build.SERIAL is abcd1234efgh5678 and so", ("abcd1234efgh5678", ""))
        self.assertNotIn("abcd1234efgh5678", masked)
        self.assertIn("Build.SERIAL is", masked)

    def test_the_values_of_the_identity_files_are_known(self) -> None:
        directory = InstanceDirectory(self)
        directory.write()
        known = runner.known_values([directory.path, directory.path / "absent"])
        self.assertEqual(set(known), {SERIAL, ETHERNET})


class FakeProcess:
    def __init__(self) -> None:
        self.alive = True
        self.signals = 0

    def poll(self):
        return None if self.alive else 0

    def send_signal(self, _signal: int) -> None:
        self.signals += 1
        self.alive = False

    def wait(self, timeout: float = 0) -> int:
        return 0

    def kill(self) -> None:
        self.alive = False


class RunnerLifecycleTests(unittest.TestCase):
    """main() with the guests replaced: what it prints on a failure and what it stops."""

    def run_main(self, guest_class) -> tuple[int | None, str, bool]:
        escaped = False
        output = io.StringIO()
        result = None
        with mock.patch.object(runner, "Guest", guest_class), \
                mock.patch.object(sys, "argv", [str(SCRIPT)]), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            try:
                result = runner.main()
            except subprocess.TimeoutExpired:
                escaped = True
        return result, output.getvalue(), escaped

    def test_a_failure_prints_a_log_tail_with_no_serial_number_or_address(self) -> None:
        serial, address = "feedface12345678", "02:44:55:66:77:88"

        class Failing(runner.Guest):
            def __init__(self, options, instance, log) -> None:
                super().__init__(options, instance, log)

            def start(self) -> None:
                self.instance.mkdir(parents=True, exist_ok=True)
                (self.instance / runner.IDENTITY_FILE).write_text(json.dumps(
                    {"schema": 1, "serialno": "abcd1234efgh5678", "ethernet_mac": "02:11:22:33:44:99"}))
                self.log.write_text(
                    f"Kernel command line: androidboot.serialno={serial}\nlink/ether {address}\n"
                    "Build.SERIAL abcd1234efgh5678 on 02:11:22:33:44:99\n")
                raise runner.Failure(f"ro.serialno {serial} is not the file's")

            def stop(self) -> None:
                pass

        result, text, _ = self.run_main(Failing)
        self.assertEqual(result, 1)
        self.assertIn("FAIL:", text)
        for value in (serial, address, "abcd1234efgh5678", "02:11:22:33:44:99"):
            self.assertNotIn(value, text)
        self.assertIn("launcher log tail", text)
        self.assertIn("androidboot.serialno=", text)

    def test_every_guest_is_stopped_when_an_earlier_disconnect_times_out(self) -> None:
        made: list = []

        class Device:
            def __init__(self, number: int) -> None:
                self.number = number
                self.serial = "synthetic-endpoint"
                self.disconnects = 0

            def run(self, *arguments: str, **keywords) -> subprocess.CompletedProcess:
                self.disconnects += 1
                if self.number == 0 and self.disconnects == 3:
                    raise subprocess.TimeoutExpired(["adb", "disconnect"], 30)
                return subprocess.CompletedProcess([], 0, "")

        class Guest(runner.Guest):
            def __init__(self, options, instance, log) -> None:
                super().__init__(options, instance, log)
                self.number = len(made)
                self.device = Device(self.number)
                made.append(self)

            def start(self) -> None:
                self.connected = True
                self.process = self.child = FakeProcess()

            def reboot(self) -> None:
                pass

        with mock.patch.object(runner, "check_guest_matches_file", lambda device, _instance: runner.Identity(
                    "first-serial" if device.number == 0 else "second-serial", "unused",
                    "02:11:22:33:44:55" if device.number == 0 else "06:11:22:33:44:55")), \
                mock.patch.object(runner, "check_network", lambda _device: None), \
                mock.patch.object(runner, "read_identity_file", lambda _instance: (b"unchanged", {})):
            result, text, escaped = self.run_main(Guest)
        self.assertFalse(escaped, "a disconnect timeout escaped the cleanup")
        self.assertEqual(result, 0, text)
        self.assertEqual(len(made), 2)
        self.assertGreater(made[1].child.signals, 0, "the second guest was never stopped")
        self.assertFalse(made[1].child.alive)

    def test_a_guest_stops_even_when_adb_does_not_answer_the_disconnect(self) -> None:
        class SilentDevice:
            serial = "synthetic-endpoint"

            def run(self, *arguments: str, **keywords) -> subprocess.CompletedProcess:
                raise subprocess.TimeoutExpired(["adb", "disconnect"], 30)

        options = runner.build_parser().parse_args([])
        guest = runner.Guest(options, Path("unused"), Path("unused.log"))
        guest.device = SilentDevice()  # type: ignore[assignment]
        process = FakeProcess()
        guest.process = process  # type: ignore[assignment]
        guest.stop()
        self.assertFalse(process.alive, "the launcher was not stopped")
        self.assertIsNone(guest.process)
        guest.stop()  # a guest that is stopped has nothing to stop or disconnect

    def test_a_launcher_that_will_not_stop_does_not_keep_the_others_running(self) -> None:
        stopped: list[str] = []

        class Stubborn:
            def stop(self) -> None:
                raise subprocess.TimeoutExpired(["launcher"], 30)

        class Fine:
            def stop(self) -> None:
                stopped.append("fine")

        with contextlib.redirect_stderr(io.StringIO()):
            runner.stop_all([Stubborn(), Fine()])  # type: ignore[list-item]
        self.assertEqual(stopped, ["fine"])


HEADER_DIRECTORY = Path(__file__).resolve().parents[3] / "android/lineage-24/overlay/tools"
DRIVER = r"""
#include <stdio.h>
#include "kernarium_wifi_mac.h"
int main() {
  char line[64];
  while (fgets(line, sizeof line, stdin)) {
    unsigned v[6];
    if (sscanf(line, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return 2;
    kernarium_net::Mac ethernet = {};
    for (int i = 0; i < 6; ++i) ethernet[i] = static_cast<unsigned char>(v[i]);
    const kernarium_net::Mac wifi = kernarium_net::DeriveWifiMac(ethernet);
    const kernarium_net::Mac back = kernarium_net::EthernetFromWifiMac(wifi);
    printf("%02x:%02x:%02x:%02x:%02x:%02x %02x:%02x:%02x:%02x:%02x:%02x\n",
           wifi[0], wifi[1], wifi[2], wifi[3], wifi[4], wifi[5],
           back[0], back[1], back[2], back[3], back[4], back[5]);
  }
  return 0;
}
"""


def reserved_addresses() -> list[int]:
    return [0x020000000000] + [0x52550A000200 | last for last in range(256)]


def is_host_address(mac: str) -> bool:
    """An address the host can give eth0: locally administered unicast and not reserved."""
    packed = int(mac.replace(":", ""), 16)
    return packed >> 40 & 0x03 == 0x02 and packed not in reserved_addresses()


def random_macs(count: int, seed: int) -> list[str]:
    rng = random.Random(seed)
    return [runner.format_mac(runner.mac_from_bits(rng.getrandbits(46))) for _ in range(count)]


class WifiDerivationTests(unittest.TestCase):
    def test_the_port_gives_the_golden_vectors_the_cpp_gives(self) -> None:
        vectors = runner.golden_vectors()
        self.assertGreaterEqual(len(vectors), 1000)
        for ethernet, wifi in vectors:
            self.assertEqual(runner.derive_wifi_mac(ethernet), wifi, ethernet)
            if is_host_address(ethernet):
                self.assertEqual(runner.ethernet_from_wifi_mac(wifi), ethernet, wifi)

    def test_every_result_is_a_local_unicast_address_that_is_not_refused(self) -> None:
        for ethernet in ["52:54:00:12:34:56", "00:00:00:00:00:00", ETHERNET, OTHER_ETHERNET,
                         "02:15:b2:00:00:00", "52:55:0a:00:02:02"] + random_macs(2000, 1):
            wifi = runner.derive_wifi_mac(ethernet)
            self.assertEqual(int(wifi[:2], 16) & 0x03, 0x02, ethernet)
            self.assertNotEqual(wifi, ethernet)
            self.assertNotIn(int(wifi.replace(":", ""), 16), reserved_addresses(), ethernet)

    def test_the_review_pair_gives_two_addresses(self) -> None:
        # A hash gave these two eth0 addresses one Wi-Fi address.
        self.assertNotEqual(runner.derive_wifi_mac("0a:11:22:33:44:56"), runner.derive_wifi_mac("62:a7:34:c8:24:f6"))

    def test_the_inverse_round_trips_and_the_permutation_is_one_to_one(self) -> None:
        for ethernet in random_macs(3000, 2):
            self.assertEqual(runner.ethernet_from_wifi_mac(runner.derive_wifi_mac(ethernet)), ethernet)
        rng = random.Random(3)
        for _ in range(3000):
            bits = rng.getrandbits(runner.WIFI_BITS)
            self.assertEqual(runner.wifi_unpermute(runner.wifi_permute(bits)), bits)
        self.assertEqual(len({runner.derive_wifi_mac(ethernet) for ethernet in random_macs(20000, 4)}), 20000)

    def test_a_reserved_address_is_walked_past_with_at_most_the_stated_bound(self) -> None:
        reserved = reserved_addresses()
        self.assertEqual(len(reserved), 257)
        self.assertEqual(runner.WIFI_MAX_STEPS, len(reserved) + 1)
        for packed in reserved:
            # The eth0 address whose first step lands on the reserved one.
            position = runner.wifi_permute(runner.mac_bits(runner.format_mac(packed)))
            ethernet = runner.format_mac(runner.mac_from_bits(
                runner.wifi_unpermute((position - 1) % runner.WIFI_VALUES)))
            wifi = int(runner.derive_wifi_mac(ethernet).replace(":", ""), 16)
            self.assertNotEqual(wifi, packed)
            self.assertNotIn(wifi, reserved)
            expected = next(candidate for step in range(2, runner.WIFI_MAX_STEPS + 1)
                            for candidate in [runner.mac_from_bits(
                                runner.wifi_unpermute((position - 1 + step) % runner.WIFI_VALUES))]
                            if candidate not in reserved)
            self.assertEqual(wifi, expected)

    def test_what_the_host_never_generates_has_the_documented_result(self) -> None:
        # The flag bits are ignored: the broadcast address gives the result of its local twin.
        self.assertEqual(runner.derive_wifi_mac("ff:ff:ff:ff:ff:ff"), runner.derive_wifi_mac("fe:ff:ff:ff:ff:ff"))
        self.assertEqual(runner.derive_wifi_mac("01:aa:bb:cc:dd:ee"), runner.derive_wifi_mac("02:aa:bb:cc:dd:ee"))
        # A reserved input gives the result of the unreserved address before it on the cycle.
        for packed in reserved_addresses()[:20]:
            before = runner.wifi_unpermute((runner.wifi_permute(runner.mac_bits(runner.format_mac(packed))) - 1)
                                           % runner.WIFI_VALUES)
            if runner.mac_from_bits(before) in reserved_addresses():
                continue
            self.assertEqual(runner.derive_wifi_mac(runner.format_mac(packed)),
                             runner.derive_wifi_mac(runner.format_mac(runner.mac_from_bits(before))))

    @unittest.skipUnless(shutil.which("c++"), "no C++ compiler")
    def test_a_differential_run_of_the_header_agrees_with_the_port(self) -> None:
        macs = random_macs(5000, 5) + [
            "00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff", "01:23:45:67:89:ab", "02:00:00:00:00:00",
            "52:55:0a:00:02:02", "52:55:0a:00:02:ff", "02:15:b2:00:00:00", "0a:11:22:33:44:56"]
        rng = random.Random(6)
        macs += [runner.format_mac(rng.getrandbits(48)) for _ in range(2000)]  # flag bits included
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "driver.cpp"
            binary = Path(directory) / "driver"
            source.write_text(DRIVER)
            subprocess.run(["c++", "-std=c++20", "-O1", f"-I{HEADER_DIRECTORY}", str(source), "-o", str(binary)],
                           check=True, capture_output=True)
            output = subprocess.run([str(binary)], input="\n".join(macs) + "\n", text=True, check=True,
                                    capture_output=True).stdout.splitlines()
        self.assertEqual(len(output), len(macs))
        for ethernet, line in zip(macs, output):
            wifi, back = line.split()
            self.assertEqual(wifi, runner.derive_wifi_mac(ethernet), ethernet)
            self.assertEqual(back, runner.ethernet_from_wifi_mac(wifi), ethernet)
            if is_host_address(ethernet):
                self.assertEqual(back, ethernet)


WLAN0 = (
    "10: wlan0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc noqueue state UP group default qlen 1000\n"
    "    link/ether {address} brd ff:ff:ff:ff:ff:ff\n"
    "{inet}")
INET = "    inet 10.0.2.16/24 brd 10.0.2.255 scope global wlan0\n"


class ImageDevice(FakeDevice):
    """A guest that carries the newer image: wlan0, the probe application and the kernel's command line."""

    def __init__(self, probes, wifi: str | None = None, inet: bool = True, key_visible: bool = False,
                 control_visible: bool = True, cmdline_has_key: bool = True, installed: bool = True,
                 install_works: bool = True) -> None:
        super().__init__()
        self.probes = probes
        self.wifi = wifi if wifi is not None else runner.derive_wifi_mac(ETHERNET)
        self.inet = inet
        self.key_visible = key_visible
        self.control_visible = control_visible
        self.cmdline_has_key = cmdline_has_key
        self.installed = installed
        self.install_works = install_works
        self.serial = "127.0.0.1:1"
        self.commands: list[str] = []

    def shell(self, command: str, timeout: int = 30) -> str:
        self.commands.append(command)
        package = runner.property_probe_apk.PACKAGE
        if command == "ip addr show wlan0":
            return WLAN0.format(address=self.wifi.upper(), inet=INET if self.inet else "").strip()
        if command == "ip -4 addr show dev wlan0":
            return INET if self.inet else "10: wlan0: <UP>"
        if command == "cat /proc/cmdline":
            key = f" androidboot.kernarium.adb_key={self.probes.key}" if self.cmdline_has_key else ""
            return "console=ttyS0" + key
        if command == f"pm path {package} || true":
            return f"package:/data/app/{package}/base.apk" if self.installed else ""
        if command == f"run-as {package} getprop {runner.CONTROL_PROPERTY}":
            return self.probes.control if self.control_visible else ""
        if command == f"run-as {package} getprop {runner.KEY_PROPERTY}":
            return self.probes.key if self.key_visible else ""
        return super().shell(command, timeout)

    def run(self, *arguments: str, timeout: int = 30) -> subprocess.CompletedProcess:
        self.commands.append(" ".join(arguments))
        return subprocess.CompletedProcess(arguments, 0 if self.install_works else 1,
                                           "Success\n" if self.install_works else "Failure\n")


class ImageInputTests(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.scratch = Path(scratch.name)
        patcher = mock.patch.object(runner.property_probe_apk, "build_apk", side_effect=lambda path: path)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.probes = runner.Probes(self.scratch, Path("/path with space/emulator"))

    def test_the_wrapper_adds_the_probe_arguments_after_the_launchers_own(self) -> None:
        text = self.probes.emulator.read_text()
        self.assertIn("exec '/path with space/emulator' \"$@\" ", text)
        self.assertIn(f"androidboot.kernarium.adb_key={self.probes.key} ", text)
        self.assertIn(f"androidboot.kernarium.identity_probe={self.probes.control}\n", text)
        self.assertTrue(os.access(self.probes.emulator, os.X_OK))
        other = runner.Probes(self.scratch, Path("/emulator"))
        self.assertNotEqual((other.key, other.control), (self.probes.key, self.probes.control))

    def test_a_guest_of_the_new_image_passes_and_gives_its_wifi_address(self) -> None:
        device = ImageDevice(self.probes)
        self.assertEqual(runner.check_image(device, ETHERNET, self.probes), runner.derive_wifi_mac(ETHERNET))

    def test_the_probe_application_is_installed_once(self) -> None:
        device = ImageDevice(self.probes, installed=False)
        runner.check_image(device, ETHERNET, self.probes)
        self.assertEqual(sum(1 for command in device.commands if command.startswith("-s ")), 1)
        device = ImageDevice(self.probes, installed=True)
        runner.check_image(device, ETHERNET, self.probes)
        self.assertEqual(sum(1 for command in device.commands if command.startswith("-s ")), 0)

    def test_every_difference_fails(self) -> None:
        cases = {
            "another wlan0 address": ImageDevice(self.probes, wifi="02:aa:bb:cc:dd:01"),
            "the old fixed wlan0 address": ImageDevice(self.probes, wifi=runner.LEGACY_WIFI_MAC),
            "eth0's own address on wlan0": ImageDevice(self.probes, wifi=ETHERNET),
            "no lease": ImageDevice(self.probes, inet=False),
            "an application reads the key": ImageDevice(self.probes, key_visible=True),
            "an application cannot read the control": ImageDevice(self.probes, control_visible=False),
            "the kernel never had the key": ImageDevice(self.probes, cmdline_has_key=False),
            "the probe will not install": ImageDevice(self.probes, installed=False, install_works=False),
        }
        for label, device in cases.items():
            with self.subTest(label), mock.patch.object(runner, "LEASE_SECONDS", 1), \
                    mock.patch.object(runner.time, "sleep", lambda seconds: None):
                with self.assertRaises(runner.Failure):
                    runner.check_image(device, ETHERNET, self.probes)

    def test_a_failure_names_no_address(self) -> None:
        device = ImageDevice(self.probes, wifi="02:aa:bb:cc:dd:01")
        with self.assertRaises(runner.Failure) as raised:
            runner.check_image(device, ETHERNET, self.probes)
        message = str(raised.exception)
        for value in (ETHERNET, "02:aa:bb:cc:dd:01", runner.derive_wifi_mac(ETHERNET), self.probes.key):
            self.assertNotIn(value, message)

    def test_a_release_gets_the_given_command_line_and_the_original_keeps_its_own(self) -> None:
        release = self.scratch / "source"
        (release / "boot").mkdir(parents=True)
        (release / "boot" / "kernel-command-line.txt").write_text("old serial=1\n")
        (release / "release-manifest.json").write_text("{}\n")
        line = self.scratch / "new-line.txt"
        line.write_text("new\n")
        clone = runner.prepared_release(release, line, self.scratch)
        self.assertEqual((clone / "boot" / "kernel-command-line.txt").read_text(), "new\n")
        self.assertEqual((release / "boot" / "kernel-command-line.txt").read_text(), "old serial=1\n")
        self.assertTrue((clone / "release-manifest.json").is_file())

    def test_a_command_line_needs_a_release(self) -> None:
        with mock.patch.object(sys, "argv", ["runner", "--command-line", "/x"]), \
                contextlib.redirect_stderr(io.StringIO()) as stderr:
            self.assertEqual(runner.main(), 2)
        self.assertIn("--release", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
