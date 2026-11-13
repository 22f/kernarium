#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Every device run starts with free diagnostic-report slots.

iOS keeps a capped number of reports per kind and drops the next one when a
kind is full, so a process kill during a run can leave no evidence. The shared
preflight (tests/integration/support/ios_device_reports.py) counts the
JetsamEvent reports and the crash reports of the processes under test and,
when a kind is short, copies the whole store off the device, checks the copy,
and only then empties the store. These tests run it against fake stores and
against the fake devicectl and idevicecrashreport of fake_device_tools.py, and
check by source structure that every device runner calls it before it touches
the device. Report names here are invented, and no report has contents.
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
import sys
import tempfile
import traceback
import unittest
from unittest import mock

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import ios_device  # noqa: E402
import ios_device_reports as reports  # noqa: E402

DEVICE = "fake-device"
UDID = "0000AAAA-0123456789ABCDEF"
PROCESSES = ("Kernarium", "Andarium")
LEAK = "Leaky-2026-01-01-000000.ips"


def invent(kind: str, count: int, *, folder: str = "") -> list[str]:
    """Synthetic report paths of one kind."""
    return [f"{folder}{kind}-2026-01-{day + 1:02d}-0{day:05d}.ips"
            for day in range(count)]


def listing_text(entries: object) -> str:
    return json.dumps({"result": {"files": entries}})


def entry(path: str, size: int = 1, *, directory: object = False) -> dict:
    return {"relativePath": path, "resources": {"isDirectory": directory},
            "metadata": {"size": size}}


class Scratch(unittest.TestCase):
    """A fresh, resolved scratch directory per test."""

    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name).resolve()
        self.lines: list[str] = []


class FakeStore:
    """A store that holds nothing but names and records what is done."""

    def __init__(self, names: list[str], *, udid: str | None = UDID,
                 listing_fails: bool = False, copy_fails: bool = False,
                 removes: bool = True, mover: bool = True) -> None:
        self.held = list(names)
        self.udid = udid
        self.listing_fails = listing_fails
        self.copy_fails = copy_fails
        self.removes = removes
        self.mover = mover
        self.calls: list[str] = []
        self.copy_directory: pathlib.Path | None = None
        self.mover_directory: pathlib.Path | None = None
        self.mover_udid: str | None = None
        self.copied_at_removal: list[str] = []

    def listing(self) -> list[reports.ListedFile]:
        self.calls.append("listing")
        if self.listing_fails:
            raise ios_device.DeviceError(
                "cannot list the device's diagnostic reports "
                "(devicectl exited 1)")
        return [reports.ListedFile(name, len(name)) for name in self.held]

    def resolve_udid(self) -> str:
        self.calls.append("resolve")
        if self.udid is None:
            raise ios_device.DeviceError(
                "cannot resolve the device's UDID, so no report was removed")
        return self.udid

    def require_mover(self) -> None:
        self.calls.append("require")
        if not self.mover:
            raise ios_device.DeviceError("idevicecrashreport is not on PATH")

    def copy_verified(self, files, directory: pathlib.Path) -> None:
        self.calls.append("copy")
        if self.copy_fails:
            raise ios_device.DeviceError(
                "copying a report failed; nothing was removed")
        self.copy_directory = directory
        for file in files:
            target = directory / file.path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(file.path)

    def remove_all(self, udid: str, directory: pathlib.Path) -> None:
        self.calls.append("remove")
        assert self.copy_directory is not None
        self.mover_udid, self.mover_directory = udid, directory
        self.copied_at_removal = sorted(
            path.relative_to(self.copy_directory).as_posix()
            for path in self.copy_directory.rglob("*") if path.is_file())
        if self.removes:
            self.held = []


class ListingTests(unittest.TestCase):
    def test_a_valid_listing_gives_its_regular_files(self) -> None:
        listed = reports.parse_listing(listing_text([
            entry("Orchid-2026-01-01-000000.ips", 7),
            entry("Retired", directory=True),
            entry("Retired/Quartz-2026-01-02-000000.ips", 9),
        ]))
        self.assertEqual(listed, [
            reports.ListedFile("Orchid-2026-01-01-000000.ips", 7),
            reports.ListedFile("Retired/Quartz-2026-01-02-000000.ips", 9)])

    def test_the_entry_shape_devicectl_gives_is_accepted(self) -> None:
        resources = {"isDirectory": False, "isHidden": False,
                     "isReadable": True, "isSymbolicLink": False,
                     "isWritable": True}
        metadata = {"extendedAttributes": {}, "lastModDate": "x",
                    "ownerGid": 0, "ownerUid": 0, "permissions": 420,
                    "size": 12}
        listed = reports.parse_listing(listing_text([
            {"name": "Search", "relativePath": "A/Search", "metadata": metadata,
             "resources": {**resources, "isDirectory": True}},
            {"name": "x.plist", "relativePath": "A/Search/x.plist",
             "metadata": metadata, "resources": resources}]))
        self.assertEqual(listed, [reports.ListedFile("A/Search/x.plist", 12)])

    def test_only_a_valid_empty_listing_is_an_empty_store(self) -> None:
        self.assertEqual(reports.parse_listing(listing_text([])), [])

    def test_a_listing_that_is_not_understood_is_an_error(self) -> None:
        good = entry("Orchid-2026-01-01-000000.ips", 7)
        cases = {
            "an empty object for the files": listing_text({}),
            "an empty string for the files": listing_text(""),
            "null for the files": listing_text(None),
            "no result": json.dumps({"info": {}}),
            "not an object": json.dumps([good]),
            "not JSON": "no listing",
            "isDirectory as a string": listing_text(
                [entry(f"Orchid-2026-01-{d + 1:02d}-000000.ips", 1,
                       directory="false") for d in range(25)]),
            "isDirectory missing": listing_text(
                [{"relativePath": "x", "resources": {}, "metadata": {"size": 1}}]),
            "a directory with children": listing_text([
                {**entry("Retired", directory=True),
                 "children": [good]}]),
            "an entry that is no object": listing_text(["Orchid"]),
            "no path": listing_text([{"resources": {"isDirectory": False},
                                      "metadata": {"size": 1}}]),
            "no size": listing_text([{"relativePath": "x",
                                      "resources": {"isDirectory": False}}]),
            "a boolean size": listing_text([entry("x", True)]),
            "a negative size": listing_text([entry("x", -1)]),
            "a float size": listing_text([entry("x", 1.5)]),
            "an absolute path": listing_text([entry("/etc/x")]),
            "a path that climbs": listing_text([entry("../x")]),
            "a path with dots": listing_text([entry("a/./x")]),
            "a path listed twice": listing_text([good, good]),
            "a symbolic link": listing_text([
                {**good, "resources": {"isDirectory": False,
                                       "isSymbolicLink": True}}]),
            "a symbolic link that is no boolean": listing_text([
                {**good, "resources": {"isDirectory": False,
                                       "isSymbolicLink": "no"}}]),
        }
        for name, text in cases.items():
            with self.subTest(name):
                with self.assertRaisesRegex(
                    ios_device.DeviceError, "report listing is not understood"
                ) as caught:
                    reports.parse_listing(text)
                self.assertNotIn("Orchid", str(caught.exception))


class CountingTests(unittest.TestCase):
    def test_a_report_counts_under_its_name_without_the_date(self) -> None:
        for file_name, kind in (
            ("JetsamEvent-2026-01-02-030405.ips", "JetsamEvent"),
            ("Orchid-2026-01-02-030405.ips", "Orchid"),
            ("Orchid-2026-01-02-030405.1.ips", "Orchid"),
            ("Orchid-2026-01-02-030405.crash", "Orchid"),
            ("Orchid-2026-01-02-030405.crash.synced", "Orchid"),
            ("Orchid.cpu_resource-2026-01-02-030405.ips", "Orchid.cpu_resource"),
            ("gpuEvent-Orchid-2026-01-02-030405.00015.ips", "gpuEvent-Orchid"),
            ("Quartz-Runner-2026-01-02-030405.ips", "Quartz-Runner"),
            ("Orchid-2026-01-02-030405.ips.synced", "Orchid"),
        ):
            with self.subTest(file_name=file_name):
                self.assertEqual(reports.report_name(file_name), kind)

    def test_a_file_that_is_no_report_has_no_name(self) -> None:
        for file_name in ("heartbeat.plist", "panic-full", "Orchid.txt",
                          "Orchid-2026-01-02.ips", ".unknown"):
            with self.subTest(file_name=file_name):
                self.assertIsNone(reports.report_name(file_name))

    def test_reports_count_at_any_depth_by_their_basename(self) -> None:
        held = (invent("JetsamEvent", 2) + invent("JetsamEvent", 3, folder="Retired/")
                + invent("Kernarium", 4, folder="Panics/Deep/")
                + invent("Kernarium", 1, folder="Orchid-2026-01-01-000000.ips/"))
        self.assertEqual(reports.count_reports(held, PROCESSES),
                         {"JetsamEvent": 5, "Kernarium": 5, "Andarium": 0})

    def test_crash_files_count_as_reports(self) -> None:
        held = [f"Kernarium-2026-01-{d + 1:02d}-000000.crash" for d in range(25)]
        self.assertEqual(reports.count_reports(held, PROCESSES)["Kernarium"], 25)
        self.assertEqual(reports.unparsed_reports(held, PROCESSES), 0)

    def test_only_the_kinds_a_run_needs_are_counted(self) -> None:
        held = (invent("JetsamEvent", 3) + invent("Kernarium", 2)
                + invent("Orchid", 9) + invent("Kernarium.cpu_resource", 7)
                + ["heartbeat.plist"])
        self.assertEqual(
            reports.count_reports(held, PROCESSES),
            {"JetsamEvent": 3, "Kernarium": 2, "Andarium": 0})
        self.assertEqual(reports.unparsed_reports(held, PROCESSES), 0)

    def test_a_file_named_for_a_needed_kind_that_cannot_be_counted(self) -> None:
        held = ["Kernarium-2026-01-01.ips", "JetsamEvent.diag",
                "Andarium_2026-01-01-000000_pad.crash", "Kernarium",
                "KernariumFoo.txt", "Orchid.txt"]
        self.assertEqual(reports.unparsed_reports(held, PROCESSES), 3)


class SlotTests(Scratch):
    def ensure(self, store: FakeStore, destination: str = "moved",
               **keywords) -> reports.ReportSlots:
        return reports.ensure_report_slots(
            store, PROCESSES, self.root / destination, emit=self.lines.append,
            **keywords)

    def test_an_empty_store_moves_nothing(self) -> None:
        store = FakeStore([])
        slots = self.ensure(store)
        self.assertEqual(store.calls, ["listing"])
        self.assertEqual(slots.short, [])
        self.assertEqual(len(self.lines), 1)
        self.assertIn("JetsamEvent 0 -> 0", self.lines[0])
        self.assertIn("Kernarium crash 0 -> 0", self.lines[0])
        self.assertIn("nothing to move", self.lines[0])
        self.assertFalse((self.root / "moved").exists())

    def test_exactly_five_free_slots_is_enough_and_four_is_not(self) -> None:
        enough = reports.REPORT_CAP - reports.FREE_SLOTS
        for kind in ("JetsamEvent", "Kernarium"):
            with self.subTest(kind=kind, held=enough):
                store = FakeStore(invent(kind, enough))
                self.assertEqual(self.ensure(store).short, [])
                self.assertEqual(store.calls, ["listing"])
            with self.subTest(kind=kind, held=enough + 1):
                store = FakeStore(invent(kind, enough + 1))
                slots = self.ensure(store, f"moved-{kind}")
                self.assertEqual(slots.short, [kind])
                self.assertEqual(slots.after[kind], 0)

    def test_a_short_kind_moves_the_whole_store(self) -> None:
        names = (invent("JetsamEvent", 25) + invent("Kernarium", 4)
                 + invent("Orchid", 25, folder="Retired/"))
        store = FakeStore(names)
        slots = self.ensure(store)
        self.assertEqual(store.held, [])
        self.assertEqual(slots.moved, 54)
        self.assertEqual(slots.short, ["JetsamEvent"])
        self.assertIn("JetsamEvent 25 -> 0, Kernarium crash 4 -> 0",
                      self.lines[0])
        self.assertIn(
            f"moved the whole store (54 reports) to {self.root / 'moved'}",
            self.lines[0])

    def test_a_short_crash_kind_alone_also_moves_the_whole_store(self) -> None:
        store = FakeStore(invent("JetsamEvent", 2) + invent("Andarium", 23))
        slots = self.ensure(store)
        self.assertEqual(slots.short, ["Andarium"])
        self.assertEqual(store.held, [])

    def test_nothing_leaves_the_device_before_a_verified_copy_exists(self) -> None:
        names = invent("JetsamEvent", 25) + invent("Orchid", 3, folder="Retired/")
        store = FakeStore(names)
        self.ensure(store)
        self.assertEqual(store.calls, [
            "listing", "resolve", "require", "copy", "remove", "listing"])
        self.assertEqual(store.copied_at_removal, sorted(names))
        self.assertEqual(store.mover_udid, UDID)
        self.assertEqual(store.copy_directory, self.root / "moved" / "copy")
        self.assertEqual(store.mover_directory, self.root / "moved" / "mover")

    def test_a_copy_that_fails_leaves_the_store_untouched(self) -> None:
        store = FakeStore(invent("JetsamEvent", 25), copy_fails=True)
        with self.assertRaisesRegex(ios_device.DeviceError, "nothing was removed"):
            self.ensure(store)
        self.assertNotIn("remove", store.calls)
        self.assertEqual(len(store.held), 25)
        self.assertIn("could not move them", self.lines[0])

    def test_a_device_that_cannot_be_identified_loses_nothing(self) -> None:
        store = FakeStore(invent("JetsamEvent", 25), udid=None)
        with self.assertRaisesRegex(ios_device.DeviceError, "cannot resolve"):
            self.ensure(store)
        self.assertEqual(store.calls, ["listing", "resolve"])
        self.assertFalse((self.root / "moved").exists())

    def test_a_missing_mover_fails_before_anything_is_copied(self) -> None:
        store = FakeStore(invent("JetsamEvent", 25), mover=False)
        with self.assertRaisesRegex(ios_device.DeviceError, "not on PATH"):
            self.ensure(store)
        self.assertNotIn("copy", store.calls)

    def test_a_destination_that_holds_files_is_refused_before_a_copy(self) -> None:
        destination = self.root / "moved"
        destination.mkdir()
        (destination / "old").write_text("x")
        store = FakeStore(invent("JetsamEvent", 25))
        with self.assertRaisesRegex(ios_device.DeviceError, "already holds"):
            self.ensure(store)
        self.assertNotIn("copy", store.calls)
        self.assertEqual(len(store.held), 25)

    def test_the_directories_are_new_and_private(self) -> None:
        store = FakeStore(invent("JetsamEvent", 25))
        self.ensure(store)
        for path in (self.root / "moved", store.copy_directory,
                     store.mover_directory):
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o700)

    def test_a_removal_that_frees_too_little_fails_the_run(self) -> None:
        store = FakeStore(invent("JetsamEvent", 25), removes=False)
        with self.assertRaisesRegex(
            ios_device.DeviceError,
            r"still short of 5 free slots for JetsamEvent"
        ):
            self.ensure(store)
        self.assertEqual(len(self.lines), 1)
        self.assertIn("JetsamEvent 25 -> 25", self.lines[0])
        # Whatever was copied stays where it is.
        self.assertEqual(len(list((self.root / "moved" / "copy").iterdir())), 25)

    def test_a_listing_that_fails_fails_the_run(self) -> None:
        with self.assertRaisesRegex(ios_device.DeviceError, "cannot list"):
            self.ensure(FakeStore([], listing_fails=True))
        self.assertEqual(self.lines, [])

    def test_a_relevant_file_that_cannot_be_counted_fails_the_run(self) -> None:
        store = FakeStore(["Kernarium-2026-01-01.ips"])
        with self.assertRaisesRegex(ios_device.DeviceError, "cannot be counted"):
            self.ensure(store)
        self.assertEqual(store.calls, ["listing"])

    def test_leaving_the_reports_counts_but_never_moves_or_fails(self) -> None:
        store = FakeStore(invent("JetsamEvent", 25) + ["Kernarium-2026-01-01.ips"])
        slots = self.ensure(store, leave=True)
        self.assertEqual(store.calls, ["listing"])
        self.assertEqual(slots.short, ["JetsamEvent"])
        self.assertIn("left in place, short: JetsamEvent", self.lines[0])
        self.lines.clear()
        self.ensure(FakeStore([], listing_fails=True), leave=True)
        self.assertIn("not counted", self.lines[0])

    def test_a_dry_run_reports_what_it_would_move_and_moves_nothing(self) -> None:
        store = FakeStore(invent("JetsamEvent", 25) + invent("Kernarium", 21))
        slots = self.ensure(store, dry_run=True)
        self.assertEqual(store.calls, ["listing"])
        self.assertEqual(sorted(slots.short), ["JetsamEvent", "Kernarium"])
        self.assertIn("would move the whole store (46 reports)", self.lines[0])

    def test_no_report_name_is_ever_printed(self) -> None:
        names = invent("JetsamEvent", 25) + invent("Kernarium", 6)
        self.ensure(FakeStore(names))
        with self.assertRaises(ios_device.DeviceError) as caught:
            reports.ensure_report_slots(
                FakeStore(names, removes=False), PROCESSES,
                self.root / "second", emit=self.lines.append)
        for text in (*self.lines, str(caught.exception)):
            self.assertNotRegex(text, r"\d{4}-\d{2}-\d{2}")
            self.assertNotIn(".ips", text)


class DestinationTests(Scratch):
    def ensure(self, destination: pathlib.Path, **keywords) -> None:
        reports.ensure_report_slots(
            FakeStore([]), PROCESSES, destination, emit=self.lines.append,
            **keywords)

    def test_a_directory_inside_the_checkout_is_refused_ignored_or_not(self) -> None:
        for destination in (
            REPOSITORY / "device-reports",
            REPOSITORY / "tests" / "device-reports" / "x",
            REPOSITORY / ".build" / "device-reports" / "x",
            REPOSITORY / ".git" / "x",
            REPOSITORY / ".." / REPOSITORY.name / "x",
        ):
            with self.subTest(destination=destination):
                with self.assertRaisesRegex(
                    ios_device.DeviceError, "inside the checkout"
                ):
                    self.ensure(destination)

    def test_it_is_refused_even_when_no_report_needs_moving(self) -> None:
        with self.assertRaises(ios_device.DeviceError):
            self.ensure(REPOSITORY / ".build" / "x")

    def test_a_symlink_into_the_checkout_does_not_hide_it(self) -> None:
        (self.root / "link").symlink_to(REPOSITORY / "tests")
        with self.assertRaises(ios_device.DeviceError):
            self.ensure(self.root / "link" / "x")

    def test_a_symlink_anywhere_in_the_path_is_refused(self) -> None:
        (self.root / "elsewhere").mkdir()
        (self.root / "link").symlink_to(self.root / "elsewhere")
        with self.assertRaisesRegex(ios_device.DeviceError, "symlink"):
            self.ensure(self.root / "link" / "x")

    def test_only_components_below_the_home_directory_are_checked_there(self) -> None:
        home = self.root / "home"
        (home / "real").mkdir(parents=True)
        (home / "Library").symlink_to(home / "real")
        with mock.patch.dict(os.environ, {"HOME": str(home)}):
            with self.assertRaisesRegex(ios_device.DeviceError, "symlink"):
                self.ensure(reports.default_destination())

    def test_a_plain_directory_outside_the_checkout_is_accepted(self) -> None:
        self.ensure(self.root / "reports")

    def test_leaving_the_reports_needs_no_directory(self) -> None:
        for keywords in ({"leave": True}, {"dry_run": True}):
            with self.subTest(**keywords):
                reports.ensure_report_slots(
                    FakeStore([]), PROCESSES, None, emit=lambda line: None,
                    **keywords)
        with self.assertRaisesRegex(ios_device.DeviceError, "no directory"):
            reports.ensure_report_slots(
                FakeStore([]), PROCESSES, None, emit=lambda line: None)

    def test_the_default_is_a_new_per_user_folder_outside_the_checkout(self) -> None:
        home = self.root / "home"
        home.mkdir()
        with mock.patch.dict(os.environ, {"HOME": str(home)}):
            first = reports.default_destination()
            self.assertEqual(
                first.parent,
                home / "Library" / "Logs" / "Kernarium" / "device-reports")
            self.assertRegex(first.name, r"^\d{8}-\d{6}$")
            self.ensure(first)
        self.assertNotIn(REPOSITORY.resolve(), first.resolve().parents)


class FakeTools:
    """The fake devicectl and idevicecrashreport on a PATH of their own."""

    def __init__(self, root: pathlib.Path, names: list[str], *,
                 with_move_tool: bool = True) -> None:
        self.state = root / "state"
        self.state.mkdir()
        (self.state / "crash-logs.txt").write_text(
            "".join(f"{name}\n" for name in names))
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

    def steps(self) -> list[str]:
        return [
            " ".join(["devicectl", *command["args"][:3]])
            if command["tool"] == "devicectl" else command["tool"]
            for command in self.commands()]


class DeviceStoreTests(Scratch):
    def move(self, tools: FakeTools, **environment: str) -> None:
        with tools.environment(**environment):
            reports.ensure_report_slots(
                reports.DeviceStore(DEVICE), PROCESSES, self.root / "moved",
                emit=self.lines.append)

    def test_the_listing_is_read_in_memory_at_any_depth(self) -> None:
        tools = FakeTools(self.root, invent("JetsamEvent", 3)
                          + invent("Kernarium", 2, folder="Retired/Deep/"))
        spare = self.root / "spare-tmp"
        spare.mkdir()
        with tools.environment(TMPDIR=str(spare)):
            listed = reports.DeviceStore(DEVICE).listing()
        self.assertEqual(len(listed), 5)
        self.assertEqual([c for c in listed if "/" in c.path][0].size,
                         len(listed[3].path.encode()))
        (command,) = tools.commands()
        arguments = command["args"]
        self.assertEqual(arguments[arguments.index("--json-output") + 1], "-")
        self.assertIn("systemCrashLogs", arguments)
        self.assertEqual(list(spare.iterdir()), [], "no temporary file")

    def test_a_failing_listing_repeats_nothing_the_tool_said(self) -> None:
        tools = FakeTools(self.root, invent("JetsamEvent", 3))
        with tools.environment(FAKE_REPORT_LIST="fail"), \
                self.assertRaises(ios_device.DeviceError) as caught:
            reports.DeviceStore(DEVICE).listing()
        self.assertIn("devicectl exited 1", str(caught.exception))
        shown = "".join(traceback.format_exception(caught.exception))
        self.assertNotIn("Leaky", shown)
        self.assertNotIn("JetsamEvent", shown)

    def test_a_listing_that_is_no_json_is_an_error(self) -> None:
        tools = FakeTools(self.root, [])
        with tools.environment(FAKE_REPORT_LIST="garbage"), \
                self.assertRaisesRegex(ios_device.DeviceError, "not JSON"):
            reports.DeviceStore(DEVICE).listing()

    def test_a_device_name_resolves_to_its_udid(self) -> None:
        tools = FakeTools(self.root, [])
        with tools.environment():
            self.assertEqual(reports.DeviceStore(DEVICE).resolve_udid(), UDID)
            self.assertEqual(reports.DeviceStore(UDID.lower()).resolve_udid(), UDID)

    def test_a_device_that_does_not_resolve_is_an_error(self) -> None:
        tools = FakeTools(self.root, [])
        for variable in ("none", "fail", "not-a-udid"):
            with self.subTest(variable), tools.environment(FAKE_UDID=variable), \
                    self.assertRaisesRegex(
                        ios_device.DeviceError, "cannot resolve"):
                reports.DeviceStore(DEVICE).resolve_udid()

    def test_another_udid_than_the_one_named_is_an_error(self) -> None:
        tools = FakeTools(self.root, [])
        with tools.environment(), self.assertRaisesRegex(
                ios_device.DeviceError, "another UDID"):
            reports.DeviceStore("0000BBBB-0123456789ABCDEF").resolve_udid()

    def test_a_move_copies_every_file_then_empties_the_store(self) -> None:
        names = (invent("JetsamEvent", 22) + invent("Kernarium", 1, folder="Retired/")
                 + invent("Orchid", 3))
        tools = FakeTools(self.root, names)
        self.move(tools)
        steps = tools.steps()
        self.assertEqual(steps[:2], ["devicectl device info files",
                                     "devicectl device info details"])
        self.assertEqual(steps[2:-2], ["devicectl device copy from"] * 26)
        self.assertEqual(steps[-2:], ["idevicecrashreport",
                                      "devicectl device info files"])
        (mover,) = [c for c in tools.commands()
                    if c["tool"] == "idevicecrashreport"]
        self.assertEqual(
            mover["args"], ["-u", UDID, str(self.root / "moved" / "mover")])
        copied = sorted(
            path.relative_to(self.root / "moved" / "copy").as_posix()
            for path in (self.root / "moved" / "copy").rglob("*")
            if path.is_file())
        self.assertEqual(copied, sorted(names))
        self.assertEqual(tools.held(), [])
        self.assertIn("moved the whole store (26 reports)", self.lines[0])

    def test_the_reports_survive_a_mover_that_loses_what_it_removes(self) -> None:
        names = invent("JetsamEvent", 25)
        tools = FakeTools(self.root, names)
        self.move(tools, FAKE_REPORT_MOVE="lose")
        self.assertEqual(tools.held(), [])
        self.assertEqual(
            len(list((self.root / "moved" / "copy").iterdir())), 25)
        self.assertEqual(list((self.root / "moved" / "mover").iterdir()), [])

    def test_a_copy_that_differs_from_the_listing_stops_before_a_removal(self) -> None:
        for mode in ("short", "rename", "fail"):
            with self.subTest(mode):
                root = self.root / mode
                root.mkdir()
                tools = FakeTools(root, invent("JetsamEvent", 25))
                with self.assertRaises(ios_device.DeviceError) as caught:
                    self.move_in(root, tools, mode)
                self.assertIn("nothing was removed", str(caught.exception))
                self.assertNotIn("Leaky", str(caught.exception))
                self.assertNotIn("idevicecrashreport", tools.steps())
                self.assertEqual(len(tools.held()), 25)

    def move_in(self, root: pathlib.Path, tools: FakeTools, mode: str) -> None:
        with tools.environment(FAKE_REPORT_COPY=mode):
            reports.ensure_report_slots(
                reports.DeviceStore(DEVICE), PROCESSES, root / "moved",
                emit=lambda line: None)

    def test_a_device_that_does_not_resolve_loses_nothing(self) -> None:
        tools = FakeTools(self.root, invent("JetsamEvent", 25))
        with self.assertRaisesRegex(ios_device.DeviceError, "cannot resolve"):
            self.move(tools, FAKE_UDID="none")
        self.assertEqual(tools.steps(), ["devicectl device info files",
                                         "devicectl device info details"])
        self.assertFalse((self.root / "moved").exists())
        self.assertEqual(len(tools.held()), 25)

    def test_two_reports_that_would_share_a_path_are_refused(self) -> None:
        names = ["A/Orchid-2026-01-01-000000.ips", "a/orchid-2026-01-01-000000.ips"]
        names += invent("JetsamEvent", 25)
        tools = FakeTools(self.root, names)
        with self.assertRaisesRegex(ios_device.DeviceError, "share a path"):
            self.move(tools)
        self.assertNotIn("devicectl device copy from", tools.steps())

    def test_a_missing_mover_is_fatal_only_when_a_kind_is_short(self) -> None:
        for held, short in ((20, False), (21, True)):
            with self.subTest(held=held):
                root = self.root / str(held)
                root.mkdir()
                tools = FakeTools(root, invent("JetsamEvent", held),
                                  with_move_tool=False)
                with tools.environment():
                    def preflight() -> reports.ReportSlots:
                        return reports.ensure_report_slots(
                            reports.DeviceStore(DEVICE), PROCESSES,
                            root / "moved", emit=lambda line: None)
                    if short:
                        with self.assertRaisesRegex(
                            ios_device.DeviceError,
                            r"idevicecrashreport is not on PATH.*"
                            r"install libimobiledevice"
                        ):
                            preflight()
                        # Nothing was even copied.
                        self.assertNotIn(
                            "devicectl device copy from", tools.steps())
                    else:
                        self.assertEqual(preflight().short, [])

    def test_a_mover_that_removes_nothing_fails_the_run_with_the_copy_kept(self) -> None:
        tools = FakeTools(self.root, invent("JetsamEvent", 25))
        with self.assertRaisesRegex(ios_device.DeviceError, "still short"):
            self.move(tools, FAKE_REPORT_MOVE="keep")
        self.assertEqual(
            len(list((self.root / "moved" / "copy").iterdir())), 25)


class PreflightTests(Scratch):
    def options(self, *arguments: str) -> argparse.Namespace:
        parser = argparse.ArgumentParser()
        parser.add_argument("--device")
        reports.add_arguments(parser)
        return parser.parse_args(["--device", DEVICE, *arguments])

    def run_preflight(self, tools: FakeTools, options: argparse.Namespace,
                      applications=(), extra=()) -> str:
        output = io.StringIO()
        home = self.root / "home"
        home.mkdir(exist_ok=True)
        with tools.environment(HOME=str(home)), contextlib.redirect_stdout(output):
            reports.preflight(options, applications, extra_processes=extra)
        return output.getvalue()

    def test_reports_go_to_a_new_folder_in_the_per_user_logs(self) -> None:
        tools = FakeTools(self.root, invent("JetsamEvent", 22))
        text = self.run_preflight(tools, self.options(), [ios_device.Application(
            "com.example.Orchid", ("ORCHID_",), re.compile("(x)"), "Orchid")])
        (folder,) = (self.root / "home" / "Library" / "Logs" / "Kernarium"
                     / "device-reports").iterdir()
        self.assertRegex(folder.name, r"^\d{8}-\d{6}$")
        self.assertEqual(len(list((folder / "copy").iterdir())), 22)
        self.assertIn("Orchid crash 0 -> 0", text)
        self.assertIn("JetsamEvent 22 -> 0", text)

    def test_the_flag_names_the_folder_and_the_environment_is_its_default(self) -> None:
        named = self.root / "elsewhere"
        tools = FakeTools(self.root, invent("JetsamEvent", 22))
        self.run_preflight(tools, self.options("--device-reports-dir", str(named)))
        self.assertEqual(len(list((named / "copy").iterdir())), 22)
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
        self.assertEqual(tools.steps(), ["devicectl device info files"])

    def test_the_processes_come_from_the_applications_and_the_extras(self) -> None:
        orchid = ios_device.Application(
            "com.example.Orchid", ("ORCHID_",), re.compile("(x)"), "Orchid")
        self.assertEqual(
            reports.report_processes([orchid, orchid], ["Quartz", "Orchid"]),
            ["Orchid", "Quartz"])


class CommandLineTests(Scratch):
    def run_main(self, store: FakeStore, *arguments: str) -> tuple[int, str]:
        output = io.StringIO()
        home = self.root / "home"
        home.mkdir(exist_ok=True)
        with mock.patch.object(reports, "DeviceStore", lambda device: store), \
                mock.patch.dict(os.environ, {"HOME": str(home)}), \
                contextlib.redirect_stdout(output), \
                contextlib.redirect_stderr(output):
            return reports.main(["--device", DEVICE, *arguments]), output.getvalue()

    def test_a_successful_move_exits_0(self) -> None:
        status, text = self.run_main(FakeStore(invent("JetsamEvent", 25)))
        self.assertEqual(status, 0)
        self.assertIn("JetsamEvent 25 -> 0", text)

    def test_what_is_found_short_without_a_move_exits_3(self) -> None:
        for arguments in (("--dry-run",), ("--leave-device-reports",)):
            with self.subTest(arguments):
                status, _ = self.run_main(
                    FakeStore(invent("Kernarium", 25)), *arguments)
                self.assertEqual(status, 3)
                status, _ = self.run_main(
                    FakeStore(invent("Kernarium", 3)), *arguments)
                self.assertEqual(status, 0)

    def test_a_listing_the_opt_out_cannot_count_exits_1(self) -> None:
        status, text = self.run_main(
            FakeStore([], listing_fails=True), "--leave-device-reports")
        self.assertEqual(status, 1)
        self.assertIn("not counted", text)

    def test_a_failure_exits_1_and_repeats_no_name(self) -> None:
        status, text = self.run_main(
            FakeStore(invent("JetsamEvent", 25), removes=False))
        self.assertEqual(status, 1)
        self.assertIn("still short", text)
        status, text = self.run_main(FakeStore(["Kernarium-2026-01-01.ips"]))
        self.assertEqual(status, 1)
        self.assertNotIn("2026", text)

    def test_the_standalone_command_leaks_nothing_of_a_failing_tool(self) -> None:
        tools = FakeTools(self.root, [])
        output = io.StringIO()
        with tools.environment(FAKE_REPORT_LIST="fail", HOME=str(self.root)), \
                contextlib.redirect_stdout(output), \
                contextlib.redirect_stderr(output):
            status = reports.main(["--device", DEVICE])
        self.assertEqual(status, 1)
        self.assertIn("devicectl exited 1", output.getvalue())
        self.assertNotIn("Leaky", output.getvalue())


# --- every device runner calls the preflight ------------------------------

# A call to one of these names touches the device (install, copy, launch) or
# starts the process that does.
ACTIONS = frozenset({
    "install_app", "copy_to_device", "run_device_console", "Console",
    "Popen", "run", "send",
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
    "Kernarium/scripts/ipad-automation.py": (
        "command_start", "command_supervise", "run_op"),
}
# The text of a file that installs, copies to or launches an application on a
# device.
DEVICE_ACTION = re.compile(
    r'"process",\s*"launch"|"install",\s*"app"|\binstall_app\(|'
    r"\brun_device_console\(|ios_device\.Console\(|test-without-building"
)
SHELL_DEVICE_ACTION = re.compile(
    r"devicectl\s+device\s+(?:process\s+launch|install)")
# The support modules that define the plumbing or build on it.
PLUMBING = {
    "tests/integration/support/ios_device.py",
    "tests/integration/support/ios_device_reports.py",
    "tests/integration/support/kernarium.py",
    "tests/integration/support/andarium.py",
}
IMPORTS_PLUMBING = {"ios_device", "ios_jit_debugger"}


def callee(node: ast.Call) -> str | None:
    function = node.func
    if isinstance(function, ast.Attribute):
        return function.attr
    if isinstance(function, ast.Name):
        return function.id
    return None


def live_calls(function: ast.AST) -> list[ast.Call]:
    """The calls of a function that can run: not those under a constant
    condition that never holds, nor after a return or a raise."""
    found: list[ast.Call] = []

    def block(statements: list[ast.stmt]) -> None:
        for statement in statements:
            node(statement)
            if isinstance(statement, (ast.Return, ast.Raise)):
                return

    def node(item: ast.AST) -> None:
        if isinstance(item, ast.If) and isinstance(item.test, ast.Constant):
            block(item.body if item.test.value else item.orelse)
            return
        if isinstance(item, ast.Call):
            found.append(item)
        for _, value in ast.iter_fields(item):
            children = value if isinstance(value, list) else [value]
            statements = [c for c in children if isinstance(c, ast.stmt)]
            if statements:
                block(statements)
            for child in children:
                if isinstance(child, ast.AST) and not isinstance(child, ast.stmt):
                    node(child)

    for statement in function.body:  # type: ignore[attr-defined]
        node(statement)
        if isinstance(statement, (ast.Return, ast.Raise)):
            break
    return sorted(found, key=lambda call: (call.lineno, call.col_offset))


def runner_problems(source: str, entries: tuple[str, ...]) -> list[str]:
    """What is wrong with the runner source: for each entry function, the
    preflight must be called, and before the first call that touches the
    device (a call of an action, or of a function of the file that makes one)."""
    module = ast.parse(source)
    functions = {node.name: node for node in ast.walk(module)
                 if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))}
    helpers = {name for name, function in functions.items()
               if any(callee(call) in ACTIONS for call in live_calls(function))}
    problems = []
    for entry_name in entries:
        if entry_name not in functions:
            problems.append(f"{entry_name} does not exist")
            continue
        calls = live_calls(functions[entry_name])
        checks = [c.lineno for c in calls if callee(c) in PREFLIGHTS]
        actions = [c.lineno for c in calls if callee(c) in ACTIONS | helpers]
        if not checks:
            problems.append(f"{entry_name} never calls the preflight")
        elif actions and min(actions) < min(checks):
            problems.append(f"{entry_name} touches the device first")
        elif not actions:
            problems.append(f"{entry_name} never touches the device")
    return problems


def imports_plumbing(source: str) -> bool:
    """Whether the file imports the device plumbing, under any alias."""
    for node in ast.walk(ast.parse(source)):
        if isinstance(node, ast.Import):
            names = [alias.name for alias in node.names]
        elif isinstance(node, ast.ImportFrom):
            names = [node.module or ""]
        else:
            continue
        if any(name.split(".")[0] in IMPORTS_PLUMBING for name in names):
            return True
    return False


class RunnerStructureTests(unittest.TestCase):
    def test_every_runner_calls_the_preflight_before_it_touches_the_device(self) -> None:
        for relative, entries in RUNNERS.items():
            with self.subTest(runner=relative):
                source = (REPOSITORY / relative).read_text(encoding="utf-8")
                self.assertEqual(runner_problems(source, entries), [])

    def test_the_check_notices_a_preflight_that_cannot_help(self) -> None:
        template = (
            "def main():\n{before}    ios_device.install_app(a, b, c)\n"
            "{after}")
        cases = {
            "none": ("", ""),
            "after the install": ("", "    ios_device_reports.preflight(o, [])\n"),
            "under if False": (
                "    if False:\n        ios_device_reports.preflight(o, [])\n", ""),
            "after a return": (
                "    return\n    ios_device_reports.preflight(o, [])\n", ""),
        }
        for name, (before, after) in cases.items():
            with self.subTest(name):
                self.assertNotEqual(runner_problems(
                    template.format(before=before, after=after), ("main",)), [])
        for before in ("    ios_device_reports.preflight(o, [])\n",
                       "    if x:\n        ios_device_reports.preflight(o, [])\n",
                       "    if True:\n        ios_device_reports.preflight(o, [])\n"):
            with self.subTest(before):
                self.assertEqual(runner_problems(
                    template.format(before=before, after=""), ("main",)), [])

    def test_every_file_that_launches_or_installs_is_a_listed_runner(self) -> None:
        found = set()
        for root in ("tests/perf", "tests/integration", "Kernarium/scripts",
                     "Andarium/scripts"):
            for path in (REPOSITORY / root).rglob("*.py"):
                relative = path.relative_to(REPOSITORY).as_posix()
                if relative in PLUMBING:
                    continue
                text = path.read_text(encoding="utf-8")
                if DEVICE_ACTION.search(text) or imports_plumbing(text):
                    found.add(relative)
        self.assertEqual(
            found - set(RUNNERS), set(),
            "a device runner that does not call the preflight: add it to "
            "RUNNERS here once it calls ios_device_reports.preflight")

    def test_the_discovery_sees_an_aliased_import(self) -> None:
        for source in ("import ios_device as d\n", "from ios_device import install_app as i\n",
                       "import ios_jit_debugger\n", "from ios_device import Console\n"):
            with self.subTest(source):
                self.assertTrue(imports_plumbing(source))
        self.assertFalse(imports_plumbing("import json\n"))

    def test_no_shell_script_launches_or_installs_on_a_device(self) -> None:
        found = []
        for root in ("tests", "Kernarium/scripts", "Andarium/scripts", "scripts"):
            for path in (REPOSITORY / root).rglob("*.sh"):
                if SHELL_DEVICE_ACTION.search(path.read_text(encoding="utf-8")):
                    found.append(path.relative_to(REPOSITORY).as_posix())
        self.assertEqual(found, [], "a shell script cannot call the preflight")

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
