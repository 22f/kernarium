#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Every device run starts with free diagnostic-report slots.

iOS keeps a capped number of reports per kind and drops the next one when a
kind is full, so a process kill during a run can leave no evidence. The shared
preflight (tests/integration/support/ios_device_reports.py) counts the
JetsamEvent reports and the crash reports of the processes under test and
moves a short kind off the device; these tests run it against fake listers and
the fake devicectl and idevicecrashreport of fake_device_tools.py, and check
by source structure that every device runner calls it before it touches the
device. Report names here are invented, and no report has contents.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import ast
import contextlib
import io
import json
import os
import pathlib
import re
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import ios_device  # noqa: E402
import ios_device_reports as reports  # noqa: E402

DEVICE = "fake-device"
PROCESSES = ("Kernarium", "Andarium")


def invent(kind: str, count: int) -> list[str]:
    """Synthetic report names of one kind."""
    return [f"{kind}-2026-01-{day + 1:02d}-0{day:05d}.ips"
            for day in range(count)]


class FakeStore:
    """A lister and mover that hold nothing but names."""

    def __init__(self, names: list[str], *, removes: bool = True,
                 listing_fails: bool = False) -> None:
        self.held = list(names)
        self.removes = removes
        self.listing_fails = listing_fails
        self.moves: list[tuple[list[str], pathlib.Path]] = []

    def names(self) -> list[str]:
        if self.listing_fails:
            raise ios_device.DeviceError("cannot list the device's reports")
        return list(self.held)

    def move(self, kinds, destination: pathlib.Path) -> None:
        self.moves.append((list(kinds), destination))
        if self.removes:
            self.held = [name for name in self.held
                         if reports.report_name(name) not in kinds]


class CountingTests(unittest.TestCase):
    def test_a_report_counts_under_its_name_without_the_date(self) -> None:
        for file_name, kind in (
            ("JetsamEvent-2026-01-02-030405.ips", "JetsamEvent"),
            ("Orchid-2026-01-02-030405.ips", "Orchid"),
            ("Orchid-2026-01-02-030405.1.ips", "Orchid"),
            ("Orchid.cpu_resource-2026-01-02-030405.ips", "Orchid.cpu_resource"),
            ("gpuEvent-Orchid-2026-01-02-030405.00015.ips", "gpuEvent-Orchid"),
            ("Quartz-Runner-2026-01-02-030405.ips", "Quartz-Runner"),
            ("Orchid-2026-01-02-030405.ips.synced", "Orchid"),
        ):
            with self.subTest(file_name=file_name):
                self.assertEqual(reports.report_name(file_name), kind)

    def test_a_file_that_is_no_report_counts_nowhere(self) -> None:
        for file_name in ("heartbeat.plist", "panic-full", "Orchid.txt",
                          "Orchid-2026-01-02.ips", ".unknown"):
            with self.subTest(file_name=file_name):
                self.assertIsNone(reports.report_name(file_name))

    def test_only_the_kinds_a_run_needs_are_counted(self) -> None:
        held = (invent("JetsamEvent", 3) + invent("Kernarium", 2)
                + invent("Orchid", 9) + invent("Kernarium.cpu_resource", 7)
                + ["heartbeat.plist"])
        self.assertEqual(
            reports.count_reports(held, PROCESSES),
            {"JetsamEvent": 3, "Kernarium": 2, "Andarium": 0})


class SlotTests(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.destination = pathlib.Path(scratch.name) / "moved"
        self.lines: list[str] = []

    def ensure(self, store: FakeStore, **keywords) -> reports.ReportSlots:
        return reports.ensure_report_slots(
            store, PROCESSES, self.destination, emit=self.lines.append,
            **keywords)

    def test_an_empty_store_moves_nothing(self) -> None:
        store = FakeStore([])
        slots = self.ensure(store)
        self.assertEqual(store.moves, [])
        self.assertEqual(slots.short, [])
        self.assertEqual(len(self.lines), 1)
        self.assertIn("JetsamEvent 0 -> 0", self.lines[0])
        self.assertIn("Kernarium crash 0 -> 0", self.lines[0])
        self.assertIn("nothing to move", self.lines[0])
        self.assertFalse(self.destination.exists())

    def test_exactly_five_free_slots_is_enough_and_four_is_not(self) -> None:
        enough = reports.REPORT_CAP - reports.FREE_SLOTS
        for kind in ("JetsamEvent", "Kernarium"):
            with self.subTest(kind=kind, held=enough):
                store = FakeStore(invent(kind, enough))
                self.assertEqual(self.ensure(store).short, [])
                self.assertEqual(store.moves, [])
            with self.subTest(kind=kind, held=enough + 1):
                store = FakeStore(invent(kind, enough + 1))
                slots = self.ensure(store)
                self.assertEqual(slots.short, [kind])
                self.assertEqual(store.moves, [([kind], self.destination)])
                self.assertEqual(slots.after[kind], 0)

    def test_a_short_jetsam_kind_moves_only_the_jetsam_reports(self) -> None:
        store = FakeStore(invent("JetsamEvent", 25) + invent("Kernarium", 4)
                          + invent("Orchid", 25))
        slots = self.ensure(store)
        self.assertEqual(store.moves, [(["JetsamEvent"], self.destination)])
        self.assertEqual(slots.moved, 25)
        self.assertEqual(
            sorted(store.held),
            sorted(invent("Kernarium", 4) + invent("Orchid", 25)))
        self.assertIn("JetsamEvent 25 -> 0, Kernarium crash 4 -> 4",
                      self.lines[0])
        self.assertIn(f"moved 25 to {self.destination}", self.lines[0])

    def test_a_short_crash_kind_moves_only_that_process(self) -> None:
        store = FakeStore(invent("JetsamEvent", 2) + invent("Kernarium", 3)
                          + invent("Andarium", 23))
        slots = self.ensure(store)
        self.assertEqual(store.moves, [(["Andarium"], self.destination)])
        self.assertEqual(slots.after["Andarium"], 0)
        self.assertEqual(slots.after["JetsamEvent"], 2)
        self.assertIn("Andarium crash 23 -> 0", self.lines[0])

    def test_both_kinds_short_move_both(self) -> None:
        store = FakeStore(invent("JetsamEvent", 25) + invent("Kernarium", 21))
        slots = self.ensure(store)
        self.assertEqual(sorted(slots.short), ["JetsamEvent", "Kernarium"])
        self.assertEqual(slots.moved, 46)
        self.assertEqual(reports.short_kinds(slots.after), [])

    def test_a_removal_that_frees_too_little_fails_the_run(self) -> None:
        store = FakeStore(invent("JetsamEvent", 25), removes=False)
        with self.assertRaisesRegex(
            ios_device.DeviceError,
            r"still short of 5 free slots for JetsamEvent"
        ):
            self.ensure(store)
        self.assertEqual(len(self.lines), 1)
        self.assertIn("JetsamEvent 25 -> 25", self.lines[0])

    def test_a_tool_that_fails_still_reports_the_counts(self) -> None:
        class Failing(FakeStore):
            def move(self, kinds, destination) -> None:
                raise ios_device.DeviceError("the tool failed")
        with self.assertRaisesRegex(ios_device.DeviceError, "the tool failed"):
            self.ensure(Failing(invent("JetsamEvent", 25)))
        self.assertIn("could not move them", self.lines[0])

    def test_a_listing_that_fails_fails_the_run(self) -> None:
        with self.assertRaisesRegex(ios_device.DeviceError, "cannot list"):
            self.ensure(FakeStore([], listing_fails=True))
        self.assertEqual(self.lines, [])

    def test_leaving_the_reports_counts_but_never_moves_or_fails(self) -> None:
        store = FakeStore(invent("JetsamEvent", 25))
        slots = self.ensure(store, leave=True)
        self.assertEqual(store.moves, [])
        self.assertEqual(slots.short, ["JetsamEvent"])
        self.assertIn("left in place, short: JetsamEvent", self.lines[0])
        self.lines.clear()
        self.ensure(FakeStore([], listing_fails=True), leave=True)
        self.assertIn("not counted", self.lines[0])

    def test_a_dry_run_reports_what_it_would_move_and_moves_nothing(self) -> None:
        store = FakeStore(invent("JetsamEvent", 25) + invent("Kernarium", 21))
        slots = self.ensure(store, dry_run=True)
        self.assertEqual(store.moves, [])
        self.assertEqual(len(store.held), 46)
        self.assertEqual(sorted(slots.short), ["JetsamEvent", "Kernarium"])
        self.assertIn("would move 46", self.lines[0])

    def test_no_report_name_is_ever_printed(self) -> None:
        names = invent("JetsamEvent", 25) + invent("Kernarium", 6)
        self.ensure(FakeStore(names))
        with self.assertRaises(ios_device.DeviceError):
            self.ensure(FakeStore(names, removes=False))
        for line in self.lines:
            self.assertNotRegex(line, r"\d{4}-\d{2}-\d{2}")
            self.assertNotIn(".ips", line)


class DirectoryTests(unittest.TestCase):
    def ensure(self, destination: pathlib.Path) -> None:
        reports.ensure_report_slots(
            FakeStore([]), PROCESSES, destination, emit=lambda line: None)

    def test_a_directory_inside_the_repository_is_refused(self) -> None:
        for destination in (REPOSITORY / "device-reports",
                            REPOSITORY / "tests" / "device-reports" / "x"):
            with self.subTest(destination=destination):
                with self.assertRaisesRegex(
                    ios_device.DeviceError, "inside the repository"
                ):
                    self.ensure(destination)

    def test_it_is_refused_even_when_no_report_needs_moving(self) -> None:
        with self.assertRaises(ios_device.DeviceError):
            self.ensure(REPOSITORY / "device-reports")

    def test_a_dot_dot_path_into_the_repository_is_refused(self) -> None:
        sideways = REPOSITORY / ".." / REPOSITORY.name / "tests" / "x"
        with self.assertRaises(ios_device.DeviceError):
            self.ensure(sideways)
        with tempfile.TemporaryDirectory() as name:
            # The temporary directory is outside the repository.
            self.ensure(pathlib.Path(name) / "reports")

    def test_a_directory_git_ignores_is_accepted(self) -> None:
        build = REPOSITORY / ".build" / "device-reports" / "x"
        confirmed = subprocess.run(
            ["git", "-C", str(REPOSITORY), "check-ignore", "-q", "--",
             ".build/device-reports/x"], stdin=subprocess.DEVNULL,
            capture_output=True).returncode == 0
        if not confirmed:
            self.skipTest("git does not confirm that .build is ignored here")
        self.ensure(build)

    def test_a_directory_is_not_needed_to_leave_or_to_dry_run(self) -> None:
        for keywords in ({"leave": True}, {"dry_run": True}):
            with self.subTest(**keywords):
                reports.ensure_report_slots(
                    FakeStore([]), PROCESSES, None, emit=lambda line: None,
                    **keywords)
        with self.assertRaisesRegex(ios_device.DeviceError, "no directory"):
            reports.ensure_report_slots(
                FakeStore([]), PROCESSES, None, emit=lambda line: None)


class FakeTools:
    """The fake devicectl and idevicecrashreport on a PATH of their own."""

    def __init__(self, root: pathlib.Path, names: list[str], *,
                 with_move_tool: bool = True) -> None:
        self.state = root / "state"
        self.state.mkdir()
        (self.state / "crash-logs.txt").write_text("\n".join(names) + "\n")
        self.bin = root / "bin"
        self.bin.mkdir()
        tools = ["xcrun"] + (["idevicecrashreport"] if with_move_tool else [])
        for tool in tools:
            wrapper = self.bin / tool
            wrapper.write_text(
                f"#!/bin/sh\nexec {sys.executable} -S -B "
                f"{HERE / 'fake_device_tools.py'} {tool} \"$@\"\n")
            wrapper.chmod(wrapper.stat().st_mode | stat.S_IXUSR)

    def environment(self, **extra: str):
        return mock.patch.dict(os.environ, {
            "PATH": str(self.bin), "FAKE_STATE": str(self.state), **extra})

    def held(self) -> list[str]:
        return (self.state / "crash-logs.txt").read_text().split()

    def commands(self) -> list[dict]:
        log = self.state / "commands.jsonl"
        return [json.loads(line) for line in log.read_text().splitlines()] \
            if log.exists() else []


class DeviceStoreTests(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = pathlib.Path(scratch.name)

    def test_the_listing_is_read_from_devicectl_and_every_depth_counts(self) -> None:
        tools = FakeTools(self.root, invent("JetsamEvent", 3)
                          + invent("Kernarium", 2))
        with tools.environment():
            names = reports.DeviceStore(DEVICE).names()
        self.assertEqual(len(names), 5)
        (command,) = tools.commands()
        self.assertIn("systemCrashLogs", command["args"])
        self.assertIn(DEVICE, command["args"])

    def test_a_listing_that_fails_names_no_report(self) -> None:
        tools = FakeTools(self.root, invent("JetsamEvent", 3))
        with tools.environment(FAKE_REPORT_LIST="fail"), \
                self.assertRaises(ios_device.DeviceError) as caught:
            reports.DeviceStore(DEVICE).names()
        self.assertIn("devicectl exited 1", str(caught.exception))
        self.assertNotIn("JetsamEvent", str(caught.exception))

    def test_a_move_runs_the_tool_for_the_device_and_each_kind(self) -> None:
        tools = FakeTools(self.root, invent("JetsamEvent", 3)
                          + invent("Kernarium", 2) + invent("Orchid", 4))
        destination = self.root / "moved"
        with tools.environment():
            reports.DeviceStore(DEVICE).move(
                ["JetsamEvent", "Kernarium"], destination)
        moves = [c["args"] for c in tools.commands()
                 if c["tool"] == "idevicecrashreport"]
        self.assertEqual(moves, [
            ["-u", DEVICE, "-f", "JetsamEvent-", str(destination)],
            ["-u", DEVICE, "-f", "Kernarium-", str(destination)],
        ])
        self.assertEqual(len(list(destination.iterdir())), 5)
        self.assertEqual(sorted(tools.held()), sorted(invent("Orchid", 4)))

    def test_a_missing_tool_is_fatal_only_when_a_kind_is_short(self) -> None:
        for held, short in ((20, False), (21, True)):
            with self.subTest(held=held):
                root = self.root / str(held)
                root.mkdir()
                tools = FakeTools(root, invent("JetsamEvent", held),
                                  with_move_tool=False)
                with tools.environment():
                    preflight = lambda: reports.ensure_report_slots(  # noqa: E731
                        reports.DeviceStore(DEVICE), PROCESSES,
                        root / "moved", emit=lambda line: None)
                    if short:
                        with self.assertRaisesRegex(
                            ios_device.DeviceError,
                            r"idevicecrashreport is not on PATH.*"
                            r"install libimobiledevice"
                        ):
                            preflight()
                    else:
                        self.assertEqual(preflight().short, [])
                self.assertEqual(
                    [c["tool"] for c in tools.commands()], ["devicectl"])

    def test_a_tool_that_removes_nothing_fails_the_run(self) -> None:
        tools = FakeTools(self.root, invent("JetsamEvent", 25))
        with tools.environment(FAKE_REPORT_MOVE="keep"), \
                self.assertRaisesRegex(ios_device.DeviceError, "still short"), \
                contextlib.redirect_stdout(io.StringIO()):
            reports.ensure_report_slots(
                reports.DeviceStore(DEVICE), PROCESSES, self.root / "moved")


class PreflightTests(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = pathlib.Path(scratch.name)

    def options(self, *arguments: str) -> argparse.Namespace:
        parser = argparse.ArgumentParser()
        parser.add_argument("--device")
        reports.add_arguments(parser)
        return parser.parse_args(["--device", DEVICE, *arguments])

    def run_preflight(self, tools: FakeTools, options: argparse.Namespace,
                      applications=(), extra=()) -> str:
        output = io.StringIO()
        with tools.environment(), contextlib.redirect_stdout(output):
            reports.preflight(options, applications, self.root / "run",
                              extra_processes=extra)
        return output.getvalue()

    def test_reports_go_to_a_timestamped_folder_in_the_run_directory(self) -> None:
        tools = FakeTools(self.root, invent("JetsamEvent", 22))
        text = self.run_preflight(tools, self.options(), [ios_device.Application(
            "com.example.Orchid", ("ORCHID_",), re.compile("(x)"), "Orchid")])
        (folder,) = (self.root / "run" / "device-reports").iterdir()
        self.assertRegex(folder.name, r"^\d{8}-\d{6}$")
        self.assertEqual(len(list(folder.iterdir())), 22)
        self.assertIn("Orchid crash 0 -> 0", text)
        self.assertIn("JetsamEvent 22 -> 0", text)

    def test_the_flag_names_the_folder_and_the_environment_is_its_default(self) -> None:
        named = self.root / "elsewhere"
        tools = FakeTools(self.root, invent("JetsamEvent", 22))
        self.run_preflight(tools, self.options("--device-reports-dir", str(named)))
        self.assertEqual(len(list(named.iterdir())), 22)
        with mock.patch.dict(os.environ, {reports.DIRECTORY_VARIABLE: "/x/y"}):
            self.assertEqual(self.options().device_reports_dir, pathlib.Path("/x/y"))
            self.assertEqual(
                self.options("--device-reports-dir", "/z").device_reports_dir,
                pathlib.Path("/z"))
        with mock.patch.dict(os.environ):
            os.environ.pop(reports.DIRECTORY_VARIABLE, None)
            self.assertIsNone(self.options().device_reports_dir)

    def test_the_opt_out_leaves_every_report_in_place(self) -> None:
        tools = FakeTools(self.root, invent("JetsamEvent", 25))
        text = self.run_preflight(tools, self.options("--leave-device-reports"))
        self.assertEqual(len(tools.held()), 25)
        self.assertIn("left in place", text)
        self.assertEqual(
            [c["tool"] for c in tools.commands()], ["devicectl"])

    def test_the_processes_come_from_the_applications_and_the_extras(self) -> None:
        orchid = ios_device.Application(
            "com.example.Orchid", ("ORCHID_",), re.compile("(x)"), "Orchid")
        self.assertEqual(
            reports.report_processes([orchid, orchid], ["Quartz", "Orchid"]),
            ["Orchid", "Quartz"])


class CommandLineTests(unittest.TestCase):
    def run_main(self, store: FakeStore, *arguments: str) -> tuple[int, str]:
        output = io.StringIO()
        with mock.patch.object(reports, "DeviceStore", lambda device: store), \
                contextlib.redirect_stdout(output), \
                contextlib.redirect_stderr(output):
            return reports.main(["--device", DEVICE, *arguments]), output.getvalue()

    def test_a_dry_run_exits_3_for_a_short_kind_and_0_otherwise(self) -> None:
        status, text = self.run_main(
            FakeStore(invent("Kernarium", 25)), "--dry-run")
        self.assertEqual(status, 3)
        self.assertIn("would move 25", text)
        status, _ = self.run_main(FakeStore(invent("Kernarium", 3)), "--dry-run")
        self.assertEqual(status, 0)

    def test_a_move_needs_a_directory_named(self) -> None:
        with mock.patch.dict(os.environ):
            os.environ.pop(reports.DIRECTORY_VARIABLE, None)
            with self.assertRaises(SystemExit):
                self.run_main(FakeStore([]))

    def test_a_move_that_fails_exits_1(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            status, text = self.run_main(
                FakeStore(invent("JetsamEvent", 25), removes=False),
                "--device-reports-dir", name)
        self.assertEqual(status, 1)
        self.assertIn("still short", text)


# --- every device runner calls the preflight ------------------------------

# A call to one of these names touches the device (install, copy, launch) or
# starts the process that does.
ACTIONS = frozenset({
    "install_app", "copy_to_device", "run_device_console", "Console",
    "Popen", "run",
})
PREFLIGHTS = frozenset({"preflight", "check_report_slots"})
# Each device runner and the function that is its entry point for one run.
RUNNERS = {
    "tests/perf/run_ios_device_workload.py": ("main",),
    "tests/perf/run_ios_device_android.py": ("execute",),
    "tests/perf/run_ios_device_discard_bench.py": ("main",),
    "tests/integration/gpu/run_ios_device_vk_cts.py": ("main",),
    "tests/integration/support/ios_jit_debugger.py": ("main",),
    "Kernarium/scripts/audio-interrupter.py": ("do_install", "do_launch"),
    "Kernarium/scripts/ipad-automation.py": ("command_start",),
}
# The text of a file that installs, copies to or launches an application on a
# device.
DEVICE_ACTION = re.compile(
    r'"process",\s*"launch"|"install",\s*"app"|\binstall_app\(|'
    r"\brun_device_console\(|ios_device\.Console\(|test-without-building"
)
# What the discovery leaves out: the modules that define the plumbing.
PLUMBING = {
    "tests/integration/support/ios_device.py",
    "tests/integration/support/ios_device_reports.py",
}


def callee(node: ast.Call) -> str | None:
    function = node.func
    if isinstance(function, ast.Attribute):
        return function.attr
    if isinstance(function, ast.Name):
        return function.id
    return None


def calls_in(function: ast.AST) -> list[ast.Call]:
    return sorted((node for node in ast.walk(function)
                   if isinstance(node, ast.Call)),
                  key=lambda node: (node.lineno, node.col_offset))


class RunnerStructureTests(unittest.TestCase):
    def parse(self, relative: str) -> ast.Module:
        return ast.parse((REPOSITORY / relative).read_text(encoding="utf-8"))

    def test_every_runner_calls_the_preflight_before_it_touches_the_device(
        self
    ) -> None:
        for relative, entries in RUNNERS.items():
            module = self.parse(relative)
            functions = {node.name: node for node in ast.walk(module)
                         if isinstance(node, (ast.FunctionDef,
                                              ast.AsyncFunctionDef))}
            # A helper of the file that itself calls an action is one.
            helpers = {name for name, node in functions.items()
                       if any(callee(call) in ACTIONS
                              for call in calls_in(node))}
            for entry in entries:
                with self.subTest(runner=relative, entry=entry):
                    self.assertIn(entry, functions)
                    calls = calls_in(functions[entry])
                    checks = [c.lineno for c in calls
                              if callee(c) in PREFLIGHTS]
                    actions = [c.lineno for c in calls
                               if callee(c) in ACTIONS | helpers]
                    self.assertTrue(checks, f"{entry} never calls the preflight")
                    self.assertTrue(actions, f"{entry} never touches the device")
                    self.assertLess(min(checks), min(actions))

    def test_every_file_that_launches_or_installs_is_a_listed_runner(self) -> None:
        found = set()
        for root in ("tests/perf", "tests/integration", "Kernarium/scripts",
                     "Andarium/scripts"):
            for path in (REPOSITORY / root).rglob("*.py"):
                relative = path.relative_to(REPOSITORY).as_posix()
                if relative in PLUMBING:
                    continue
                if DEVICE_ACTION.search(path.read_text(encoding="utf-8")):
                    found.add(relative)
        self.assertEqual(
            found - set(RUNNERS), set(),
            "a device runner that does not call the preflight: add it to "
            "RUNNERS here once it calls ios_device_reports.preflight")

    def test_every_runner_takes_the_two_report_options(self) -> None:
        for relative in RUNNERS:
            if relative.endswith("ios_jit_debugger.py"):
                continue
            with self.subTest(runner=relative):
                self.assertIn(
                    "ios_device_reports.add_arguments(",
                    (REPOSITORY / relative).read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
