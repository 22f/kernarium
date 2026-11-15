#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify the old-application layer: an old 32-bit application the person switched it on for starts on ART, any other is left alone.

android/lineage-24/overlay/oldapp is a library the framework loads into an application process when the
application targets an API level below 14 (Android before 4.0), is 32-bit and is switched on for it (the persistent
property persist.sys.kernarium.oldapp.<package> is not empty). It gives such an application what Dalvik gave it: a Java `byte[]` that reads
through the object pointer at the Dalvik offsets (a block that stands for the array: a write through it reaches
the array, it is the same object for every JNI function), and a heap below 2 GiB. This runner installs the probe
application of oldapp_probe_apk.py in its two variants and reads what each one reports:

- ``old`` (targetSdkVersion 10, installed past the package manager's block on old targets, as Andarium
  installs such an application): the layer is mapped in its process, the raw read of the `byte[]` returns the
  bytes (also from a thread of its own), the array still works as a JNI reference (handed back to Java, read
  and written through the array functions, one object with a global reference to it, locked, kept while more
  results arrive, popped out of a local frame), getline and malloc_usable_size accept a block the application
  allocated and the application reallocates and frees a block libc allocated, the JNI and invoke tables are
  read-only again, and every allocation lies below 2 GiB;
- ``modern`` (targetSdkVersion 29): the layer is not mapped, and the raw read does not find the bytes (the
  reference is ART's own). The array handed back works, which shows that the probe itself runs.

On an image that carries the layer the framework loads it and the logcat tag ``kernarium_oldapp`` shows the
old process installing it and the modern one not; the old application is first run without being switched on
(the layer is left out) and then switched on. ``--host-shim`` is for an image that does not carry it (the
accepted release before this change): the runner builds the layer with the host's clang and lld, bundles it in
the packages and has the old probe load it, after a control run of the old probe without it (the raw read
fails), which proves the library's mechanism but not the framework's rule. The libraries need `--clang` and
`--lld` (default: those on PATH).

An application that targets below API 23 gets the permission review before its first component runs, and a
broadcast is skipped until then; the runner presses Continue, as the user does on the first launch.

Every wait has a deadline. The guest's launcher is stopped through its own process on any exit, and the guest
is reached through the shared adb server, always named with `-s`.
usage: run_android_oldapp.py --emulator uml-darwin-android17 [--release DIR] [--host-shim] [--serial HOST:PORT]
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
import xml.etree.ElementTree as ElementTree

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "integration" / "support"))
sys.path.insert(0, str(HERE))
import oldapp_probe_apk as probe  # noqa: E402

# The orientation runner owns the guest: its launcher and its adb.
SPEC = importlib.util.spec_from_file_location("run_android_orientation", HERE / "run_android_orientation.py")
assert SPEC is not None and SPEC.loader is not None
orient = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = orient  # dataclasses resolve their annotations through sys.modules
SPEC.loader.exec_module(orient)
Failure = orient.Failure
Report = orient.Report

LOG_TAG = "kernarium_oldapp"
INSTALLED = "installed:"
INSTALL_SECONDS = 180
BROADCAST_SECONDS = 60
#: What each variant must report: the lines that must read exactly so (the others are recorded).
#: What an application does with an array and an allocation the runtime's own way: the layer must not change the verdicts.
SEMANTICS = {"write_through": "pass", "identity": "pass", "retained": "pass", "monitor": "pass", "pop": "pass",
             "getline": "pass", "usable_size": "pass", "libc_block": "pass", "table_read_only": "pass"}
EXPECTED = {
    "old": {"shim_mapped": "yes", "raw_read": "pass", "passback": "pass", "elements": "pass", "global": "pass",
            "thread_raw": "pass", **SEMANTICS, "low_heap": "pass", "done": "1"},
    "modern": {"shim_mapped": "no", "raw_read": "fail", "passback": "pass", "elements": "pass", "global": "pass",
               **SEMANTICS, "done": "1"},
    # The old application before the layer is loaded (a host-shim run's control, or an image's application that is not
    # switched on): what the layer changes.
    "control": {"shim_mapped": "no", "raw_read": "fail", "passback": "pass", "elements": "pass", "global": "pass",
                **SEMANTICS, "done": "1"},
}
DATA = re.compile(r'data="(.*)"', re.DOTALL)


def parse_result(text: str) -> dict[str, str]:
    """The probe's ``key=value`` entries, separated by semicolons or lines."""
    result: dict[str, str] = {}
    for entry in re.split(r"[;\n]", text):
        key, separator, value = entry.strip().partition("=")
        if separator and key:
            result[key] = value
    return result


def broadcast_data(output: str) -> str:
    """The result data `am broadcast` prints after the receiver ran, or an empty string."""
    match = DATA.search(output)
    return match.group(1) if match else ""


def evaluate(variant: str, result: dict[str, str]) -> str:
    """The note of a passing variant; the failure names every line that differs from what the variant must report."""
    wrong = [f"{key}={result.get(key, '<missing>')} (expected {value})"
             for key, value in EXPECTED[variant].items() if result.get(key) != value]
    if wrong:
        raise Failure(f"{variant}: " + ", ".join(wrong) + f"; the file read: {sorted(result.items())}"
                      + (f"; the log: {result['log']}" if "log" in result else ""))
    return ", ".join(f"{key}={value}" for key, value in result.items())


def installs_in_log(text: str) -> int:
    """How many times the log says the layer was installed."""
    return sum(1 for line in text.splitlines() if LOG_TAG in line and INSTALLED in line)


class Serial:
    """A running guest named by its adb serial: the part of the launcher's Guest that the steps use."""

    def __init__(self, adb: str, serial: str) -> None:
        self.adb, self.serial = adb, serial

    def adb_run(self, *arguments: str, timeout: float = 30) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, "-s", self.serial, *arguments], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=timeout, check=False)

    def shell(self, command: str, timeout: float = 30) -> str:
        result = self.adb_run("shell", command, timeout=timeout)
        if result.returncode != 0:
            raise Failure(f"'{command}' exited {result.returncode}: {result.stdout.strip()[-500:]}")
        return result.stdout


def install(guest, apk: pathlib.Path, variant: str) -> str:
    """Installs a variant past the block on old targets (what Andarium's installer does for an old application).
    Each run signs with a key of its own, so an earlier copy goes first."""
    guest.adb_run("uninstall", probe.package_of(variant), timeout=60)
    result = guest.adb_run("install", "-r", "--bypass-low-target-sdk-block", str(apk), timeout=INSTALL_SECONDS)
    if result.returncode != 0 or "Success" not in result.stdout:
        raise Failure(f"installing {probe.package_of(variant)}: {result.stdout.strip()[-400:]}")
    return f"{probe.package_of(variant)} installed"


REVIEW_FLAG = "REVIEW_REQUIRED"
REVIEW_SECONDS = 60
BOUNDS = re.compile(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]")


def continue_button(ui: str) -> tuple[int, int] | None:
    """The centre of the permission review's Continue button in a `uiautomator dump`, if it is on screen."""
    try:
        root = ElementTree.fromstring(ui)
    except ElementTree.ParseError:
        return None
    for node in root.iter("node"):
        if node.get("resource-id", "").endswith(":id/continue_button"):
            match = BOUNDS.fullmatch(node.get("bounds", ""))
            if match:
                left, top, right, bottom = (int(group) for group in match.groups())
                return (left + right) // 2, (top + bottom) // 2
    return None


def review_pending(guest, package: str) -> bool:
    return REVIEW_FLAG in guest.shell(f"dumpsys package {package} | grep -A4 'runtime permissions'")


def complete_review(guest, package: str) -> str:
    """Android shows an application that targets below API 23 the permission review before its first component
    runs (a broadcast is skipped until then). The user's Continue is pressed here, as it is on the first launch of
    such a game."""
    if not review_pending(guest, package):
        return "no review pending"
    guest.shell(f"am start -W -a android.intent.action.REVIEW_PERMISSIONS "
                f"--es android.intent.extra.PACKAGE_NAME {package}", timeout=60)
    deadline = time.monotonic() + REVIEW_SECONDS
    while time.monotonic() < deadline:
        guest.adb_run("shell", "uiautomator dump /data/local/tmp/oldapp-ui.xml", timeout=60)
        dump = guest.adb_run("shell", "cat /data/local/tmp/oldapp-ui.xml", timeout=30)
        button = continue_button(dump.stdout) if dump.returncode == 0 else None
        if button is not None:
            guest.shell(f"input tap {button[0]} {button[1]}")
            time.sleep(2)
            if not review_pending(guest, package):
                return "permission review completed"
        time.sleep(2)
    raise Failure(f"the permission review of {package} was not completed")


def layer_in_image(guest) -> str:
    """The library the framework loads is in the image, with the label an application may execute from."""
    listing = guest.shell(f"ls -lZ {probe.SHIM_IMAGE_PATH}").strip()
    if probe.SHIM_IMAGE_LABEL not in listing:
        raise Failure(f"{probe.SHIM_IMAGE_PATH} does not carry {probe.SHIM_IMAGE_LABEL}: {listing[-200:]}")
    return listing


def switch(guest, package: str, on: bool) -> str:
    """Switches the layer on for the application (or off) by writing its property of the store, as the image's
    own command does, and checks the guest holds what was set."""
    name = probe.STORE_PROPERTY_PREFIX + package
    value = str(int(time.time())) if on else ""
    guest.shell(f"su 0 setprop {name} '{value}'")
    held = guest.shell(f"getprop {name}").strip()
    if held != value:
        raise Failure(f"{name} holds '{held}', not '{value}'")
    return f"{package} {'switched on' if on else 'switched off'}"


def installed_count(guest) -> int:
    return installs_in_log(guest.adb_run("logcat", "-d", "-s", f"{LOG_TAG}:I", timeout=90).stdout)


DIAGNOSIS_WORDS = ("FATAL EXCEPTION", "UnsatisfiedLinkError", "Fatal signal", "backtrace:", "signal 11",
                   LOG_TAG, "died")


def diagnosis(guest, package: str) -> str:
    """What the guest's log says about a probe that never reached its last line: the lines about a crash or
    the layer, and the package's own."""
    text = guest.adb_run("logcat", "-d", "-b", "all", "-v", "threadtime", timeout=90).stdout
    lines = [line for line in text.splitlines()
             if " adbd " not in line and (package in line or any(word in line for word in DIAGNOSIS_WORDS))]
    return " | ".join(line.split(": ", 1)[-1][:200] for line in lines[-8:])


def run_variant(guest, variant: str, *, shim: bool) -> tuple[dict[str, str], int]:
    """Starts the probe with `am broadcast` (the zygote forks an ordinary application process) and reads the probe's
    report from the result data the broadcast prints, and how many installs the log gained meanwhile. A probe that
    did not finish leaves "log" among the entries, with what the guest's log says."""
    package = probe.package_of(variant)
    # A process of an earlier run would take the broadcast with whatever it loaded then.
    guest.adb_run("shell", f"am force-stop {package}", timeout=30)
    before = installed_count(guest)
    extras = f" --es shim {probe.SHIM_LIBRARY}" if shim else ""
    output = guest.shell(f"am broadcast -n {probe.component_of(variant)}{extras}", timeout=BROADCAST_SECONDS)
    if "Broadcast completed" not in output:
        raise Failure(f"the broadcast to {package} was not delivered: {output.strip()[-300:]}")
    result = parse_result(broadcast_data(output))
    if "done" not in result:
        result = {**result, "log": diagnosis(guest, package)}
    return result, installed_count(guest) - before


def run(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report) -> None:
    clang = options.clang or shutil.which("clang++")
    lld = options.lld or shutil.which("ld.lld")
    if not clang or not lld:
        raise Failure("--clang and --lld (or clang++ and ld.lld on PATH) build the armeabi-v7a libraries")
    private.mkdir(parents=True, exist_ok=True)
    probe_library = probe.compile_library((probe.PROBE_SOURCE,), private / f"lib{probe.PROBE_LIBRARY}.so",
                                          f"lib{probe.PROBE_LIBRARY}.so", clang=clang, lld=lld,
                                          needed=probe.PROBE_NEEDED)
    shim_library = None
    if options.host_shim:
        shim_library = probe.compile_library(probe.SHIM_SOURCES, private / f"lib{probe.SHIM_LIBRARY}.so",
                                             f"lib{probe.SHIM_LIBRARY}.so", clang=clang, lld=lld,
                                             needed=probe.SHIM_NEEDED)
    apks = {variant: probe.build_apk(private / f"{variant}.apk", variant, probe_library, shim_library)
            for variant in probe.VARIANTS}
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
        if not options.host_shim:
            report.run("the layer is in the image", lambda: layer_in_image(guest))
        for variant in probe.VARIANTS:
            report.run(f"{variant}: install", lambda variant=variant: install(guest, apks[variant], variant))
        for variant in probe.VARIANTS:
            report.run(f"{variant}: permission review",
                       lambda variant=variant: complete_review(guest, probe.package_of(variant)))

        def old() -> str:
            result, installs = run_variant(guest, "old", shim=options.host_shim)
            note = evaluate("old", result)
            if not options.host_shim and installs < 1:
                raise Failure(f"the log shows no install of the layer in the old process; {note}")
            return f"{note}; installs logged: {installs}"

        def modern() -> str:
            result, installs = run_variant(guest, "modern", shim=False)
            note = evaluate("modern", result)
            if installs != 0:
                raise Failure(f"the layer installed itself in the modern process {installs} times; {note}")
            return note

        def control() -> str:
            result, installs = run_variant(guest, "old", shim=False)
            note = evaluate("control", result)
            if installs != 0:
                raise Failure(f"the layer installed itself in an old process that is not switched on {installs} times; {note}")
            return note

        if options.host_shim:
            report.run("old without the layer: the reads fail (control)", control)
            report.run("old: the application's reads, references and allocations", old)
        else:
            report.run("old, not switched on: the layer is left out", control)
            report.run("old: switched on", lambda: switch(guest, probe.package_of("old"), True))
            try:
                report.run("old: the application's reads, references and allocations", old)
            finally:
                switch(guest, probe.package_of("old"), False)
        report.run("modern: the layer is left out", modern)
    finally:
        stop()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", type=pathlib.Path,
                        help="the macos-android17 uml-darwin-android17 (the launcher's default)")
    parser.add_argument("--release", type=pathlib.Path, help="installed Android release (default: accepted)")
    parser.add_argument("--serial", help="a running guest (host:port); no guest is launched")
    parser.add_argument("--host-shim", action="store_true",
                        help="build the layer on the host, bundle it in the packages and load it from the old "
                        "probe: for an image that does not carry it")
    parser.add_argument("--clang", help="clang++ that builds armv7 (default: on PATH)")
    parser.add_argument("--lld", help="ld.lld (default: on PATH)")
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
    options = build_parser().parse_args()
    if options.cpus <= 0 or options.boot_timeout <= 0:
        print("counts and timeouts must be positive", file=sys.stderr)
        return 2
    adb = orient.boot_android.resolve_executable(options.adb, sdk_fallback=True)
    # Short, so every socket path below it stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-oldapp-"))
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
    raise SystemExit(main())
