#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The iPad automation host never lets Xcode record the device's screen.

A host runs for hours. With Xcode's defaults its test run recorded the
device's screen for the whole session into a result bundle under the internal
disk's DerivedData, and kept every attachment. The scheme that generates the
test run file turns both off, `start` refuses a run file that does not say so,
and the supervisor puts the result bundle under the state directory. Nothing
here talks to a device or starts xcodebuild.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ElementTree

REPOSITORY = Path(__file__).resolve().parents[3]
SCRIPT = REPOSITORY / "Kernarium/scripts/ipad-automation.py"
SCHEME = (REPOSITORY / "Kernarium/Kernarium.xcodeproj/xcshareddata"
          / "xcschemes/KernariumAutomation.xcscheme")
SPEC = importlib.util.spec_from_file_location("ipad_automation", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
automation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(automation)

DEVICE = "00000000-0000000000000000"
NONCE = "0123456789abcdef0123456789abcdef"
GOOD = {
    "PreferredScreenCaptureFormat": "screenshots",
    "SystemAttachmentLifetime": "keepNever",
    "UserAttachmentLifetime": "keepNever",
}


def target(**settings: str | None) -> dict[str, str]:
    """A test target of a run file; a None setting is left out."""
    entry = {"TestBundlePath": "__TESTHOST__/PlugIns/Host.xctest",
             **GOOD, **settings}
    return {key: value for key, value in entry.items() if value is not None}


def format_two(*targets: dict[str, str]) -> dict:
    return {"__xctestrun_metadata__": {"FormatVersion": 2},
            "TestConfigurations": [{"Name": "Test", "TestTargets": list(targets)}]}


def format_one(entry: dict[str, str]) -> dict:
    return {"__xctestrun_metadata__": {"FormatVersion": 1}, "Host": entry}


class FakeProcess:
    """What subprocess.Popen returns for a process that ended at once."""

    def __init__(self, calls: list, command: list[str], **keywords) -> None:
        self.pid = 4242
        calls.append((command, keywords))

    def poll(self) -> int:
        return 1

    def wait(self, timeout: float | None = None) -> int:
        return 0


class SchemeTests(unittest.TestCase):
    def test_the_scheme_that_generates_the_run_file_records_nothing(self) -> None:
        action = ElementTree.parse(SCHEME).getroot().find("TestAction")
        assert action is not None
        self.assertEqual(action.get("preferredScreenCaptureFormat"), "screenshots")
        self.assertEqual(action.get("systemAttachmentLifetime"), "keepNever")
        self.assertEqual(action.get("userAttachmentLifetime"), "keepNever")


class RecordingProblemTests(unittest.TestCase):
    def setUp(self) -> None:
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)

    def problems(self, plan: dict) -> list[str]:
        path = Path(self.scratch.name) / "Host.xctestrun"
        path.write_bytes(plistlib.dumps(plan))
        return automation.recording_problems(path)

    def test_both_file_formats_with_the_settings_pass(self) -> None:
        self.assertEqual(self.problems(format_two(target())), [])
        self.assertEqual(self.problems(format_one(target())), [])

    def test_xcodes_defaults_are_refused_in_both_formats(self) -> None:
        defaults = target(PreferredScreenCaptureFormat="screenRecording",
                          SystemAttachmentLifetime="deleteOnSuccess",
                          UserAttachmentLifetime="deleteOnSuccess")
        for plan in (format_two(defaults), format_one(defaults)):
            problems = self.problems(plan)
            self.assertEqual(len(problems), 3, problems)
            self.assertIn("PreferredScreenCaptureFormat is 'screenRecording'",
                          " ".join(problems))

    def test_each_setting_is_required_on_its_own(self) -> None:
        for key, value in (
            ("PreferredScreenCaptureFormat", "screenRecording"),
            ("SystemAttachmentLifetime", "keepAlways"),
            ("UserAttachmentLifetime", "deleteOnSuccess"),
            ("PreferredScreenCaptureFormat", None),
            ("SystemAttachmentLifetime", None),
            ("UserAttachmentLifetime", None),
        ):
            with self.subTest(key=key, value=value):
                problems = self.problems(format_two(target(**{key: value})))
                self.assertEqual(len(problems), 1, problems)
                self.assertTrue(problems[0].startswith(key), problems)

    def test_every_target_must_pass(self) -> None:
        problems = self.problems(format_two(
            target(), target(SystemAttachmentLifetime="deleteOnSuccess")))
        self.assertEqual(len(problems), 1, problems)

    def test_a_file_without_a_test_target_or_unreadable_is_refused(self) -> None:
        self.assertIn("names no test target", self.problems({"a": [{"b": 1}]})[0])
        broken = Path(self.scratch.name) / "Broken.xctestrun"
        broken.write_text("not a property list")
        self.assertIn("cannot be read", automation.recording_problems(broken)[0])
        missing = Path(self.scratch.name) / "Missing.xctestrun"
        self.assertIn("cannot be read", automation.recording_problems(missing)[0])


class StartAndSuperviseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.state = Path(self.scratch.name) / "state"
        self.derived = Path(self.scratch.name) / "derived"
        (self.derived / "Build/Products").mkdir(parents=True)
        saved = automation.host_state
        automation.host_state = lambda options: None
        self.addCleanup(setattr, automation, "host_state", saved)

    def options(self, *command: str):
        return automation.parse(
            ["--device", DEVICE, "--state-dir", str(self.state),
             "--derived-data", str(self.derived), *command])

    def run_file(self, plan: dict) -> Path:
        path = self.derived / "Build/Products/KernariumAutomation_iphoneos27.0.xctestrun"
        path.write_bytes(plistlib.dumps(plan))
        return path

    def start(self) -> tuple[int, str, list]:
        calls: list = []
        errors = io.StringIO()
        with mock.patch.object(
            automation.subprocess, "Popen",
            lambda command, **keywords: FakeProcess(calls, command, **keywords),
        ), mock.patch.object(
            automation.ios_device_reports, "preflight"
        ), contextlib.redirect_stderr(errors):
            status = automation.command_start(self.options("start", "--wait", "1"))
        return status, errors.getvalue(), calls

    def test_start_refuses_a_run_file_that_records_the_screen(self) -> None:
        self.run_file(format_two(target(
            PreferredScreenCaptureFormat="screenRecording",
            SystemAttachmentLifetime="deleteOnSuccess",
            UserAttachmentLifetime="deleteOnSuccess")))
        status, errors, calls = self.start()
        self.assertEqual(status, automation.EXIT_FAILED)
        self.assertEqual(calls, [], "no supervisor and no xcodebuild may start")
        self.assertIn("screenRecording", errors)
        self.assertIn("run `build` again", errors)
        self.assertFalse((self.state / DEVICE / "host.json").exists())

    def test_start_refuses_a_run_file_that_keeps_attachments(self) -> None:
        self.run_file(format_one(target(UserAttachmentLifetime="deleteOnSuccess")))
        status, errors, calls = self.start()
        self.assertEqual(status, automation.EXIT_FAILED)
        self.assertEqual(calls, [])
        self.assertIn("UserAttachmentLifetime", errors)

    def test_start_judges_the_run_file_it_would_launch(self) -> None:
        older = self.derived / "Build/Products/KernariumAutomation_a.xctestrun"
        older.write_bytes(plistlib.dumps(format_two(target(
            PreferredScreenCaptureFormat="screenRecording"))))
        launched = self.derived / "Build/Products/KernariumAutomation_b.xctestrun"
        launched.write_bytes(plistlib.dumps(format_two(target())))
        _, _, calls = self.start()
        self.assertEqual(len(calls), 1)
        self.assertIn(str(launched), calls[0][0])

    def test_the_result_bundle_lives_next_to_the_host_log(self) -> None:
        self.run_file(format_two(target()))
        status, _, calls = self.start()
        self.assertEqual(status, automation.EXIT_NO_HOST)  # the fake ended at once
        self.assertEqual(len(calls), 1)
        command = calls[0][0]
        bundle = Path(command[command.index("--result-bundle") + 1])
        record = json.loads((self.state / DEVICE / "host.json").read_text())
        log = Path(record["log"])
        self.assertEqual(bundle.parent, self.state / DEVICE)
        self.assertEqual(bundle.parent, log.parent)
        self.assertEqual(bundle.suffix, ".xcresult")
        self.assertEqual(bundle.stem, log.stem)
        self.assertEqual(record["result_bundle"], str(bundle))
        self.assertFalse(bundle.exists(), "xcodebuild refuses an existing bundle")

    def test_start_makes_room_for_reports_before_it_starts_anything(self) -> None:
        self.run_file(format_two(target()))
        order: list[str] = []

        def popen(command, **keywords):
            order.append("supervisor")
            return FakeProcess([], command, **keywords)

        with mock.patch.object(automation.subprocess, "Popen", popen), \
                mock.patch.object(
                    automation.ios_device_reports, "preflight",
                    side_effect=lambda *a, **k: order.append("reports")
                ) as preflight, contextlib.redirect_stderr(io.StringIO()):
            automation.command_start(self.options("start", "--wait", "1"))
        self.assertEqual(order, ["reports", "supervisor"])
        options, applications, directory = preflight.call_args.args
        self.assertEqual(options.device, DEVICE)
        self.assertEqual(directory, self.state / DEVICE)
        self.assertIn("KernariumAutomation-Runner",
                      preflight.call_args.kwargs["extra_processes"])

    def test_start_refuses_when_the_reports_cannot_be_made_room_for(self) -> None:
        self.run_file(format_two(target()))
        calls: list = []
        errors = io.StringIO()
        with mock.patch.object(
            automation.subprocess, "Popen",
            lambda command, **keywords: FakeProcess(calls, command, **keywords),
        ), mock.patch.object(
            automation.ios_device_reports, "preflight",
            side_effect=automation.ios_device_reports.DeviceError(
                "still short of 5 free slots"),
        ), contextlib.redirect_stderr(errors):
            status = automation.command_start(self.options("start", "--wait", "1"))
        self.assertEqual(status, automation.EXIT_FAILED)
        self.assertEqual(calls, [])
        self.assertIn("still short of 5 free slots", errors.getvalue())
        self.assertFalse((self.state / DEVICE / "host.json").exists())

    def test_the_supervisor_gives_xcodebuild_that_result_bundle(self) -> None:
        bundle = self.state / DEVICE / "host-20260101-000000.xcresult"
        options = self.options(
            "_supervise", "--xctestrun", "Host.xctestrun",
            "--result-bundle", str(bundle), "--seconds", "5", "--nonce", NONCE)
        calls: list = []
        output = io.StringIO()
        with mock.patch.object(
            automation.subprocess, "Popen",
            lambda command, **keywords: FakeProcess(calls, command, **keywords),
        ), contextlib.redirect_stdout(output):
            self.assertEqual(automation.command_supervise(options), 0)
        (command, _), = calls
        self.assertEqual(command[:2], ["xcodebuild", "test-without-building"])
        self.assertEqual(
            command[command.index("-resultBundlePath") + 1], str(bundle))
        self.assertEqual(command.count("-resultBundlePath"), 1)

    def test_a_supervisor_needs_a_result_bundle(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()), \
                self.assertRaises(SystemExit):
            self.options("_supervise", "--xctestrun", "Host.xctestrun",
                         "--seconds", "5", "--nonce", NONCE)


if __name__ == "__main__":
    unittest.main()
