#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify the shell command and the store of Old app compatibility on an image.

android/lineage-24/overlay/oldapp_switch is `kernarium_oldapp`, a command that Andarium runs as the adb shell
user: ``list`` names the applications the compatibility layer can apply to (not a system application, a 32-bit
primary ABI, a target level below 14, no shared user ID) with their labels, ``set PACKAGE on|off`` writes or
clears the persistent property ``persist.sys.kernarium.oldapp.<package>`` and stops the application. The
property's SELinux type (overlay/sepolicy/system_ext/private/kernarium_oldapp_prop.te) lets the shell domain set
it and every application domain read it, and no application domain set it. This runner installs the probe
applications of oldswitch_probe_apk.py (one eligible, three that each fail one clause of the rule) and, as the
adb shell user, checks that:

- the command and its jar are in the image and `kernarium_oldapp` resolves on the shell's PATH;
- ``list`` starts with the version line, names the eligible application once, with its label (the TAB of the
  manifest's string turned into a space), ``off``, a turn-on time of 0 and a last-update time between the
  install's start and now, and names none of the others;
- ``set`` refuses an application that fails a clause, a system application and a package that is not installed
  with status 2 and one line on standard error, leaves the property empty, and takes unknown arguments with
  status 64;
- ``set old on`` exits 0 with ``<package><TAB>on``, the property holds the turn-on time (the guest's clock, which
  is the host's), and ``list`` shows ``on`` with that time;
- an ordinary application (the modern probe, domain ``untrusted_app_29``, started with `am broadcast`) reads the
  property with that value and its attempt to set it is refused, with the value unchanged;
- ``set old off`` exits 0 with ``<package><TAB>off``, the property reads empty and ``list`` shows ``off``.

``--real-apk`` (with ``--real-package`` and ``--real-label``) also installs an application you provide, whose
label is a resource of its own package, and requires the listing to show that label: the probe's label is a
literal, so only this application exercises the resource path. The command stops a running application with
`cmd activity force-stop`; the runner checks that it exits 0, not that a process died (the probe's components
do not run before Android's permission review of an application that targets below API 23).

Every wait has a deadline. The guest's launcher is stopped through its own process on any exit, and the guest is
reached through the shared adb server, always named with `-s`.
usage: run_android_oldapp_switch.py --emulator uml-darwin-android17 [--release DIR] [--serial HOST:PORT]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
from typing import NamedTuple

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import oldswitch_probe_apk as probe  # noqa: E402

# The orientation runner owns the guest: its launcher and its adb.
SPEC = importlib.util.spec_from_file_location("run_android_orientation", HERE / "run_android_orientation.py")
assert SPEC is not None and SPEC.loader is not None
orient = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = orient  # dataclasses resolve their annotations through sys.modules
SPEC.loader.exec_module(orient)
Failure = orient.Failure
Report = orient.Report

COMMAND = "kernarium_oldapp"
WRAPPER_PATH = "/system_ext/bin/kernarium_oldapp"
JAR_PATH = "/system_ext/framework/kernarium_oldapp.jar"
VERSION_LINE = "kernarium_oldapp 1"
SHELL_CONTEXT = "u:r:shell:s0"
EXIT_REFUSED = 2
EXIT_USAGE = 64
INSTALL_SECONDS = 180
COMMAND_SECONDS = 120
PROBE_SECONDS = 60
#: The system application the command must refuse.
SYSTEM_PACKAGE = "com.android.settings"
UNKNOWN_PACKAGE = "org.example.andarium.oldswitchprobe.notinstalled"
#: Slack between the host's reading of the guest's clock and a time the guest wrote.
CLOCK_SLACK_SECONDS = 5


class Row(NamedTuple):
    package: str
    label: str
    state: str
    turned_on: int
    last_update: int


class Run(NamedTuple):
    status: int
    out: str
    err: str


def parse_listing(text: str) -> list[Row]:
    """The rows of `kernarium_oldapp list`: the version line first, then five TAB-separated fields on each line."""
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    else:
        raise Failure(f"the listing does not end with a line break: {text[-80:]!r}")
    if not lines or lines[0] != VERSION_LINE:
        raise Failure(f"the listing starts with {lines[:1]!r}, not {VERSION_LINE!r}")
    rows = []
    for line in lines[1:]:
        fields = line.split("\t")
        if len(fields) < 5:
            raise Failure(f"a listing line has {len(fields)} fields, not five: {line!r}")
        package, label, state, turned_on, last_update = fields[:5]
        if state not in ("on", "off") or not turned_on.isdigit() or not last_update.isdigit():
            raise Failure(f"a listing line is malformed: {line!r}")
        rows.append(Row(package, label, state, int(turned_on), int(last_update)))
    return rows


def parse_result(text: str) -> dict[str, str]:
    """The probe application's ``key=value`` lines; a line without ``=`` is a key with an empty value (``done``)."""
    result: dict[str, str] = {}
    for line in text.splitlines():
        key, _, value = line.partition("=")
        if key.strip():
            result[key.strip()] = value.strip()
    return result


def evaluate_probe(result: dict[str, str], expected: str) -> str:
    """The note of a passing probe: an ordinary application read the value the command stored and could not
    change it."""
    context = result.get("context", "")
    if not context.startswith("u:r:untrusted_app_"):
        raise Failure(f"the probe did not run as an ordinary application: context {context!r}")
    wrong = [f"{key}={result.get(key, '<missing>')!r} (expected {value!r})"
             for key, value in (("get", expected), ("set", "denied"), ("after", expected), ("done", ""))
             if result.get(key) != value]
    if wrong:
        raise Failure("the application's view of the property differs: " + ", ".join(wrong))
    return f"{context.split(':s0')[0]}:s0 read {expected} and could not set it"


class Serial:
    """A running guest named by its adb serial."""

    def __init__(self, adb: str, serial: str) -> None:
        self.adb, self.serial = adb, serial

    def adb_run(self, *arguments: str, timeout: float = 30) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, "-s", self.serial, *arguments], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, timeout=timeout, check=False)

    def run(self, command: str, timeout: float = COMMAND_SECONDS) -> Run:
        """A command in the adb shell: its exit status (adb's shell protocol carries it) and its two streams."""
        result = self.adb_run("shell", command, timeout=timeout)
        return Run(result.returncode, result.stdout, result.stderr)

    def shell(self, command: str, timeout: float = 30) -> str:
        result = self.run(command, timeout)
        if result.status != 0:
            raise Failure(f"'{command}' exited {result.status}: {(result.out + result.err).strip()[-500:]}")
        return result.out


def guest_time(guest) -> int:
    return int(guest.shell("date +%s").strip())


def install(guest, apk: pathlib.Path, package: str) -> None:
    """Installs past the block on old targets (what Andarium's installer does for an old application). Each run
    signs with a key of its own, so an earlier copy goes first."""
    guest.adb_run("uninstall", package, timeout=60)
    result = guest.adb_run("install", "-r", "--bypass-low-target-sdk-block", str(apk), timeout=INSTALL_SECONDS)
    if result.returncode != 0 or "Success" not in result.stdout:
        raise Failure(f"installing {package}: {(result.stdout + result.stderr).strip()[-400:]}")


def command_present(guest) -> str:
    """The wrapper and the jar are in the image, the wrapper resolves on the shell's PATH, and the shell is
    the shell domain (the policy under test is the shell domain's)."""
    listing = guest.shell(f"ls -lZ {WRAPPER_PATH} {JAR_PATH}")
    if WRAPPER_PATH not in listing or JAR_PATH not in listing:
        raise Failure(f"the image lacks the command: {listing.strip()[-300:]}")
    resolved = guest.shell(f"command -v {COMMAND}").strip()
    if resolved != WRAPPER_PATH:
        raise Failure(f"{COMMAND} resolves to {resolved!r}, not {WRAPPER_PATH}")
    context = guest.shell("id -Z").strip()
    if not context.startswith(SHELL_CONTEXT):
        raise Failure(f"adb shell runs in {context!r}, not {SHELL_CONTEXT}")
    return f"{resolved}; adb shell is {context}"


def listing_of(guest) -> list[Row]:
    result = guest.run(f"{COMMAND} list")
    if result.status != 0 or result.err.strip():
        raise Failure(f"'{COMMAND} list' exited {result.status}: {(result.out + result.err).strip()[-500:]}")
    return parse_listing(result.out)


def row_of(rows: list[Row], variant: str) -> Row | None:
    found = [row for row in rows if row.package == probe.package_of(variant)]
    if len(found) > 1:
        raise Failure(f"{probe.package_of(variant)} is listed {len(found)} times")
    return found[0] if found else None


def check_listing(guest, installed_at: int) -> str:
    rows = listing_of(guest)
    now = guest_time(guest)
    old = row_of(rows, "old")
    if old is None:
        raise Failure(f"the eligible probe is not listed: {[row.package for row in rows]}")
    if old.label != probe.LISTED_LABEL:
        raise Failure(f"the label reads {old.label!r}, not {probe.LISTED_LABEL!r}")
    if (old.state, old.turned_on) != ("off", 0):
        raise Failure(f"the eligible probe starts as {old.state} {old.turned_on}, not off 0")
    if not installed_at - CLOCK_SLACK_SECONDS <= old.last_update <= now + CLOCK_SLACK_SECONDS:
        raise Failure(f"last-update {old.last_update} lies outside the install window [{installed_at}, {now}]")
    unlisted = [probe.package_of(variant) for variant in probe.REFUSED if row_of(rows, variant) is not None]
    if unlisted or any(row.package == SYSTEM_PACKAGE for row in rows):
        raise Failure(f"applications that are not eligible are listed: {unlisted or SYSTEM_PACKAGE}")
    return f"{len(rows)} application(s) listed; {old.package} {old.label!r} off, updated {old.last_update}"


def property_value(guest, name: str) -> str:
    return guest.shell(f"getprop {name}").strip()


def check_refusals(guest) -> str:
    notes = []
    targets = [(probe.package_of(variant), probe.property_of(variant)) for variant in probe.REFUSED]
    targets += [(SYSTEM_PACKAGE, probe.STORE_PROPERTY_PREFIX + SYSTEM_PACKAGE),
                (UNKNOWN_PACKAGE, probe.STORE_PROPERTY_PREFIX + UNKNOWN_PACKAGE)]
    for package, name in targets:
        for mode in ("on", "off"):
            result = guest.run(f"{COMMAND} set {package} {mode}")
            lines = result.err.strip().splitlines()
            if result.status != EXIT_REFUSED or result.out.strip() or len(lines) != 1:
                raise Failure(f"set {package} {mode}: status {result.status}, out {result.out.strip()!r}, "
                              f"err {result.err.strip()!r}; expected status {EXIT_REFUSED} and one line on stderr")
        if property_value(guest, name):
            raise Failure(f"{name} holds a value after a refusal")
        notes.append(package.rsplit(".", 1)[-1])
    for arguments in ("", "list extra", "set", f"set {probe.package_of('old')}", f"set {probe.package_of('old')} maybe",
                      f"set {probe.package_of('old')} on extra", "frobnicate"):
        result = guest.run(f"{COMMAND} {arguments}".strip())
        if result.status != EXIT_USAGE or not result.err.startswith("usage: ") or result.out.strip():
            raise Failure(f"'{COMMAND} {arguments}': status {result.status}, out {result.out.strip()!r}, "
                          f"err {result.err.strip()!r}; expected status {EXIT_USAGE} and the usage on stderr")
    return "refused (status 2, one line): " + ", ".join(notes) + "; unknown arguments: status 64"


def switch_on(guest) -> tuple[str, int]:
    """Turns the eligible probe on and returns the note and the property's value."""
    package, name = probe.package_of("old"), probe.property_of("old")
    before = guest_time(guest)
    result = guest.run(f"{COMMAND} set {package} on")
    after = guest_time(guest)
    if result.status != 0 or result.out != f"{package}\ton\n" or result.err.strip():
        raise Failure(f"set {package} on: status {result.status}, out {result.out!r}, err {result.err.strip()!r}")
    value = property_value(guest, name)
    if not value.isdigit() or not before - CLOCK_SLACK_SECONDS <= int(value) <= after + CLOCK_SLACK_SECONDS:
        raise Failure(f"{name} holds {value!r}, not a time between {before} and {after}")
    row = row_of(listing_of(guest), "old")
    if row is None or (row.state, row.turned_on) != ("on", int(value)):
        raise Failure(f"the listing shows {row} after set on; the property holds {value}")
    return f"{name} = {value}; listed on", int(value)


def application_view(guest, value: int) -> str:
    """The modern probe, an ordinary application, reads the eligible application's property and tries to set it."""
    package = probe.package_of("modern")
    guest.adb_run("shell", f"run-as {package} rm -f {probe.RESULT_FILE}", timeout=30)
    broadcast = guest.shell(f"am broadcast -n {probe.component_of('modern')}", timeout=60)
    if "Broadcast completed" not in broadcast:
        raise Failure(f"the probe broadcast was not delivered: {broadcast.strip()[-300:]}")
    deadline = time.monotonic() + PROBE_SECONDS
    result: dict[str, str] = {}
    while time.monotonic() < deadline:
        read = guest.run(f"run-as {package} cat {probe.RESULT_FILE}", timeout=30)
        result = parse_result(read.out) if read.status == 0 else {}
        if "done" in result:
            break
        time.sleep(1)
    else:
        raise Failure(f"the probe application wrote no result in {PROBE_SECONDS} s: {result}")
    note = evaluate_probe(result, str(value))
    held = property_value(guest, probe.property_of("old"))
    if held != str(value):
        raise Failure(f"the property reads {held!r} after the application's attempt, not {value}")
    return note


def switch_off(guest) -> str:
    package, name = probe.package_of("old"), probe.property_of("old")
    result = guest.run(f"{COMMAND} set {package} off")
    if result.status != 0 or result.out != f"{package}\toff\n" or result.err.strip():
        raise Failure(f"set {package} off: status {result.status}, out {result.out!r}, err {result.err.strip()!r}")
    held = property_value(guest, name)
    if held:
        raise Failure(f"{name} holds {held!r} after set off")
    row = row_of(listing_of(guest), "old")
    if row is None or (row.state, row.turned_on) != ("off", 0):
        raise Failure(f"the listing shows {row} after set off")
    return f"{name} is empty; listed off"


def check_real(guest, apk: pathlib.Path, package: str, label: str) -> str:
    """An application whose label is a resource of its own package is listed under that label."""
    install(guest, apk, package)
    try:
        row = next((row for row in listing_of(guest) if row.package == package), None)
        if row is None:
            raise Failure(f"{package} is not listed: it is not eligible, or the command does not see it")
        if row.label != label:
            raise Failure(f"{package} is listed as {row.label!r}, not {label!r}")
        return f"{package} is listed as {row.label!r}"
    finally:
        guest.adb_run("uninstall", package, timeout=60)


def run(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report) -> None:
    private.mkdir(parents=True, exist_ok=True)
    apks = {variant: probe.build_apk(private / f"{variant}.apk", variant) for variant in probe.VARIANTS}
    if options.serial:
        guest = Serial(adb, options.serial)
        stop = lambda: None  # noqa: E731
    else:
        settings = argparse.Namespace(
            release=options.release, emulator=options.emulator, instance=None, adb_port=options.adb_port,
            cpus=options.cpus, memory=options.memory, boot_timeout=options.boot_timeout, no_argument=True,
            extra_arguments=[])
        guest = orient.start_guest(settings, adb, private / "guest", early=None, holder={}, provision=True)
        stop = lambda: orient.stop_guest(adb, guest)  # noqa: E731
    try:
        report.run("the command is in the image", lambda: command_present(guest))
        installed_at = guest_time(guest)
        for variant in probe.VARIANTS:
            report.run(f"{variant}: install", lambda variant=variant: (
                install(guest, apks[variant], probe.package_of(variant)) or f"{probe.package_of(variant)} installed"))
        report.run("list names the eligible application and no other", lambda: check_listing(guest, installed_at))
        report.run("set refuses what is not eligible; usage", lambda: check_refusals(guest))
        state: dict[str, int] = {}

        def on() -> str:
            note, state["value"] = switch_on(guest)
            return note

        try:
            if report.run("set on", on):
                report.run("an application reads it and cannot set it", lambda: application_view(guest, state["value"]))
        finally:
            report.run("set off", lambda: switch_off(guest))
        if options.real_apk:
            report.run("a resource label is listed", lambda: check_real(
                guest, options.real_apk, options.real_package, options.real_label))
    finally:
        for variant in probe.VARIANTS:
            guest.adb_run("uninstall", probe.package_of(variant), timeout=60)
        stop()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", type=pathlib.Path,
                        help="the macos-android17 uml-darwin-android17 (the launcher's default)")
    parser.add_argument("--release", type=pathlib.Path, help="installed Android release built with the command")
    parser.add_argument("--serial", help="a running guest (host:port); no guest is launched")
    parser.add_argument("--real-apk", type=pathlib.Path,
                        help="an old application whose label is a resource, to check the label path")
    parser.add_argument("--real-package", help="its package name (with --real-apk)")
    parser.add_argument("--real-label", help="the label the listing must show for it (with --real-apk)")
    parser.add_argument("--adb", default="adb", help="adb client to use")
    parser.add_argument("--adb-port", type=int,
                        default=orient.slot_ports.slot_port(orient.slot_ports.ADB_OFFSET, orient.DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {orient.DEFAULT_ADB_PORT})")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--memory", default="4G")
    parser.add_argument("--boot-timeout", type=int, default=900, help="seconds to wait for adbd and boot_completed")
    return parser


def main() -> int:
    parser = build_parser()
    options = parser.parse_args()
    if options.cpus <= 0 or options.boot_timeout <= 0:
        print("counts and timeouts must be positive", file=sys.stderr)
        return 2
    if bool(options.real_apk) != bool(options.real_package) or bool(options.real_apk) != bool(options.real_label):
        parser.error("--real-apk, --real-package and --real-label go together")
    adb = orient.boot_android.resolve_executable(options.adb, sdk_fallback=True)
    # Short, so every socket path below it stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-oldswitch-"))
    report = Report()
    try:
        run(options, adb, private, report)
        print("\n".join(f"{status:4} {name}: {detail}" for name, status, detail in report.rows))
        print("FAIL" if report.failed else "PASS")
        return 1 if report.failed else 0
    except (Failure, orient.boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        for log in sorted(private.glob("*/emulator.log")):
            print(f"{log.parent.name} launcher log tail:\n"
                  + "\n".join(log.read_text(errors="replace").splitlines()[-30:]), file=sys.stderr)
        return 1
    finally:
        shutil.rmtree(private, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
