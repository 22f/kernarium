#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Free the diagnostic-report slots of a device before a run uses it.

iOS keeps a capped number of diagnostic reports per report name: the
``JetsamEvent`` reports of every process kill, and the crash reports of each
process (``<Process>-<date>.ips``) are separate sets. A full set drops the
next report instead of replacing an old one, so an application kill during a
run leaves no evidence. Every device runner therefore calls ``preflight``
before it installs or launches anything: it counts the reports of the kinds a
run needs, and when fewer than ``FREE_SLOTS`` of ``REPORT_CAP`` slots are free
for a kind, moves every report of that kind off the device into a directory
outside the repository, then counts again and fails the run if the slots are
still short. The caller names the directory (``--device-reports-dir`` or
``UML_DEVICE_REPORTS_DIR``); by default it is ``device-reports/<timestamp>``
in the run's own output directory.

The cap is an observation, not a documented value: a device's store held
exactly that many reports of several kinds and none above it, and its newest
``JetsamEvent`` was older than a process kill whose report was missing.

``devicectl`` lists and copies the ``systemCrashLogs`` domain but has no
command that removes a file, so removal is ``idevicecrashreport`` (libimobile-
device), which copies a report to the Mac and removes it from the device.
That tool is needed only when a kind is short. Reports are the device owner's
data: this module prints counts and a directory, never a name or content.

  ios_device_reports.py --device UDID --device-reports-dir DIR [--dry-run]

checks a device on its own and exits 3 when a kind is short, in the dry run
without moving anything.
"""

from __future__ import annotations
# Imported support must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass, field
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

import ios_device
from ios_device import DeviceError

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
# iOS keeps at most this many reports of one name (observed, not documented).
REPORT_CAP = 25
# A run starts only with this many free slots in each kind it needs.
FREE_SLOTS = 5
JETSAM = "JetsamEvent"
CRASH_LOG_DOMAIN = "systemCrashLogs"
MOVE_TOOL = "idevicecrashreport"
DIRECTORY_VARIABLE = "UML_DEVICE_REPORTS_DIR"
LIST_TIMEOUT = 120
MOVE_TIMEOUT = 600
# ``<name>-<date>-<time>[.<n>].ips``: the name is what the cap counts.
REPORT_NAME = re.compile(
    r"^(?P<name>.+?)-\d{4}-\d{2}-\d{2}-\d{6}(?:\.\d+)?\.ips(?:\..*)?$"
)
# The processes of the applications this repository runs on a device, for a
# caller that names none.
APPLICATION_PROCESSES = ("Kernarium", "Andarium")


def report_name(file_name: str) -> str | None:
    """The kind a report counts under, or None for a file that is no report."""
    found = REPORT_NAME.match(file_name)
    return found.group("name") if found else None


def count_reports(
    file_names: Iterable[str], processes: Sequence[str]
) -> dict[str, int]:
    """Reports held per kind: ``JetsamEvent`` and each process's crashes."""
    counts = {JETSAM: 0, **{process: 0 for process in processes}}
    for file_name in file_names:
        name = report_name(file_name)
        if name in counts:
            counts[name] += 1
    return counts


def short_kinds(counts: dict[str, int]) -> list[str]:
    return [
        name for name, held in counts.items() if REPORT_CAP - held < FREE_SLOTS
    ]


def move_filter(name: str) -> str:
    """What ``idevicecrashreport -f`` matches in a file name for this kind."""
    return f"{name}-"


class DeviceStore:
    """The report store of one device: ``devicectl`` lists it and
    ``idevicecrashreport`` empties it of a kind."""

    def __init__(self, device: str) -> None:
        self.device = device

    def names(self) -> list[str]:
        """The file names of the device's reports, at any depth."""
        with tempfile.TemporaryDirectory(prefix="ios-device-reports-") as name:
            listing = pathlib.Path(name) / "files.json"
            try:
                completed = subprocess.run(
                    ios_device.device_command(
                        "device", "info", "files", "--device", self.device,
                        "--domain-type", CRASH_LOG_DOMAIN,
                        "--json-output", str(listing),
                        "--quiet", "--timeout", str(LIST_TIMEOUT),
                    ),
                    capture_output=True,
                    text=True,
                    stdin=subprocess.DEVNULL,
                    timeout=LIST_TIMEOUT + 30,
                )
            except (OSError, subprocess.SubprocessError) as error:
                raise DeviceError(
                    f"cannot list the device's diagnostic reports: {error}"
                ) from error
            if completed.returncode != 0:
                lines = completed.stderr.strip().splitlines() or [""]
                raise DeviceError(
                    "cannot list the device's diagnostic reports (devicectl "
                    f"exited {completed.returncode}): {lines[-1][-200:]}"
                )
            try:
                entries = json.loads(listing.read_text(encoding="utf-8"))[
                    "result"]["files"]
                return [
                    pathlib.PurePosixPath(entry["relativePath"]).name
                    for entry in entries
                    if not entry.get("resources", {}).get("isDirectory")
                ]
            except (OSError, ValueError, KeyError, TypeError) as error:
                raise DeviceError(
                    "the device's report listing is unreadable: "
                    f"{type(error).__name__}"
                ) from error

    def move(self, names: Sequence[str], destination: pathlib.Path) -> None:
        """Move every report of the given kinds into ``destination``."""
        tool = shutil.which(MOVE_TOOL)
        if tool is None:
            raise DeviceError(
                f"{MOVE_TOOL} is not on PATH and the device's report store "
                "is short of free slots; install libimobiledevice (it "
                f"provides {MOVE_TOOL}) or empty the store by hand"
            )
        destination.mkdir(parents=True, exist_ok=True)
        for name in names:
            try:
                completed = subprocess.run(
                    [tool, "-u", self.device, "-f", move_filter(name),
                     str(destination)],
                    capture_output=True,
                    text=True,
                    stdin=subprocess.DEVNULL,
                    timeout=MOVE_TIMEOUT,
                )
            except (OSError, subprocess.SubprocessError) as error:
                raise DeviceError(
                    f"{MOVE_TOOL} failed on the device's {name} reports: "
                    f"{error}"
                ) from error
            if completed.returncode != 0:
                raise DeviceError(
                    f"{MOVE_TOOL} exited {completed.returncode} on the "
                    f"device's {name} reports; it needs the device's UDID "
                    "and a USB connection"
                )


@dataclass
class ReportSlots:
    """What a preflight found and did."""

    before: dict[str, int]
    after: dict[str, int]
    short: list[str] = field(default_factory=list)
    destination: pathlib.Path | None = None
    moved: int = 0
    dry_run: bool = False
    left: bool = False
    failed: bool = False


def label(name: str) -> str:
    return name if name == JETSAM else f"{name} crash"


def summary_line(slots: ReportSlots) -> str:
    counts = ", ".join(
        f"{label(name)} {held} -> {slots.after[name]}"
        for name, held in slots.before.items()
    )
    if not slots.short:
        outcome = "nothing to move"
    elif slots.dry_run:
        outcome = f"would move {sum(slots.before[n] for n in slots.short)}"
    elif slots.left:
        outcome = "left in place, short: " + ", ".join(
            label(name) for name in slots.short)
    elif slots.failed:
        outcome = "could not move them"
    else:
        outcome = f"moved {slots.moved} to {slots.destination}"
    return (
        f"device reports (cap {REPORT_CAP} per kind, {FREE_SLOTS} free "
        f"needed): {counts}; {outcome}"
    )


def refuse_repository_directory(destination: pathlib.Path) -> None:
    """Reports are the device owner's data and never enter the repository.

    A directory inside the work tree is refused unless git ignores it, which
    keeps it out of every commit (the default build and artifact trees)."""
    resolved = destination.resolve()
    try:
        relative = resolved.relative_to(REPOSITORY)
    except ValueError:
        return
    try:
        ignored = subprocess.run(
            ["git", "-C", str(REPOSITORY), "check-ignore", "-q", "--",
             str(relative)],
            capture_output=True,
            stdin=subprocess.DEVNULL,
            timeout=30,
        ).returncode == 0
    except (OSError, subprocess.SubprocessError):
        ignored = False
    if not ignored:
        raise DeviceError(
            f"the report directory {destination} is inside the repository "
            "and not ignored by git; name one outside it "
            f"(--device-reports-dir or {DIRECTORY_VARIABLE})"
        )


def ensure_report_slots(
    store: DeviceStore,
    processes: Sequence[str],
    destination: pathlib.Path | None,
    *,
    leave: bool = False,
    dry_run: bool = False,
    emit: Callable[[str], None] = lambda line: print(line, flush=True),
) -> ReportSlots:
    """At least ``FREE_SLOTS`` free slots of ``JetsamEvent`` and of each
    process's crash reports, or an error.

    ``destination`` is where moved reports go; only a dry run or ``leave``
    may omit it. ``leave`` only counts and reports: a caller that needs the
    old reports on the device accepts a short store. ``dry_run`` also moves
    nothing but reports what it would move."""
    if destination is None and not (leave or dry_run):
        raise DeviceError("no directory was named for moved reports")
    if destination is not None and not leave:
        refuse_repository_directory(destination)
    try:
        before = count_reports(store.names(), processes)
    except DeviceError as error:
        if not leave:
            raise
        emit(f"device reports: not counted ({error})")
        return ReportSlots({}, {}, left=True)
    short = short_kinds(before)
    slots = ReportSlots(dict(before), dict(before), short,
                        dry_run=dry_run, left=leave)
    if not short or leave or dry_run:
        emit(summary_line(slots))
        return slots
    assert destination is not None
    slots.destination = destination
    try:
        store.move(short, destination)
    except DeviceError:
        slots.failed = True
        emit(summary_line(slots))
        raise
    slots.after = count_reports(store.names(), processes)
    slots.moved = sum(slots.before[name] - slots.after[name] for name in short)
    emit(summary_line(slots))
    if short_kinds(slots.after):
        raise DeviceError(
            f"the device's report store is still short of {FREE_SLOTS} free "
            "slots for "
            + ", ".join(label(name) for name in short_kinds(slots.after))
            + f" after moving reports; empty it by hand with {MOVE_TOOL} "
            "-u <udid> <directory>"
        )
    return slots


def add_arguments(parser: argparse.ArgumentParser) -> None:
    """The two options every device runner takes."""
    group = parser.add_argument_group("diagnostic reports")
    group.add_argument(
        "--device-reports-dir", type=pathlib.Path, metavar="DIR",
        default=pathlib.Path(os.environ[DIRECTORY_VARIABLE])
        if os.environ.get(DIRECTORY_VARIABLE) else None,
        help="where reports moved off the device go, outside the repository "
             f"(default: ${DIRECTORY_VARIABLE}, else device-reports/"
             "<timestamp> in the run's output directory)",
    )
    group.add_argument(
        "--leave-device-reports", action="store_true",
        help="count the device's reports but never move any, and run even "
             "when a kind has fewer than "
             f"{FREE_SLOTS} free slots",
    )


def report_processes(
    applications: Iterable[ios_device.Application],
    extra: Iterable[str] = (),
) -> list[str]:
    names = [application.process_name for application in applications]
    names.extend(extra)
    return list(dict.fromkeys(names))


def preflight(
    options: argparse.Namespace,
    applications: Iterable[ios_device.Application],
    run_directory: pathlib.Path,
    *,
    extra_processes: Iterable[str] = (),
) -> ReportSlots:
    """The check every device runner makes before it installs or launches.

    ``options`` carries ``device`` and the two ``add_arguments`` options;
    ``run_directory`` is the run's own output directory."""
    destination = options.device_reports_dir or (
        run_directory / "device-reports" / time.strftime("%Y%m%d-%H%M%S")
    )
    return ensure_report_slots(
        DeviceStore(options.device),
        report_processes(applications, extra_processes),
        destination,
        leave=options.leave_device_reports,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Count a device's diagnostic reports and free the slots "
                    "a run needs.")
    parser.add_argument("--device", required=True,
                        help="devicectl identifier; the UDID to move reports")
    parser.add_argument(
        "--process", action="append", metavar="NAME",
        help="a process whose crash reports a run needs slots for "
             f"(repeatable; default: {', '.join(APPLICATION_PROCESSES)})")
    parser.add_argument("--dry-run", action="store_true",
                        help="count and report, moving nothing")
    add_arguments(parser)
    options = parser.parse_args(argv)
    if options.device_reports_dir is None and not (
        options.dry_run or options.leave_device_reports
    ):
        parser.error(
            f"name where reports go with --device-reports-dir or "
            f"{DIRECTORY_VARIABLE}")
    try:
        slots = ensure_report_slots(
            DeviceStore(options.device),
            options.process or list(APPLICATION_PROCESSES),
            options.device_reports_dir,
            leave=options.leave_device_reports,
            dry_run=options.dry_run,
        )
    except DeviceError as error:
        print(f"ios_device_reports.py: {error}", file=sys.stderr)
        return 1
    return 3 if slots.short else 0


if __name__ == "__main__":
    sys.exit(main())
