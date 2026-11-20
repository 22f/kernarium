#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Andarium/scripts/check-release-archive.py holds a Release archive to what any archive needs.

Each case builds a fixture application bundle, changes one thing and runs the
script on it. The Release archive's build phase runs the script; Debug builds,
the Preview and an ordinary Release build do not, so a fixture with the example
handle stays usable everywhere else.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import plistlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[3] / "Andarium" / "scripts" / "check-release-archive.py"
HANDLE_KEY = "AndariumFollowXHandle"


def run(app: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(SCRIPT), str(app)], check=False, stdin=subprocess.DEVNULL,
        capture_output=True, text=True, timeout=120)


class Fixtures(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.count = 0

    def app(self, **info: object) -> Path:
        """A bundle with an Info.plist of `info`; a value of None leaves the key out."""
        self.count += 1
        app = self.root / f"fixture-{self.count}" / "Andarium.app"
        app.mkdir(parents=True)
        keys = {"CFBundleIdentifier": "org.example.Fixture", **info}
        (app / "Info.plist").write_bytes(plistlib.dumps({k: v for k, v in keys.items() if v is not None}))
        return app

    def passes(self, app: Path) -> None:
        done = run(app)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def refuses(self, app: Path, expect: str) -> None:
        done = run(app)
        self.assertNotEqual(done.returncode, 0, done.stdout)
        self.assertIn(expect, done.stderr)
        self.assertNotIn("App Store", done.stdout + done.stderr)


class FollowXHandle(Fixtures):
    """The profile link the row opens is the developer's own, or there is none."""

    def test_a_bundle_with_no_handle_has_no_link(self) -> None:
        self.passes(self.app())

    def test_an_empty_handle_has_no_link(self) -> None:
        self.passes(self.app(**{HANDLE_KEY: ""}))

    def test_a_well_formed_handle_passes(self) -> None:
        for handle in ("real_handle", "a", "A1_b2", "abcdefghijklmno", "_underscore_"):
            with self.subTest(handle=handle):
                self.passes(self.app(**{HANDLE_KEY: handle}))

    def test_a_malformed_handle_is_refused(self) -> None:
        for handle in ("abcdefghijklmnop", "with.dot", "with space", "-dash", "x/y", "@name", "héllo", " ", "a\nb"):
            with self.subTest(handle=handle):
                self.refuses(self.app(**{HANDLE_KEY: handle}), HANDLE_KEY)

    def test_a_handle_that_is_not_a_string_is_refused(self) -> None:
        for value in (7, True, ["real_handle"]):
            with self.subTest(value=value):
                self.refuses(self.app(**{HANDLE_KEY: value}), HANDLE_KEY)

    def test_the_example_handle_is_refused_in_every_case(self) -> None:
        # What Developer.local.xcconfig.example names: copied unchanged, it would ship a link to it.
        for handle in ("example_handle", "EXAMPLE_HANDLE", "Example_Handle"):
            with self.subTest(handle=handle):
                self.refuses(self.app(**{HANDLE_KEY: handle}), "example")

    def test_a_bundle_without_an_info_plist_is_refused(self) -> None:
        app = self.root / "bare" / "Andarium.app"
        app.mkdir(parents=True)
        self.refuses(app, "Info.plist")


if __name__ == "__main__":
    unittest.main(verbosity=2)
