#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The installation identity runner's reading of the guest and of the host's file.

The runner (tests/integration/android/run_android_installation_identity.py)
compares what the guest reports with `identity.json` beside the data disk. The
values here are synthetic: a test never holds a real installation's.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest


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


if __name__ == "__main__":
    unittest.main()
