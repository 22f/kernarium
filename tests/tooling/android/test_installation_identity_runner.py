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

    def getprop(self, name: str) -> str:
        return self.properties[name]

    def shell(self, command: str, timeout: int = 30) -> str:
        if command == "ip addr show eth0":
            return listing(self.address).strip()
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


if __name__ == "__main__":
    unittest.main()
