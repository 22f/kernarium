#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The UI automation host's `launch` keeps an iPhone silent for Andarium.

An automated run on the phone plays no haptics unless it tests them. A launch
of Andarium states `ANDARIUM_HAPTICS=0`; `--haptics` (the runners' name for a
run that tests haptics) states nothing, so the application's own Haptics
switch decides. Kernarium plays no haptics, and its launch is as it was.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
from pathlib import Path
import unittest


SCRIPT = Path(__file__).resolve().parents[3] / "Kernarium/scripts/ipad-automation.py"
SPEC = importlib.util.spec_from_file_location("ipad_automation", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
automation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(automation)

DEVICE = ["--device", "00000000-0000000000000000"]
ANDARIUM = ["--bundle", "com.imxieyi.andarium"]


def launch(*arguments: str, application: list[str] = ()) -> dict:
    return automation.build_command(
        automation.parse([*DEVICE, *application, "launch", *arguments]))


def refused_by_parser(*arguments: str) -> bool:
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            automation.parse([*DEVICE, *arguments])
    except SystemExit:
        return True
    return False


class LaunchHapticsTests(unittest.TestCase):
    def test_a_launch_of_andarium_silences_the_phone_by_default(self) -> None:
        self.assertEqual(
            launch(application=ANDARIUM)["environment"],
            {"ANDARIUM_HAPTICS": "0"})
        self.assertEqual(
            launch("--env", "ANDARIUM_START=normal", "--env", "A=b",
                   application=ANDARIUM)["environment"],
            {"ANDARIUM_START": "normal", "A": "b", "ANDARIUM_HAPTICS": "0"})

    def test_a_launch_that_tests_haptics_states_no_haptics_setting(
        self
    ) -> None:
        self.assertEqual(
            launch("--haptics", application=ANDARIUM)["environment"], {})
        self.assertEqual(
            launch("--haptics", "--env", "ANDARIUM_START=normal",
                   application=ANDARIUM)["environment"],
            {"ANDARIUM_START": "normal"})

    def test_a_launch_of_kernarium_is_as_it_was(self) -> None:
        # Kernarium has no vibrator and no haptics of its own to silence.
        self.assertEqual(launch()["environment"], {})
        self.assertEqual(
            launch("--env", "KERNARIUM_ROOTFS=none.erofs")["environment"],
            {"KERNARIUM_ROOTFS": "none.erofs"})
        self.assertEqual(launch("--haptics")["environment"], {})

    def test_the_haptics_setting_is_the_option_s_and_not_an_environment_pair(
        self
    ) -> None:
        for value in ("0", "1"):
            with self.subTest(value=value):
                self.assertTrue(refused_by_parser(
                    *ANDARIUM, "launch", "--env", f"ANDARIUM_HAPTICS={value}"))

    def test_only_a_launch_states_it(self) -> None:
        for name in ("activate", "terminate", "state", "tree", "ping"):
            with self.subTest(name=name):
                command = automation.build_command(
                    automation.parse([*DEVICE, *ANDARIUM, name]))
                self.assertNotIn("environment", command)


if __name__ == "__main__":
    unittest.main()
