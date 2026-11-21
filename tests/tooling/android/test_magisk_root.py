#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Magisk root files of an Android release.

A release can carry, beside its system disk, a copy of its own init_boot that
Magisk's patcher made and the official Magisk APK, both from the one release
pinned in android/lineage-24/metadata/magisk.env. These tests use a synthetic
cpio, a synthetic APK and stand-in tools: no Magisk file is in the repository.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PIN_KEYS = {
    "LINEAGE24_MAGISK_SCHEMA",
    "LINEAGE24_MAGISK_NAME",
    "LINEAGE24_MAGISK_VERSION",
    "LINEAGE24_MAGISK_VERSION_CODE",
    "LINEAGE24_MAGISK_APK_SHA256",
    "LINEAGE24_MAGISK_APK_SIZE",
}


class PinTests(unittest.TestCase):
    """The tracked pin names the release and its digest, and nothing about where a copy came from."""

    def setUp(self) -> None:
        self.path = ROOT / "android/lineage-24/metadata/magisk.env"
        self.pin = contract_checks.parse_assignments(self.path)

    def test_the_pin_has_exactly_its_fields(self) -> None:
        self.assertEqual(set(self.pin), PIN_KEYS)
        self.assertEqual(self.pin["LINEAGE24_MAGISK_SCHEMA"], "1")
        self.assertEqual(self.pin["LINEAGE24_MAGISK_NAME"], "Magisk")

    def test_the_version_and_digest_are_well_formed(self) -> None:
        self.assertRegex(self.pin["LINEAGE24_MAGISK_VERSION"], r"^v[0-9]+\.[0-9]+(\.[0-9]+)?$")
        self.assertGreater(int(self.pin["LINEAGE24_MAGISK_VERSION_CODE"]), 0)
        self.assertRegex(self.pin["LINEAGE24_MAGISK_APK_SHA256"], r"^[0-9a-f]{64}$")
        self.assertGreater(int(self.pin["LINEAGE24_MAGISK_APK_SIZE"]), 0)

    def test_the_pin_records_no_source_location(self) -> None:
        text = self.path.read_text(encoding="utf-8")
        self.assertIsNone(re.search(r"https?:|git@|\.com|\.org|/Users/|/home/|/Volumes/", text))

    def test_the_pin_is_a_release_input(self) -> None:
        rows = release_inputs.read_rows(ROOT / release_inputs.LIST_RELATIVE)
        self.assertIn(
            ("input", "android/lineage-24/metadata/magisk.env"),
            [(kind, path) for kind, path, _ in rows],
        )


def main() -> None:
    global contract_checks, release_inputs
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=ROOT)
    arguments, rest = parser.parse_known_args()
    globals()["ROOT"] = arguments.source_root.resolve()
    sys.path.insert(0, str(ROOT / "kernel/common/artifacts"))
    sys.path.insert(0, str(ROOT / "android/lineage-24/tools"))
    import contract_checks as checks  # noqa: PLC0415
    import release_inputs as inputs  # noqa: PLC0415

    contract_checks, release_inputs = checks, inputs
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)


if __name__ == "__main__":
    main()
