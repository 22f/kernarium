#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A camera stream carries on across a guest system suspend.

A guest that leaves the foreground, or that Android suspends once its display
is off, has its virtio-uml disable the rings of every device, and a ring
disable with a request the device still holds resets that device. A camera
stream always holds buffers, so the driver stops its streams before the
suspend, which hands every buffer back and leaves the device with the streams,
and queues the buffers again after it.

The guest streams the back camera's video next to its metadata, with the zoom
written, and suspends by each route (the host's, `suspend` and `wake` on the
management console, and the guest's own, `echo mem` into /sys/power/state,
ended by a `wake`). After each wake the frames must keep coming within
RESUME_SECONDS, no buffer may have failed, the metadata must still show the
zoom, and the driver must not have reset the device. The contract is the
camera's suspend section of the Virtio video README.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import shutil
import sys
import tempfile
import time

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "kernel"))

from guest import Guest, GuestFile  # noqa: E402
from run_guest_suspend_holds import (  # noqa: E402
    ManagementConsole, SELF_SUSPEND, SUSPEND_ENTRY, SUSPEND_EXIT, drain,
    hold_quietly, wait_for_exit_line, wait_for_line)

VCPUS = 2
BOOT_SECONDS = 180
BUILD_SECONDS = 300
READY_SECONDS = 90
RESUME_SECONDS = 60
HOLD_SECONDS = 3.0
SOURCE = "/opt/uml-test/camera_capture_probe.cpp"
BINARY = "/tmp/camera-probe"
LOG = "/tmp/camera-hold.log"
FAILURE = re.compile(
    r"WARNING:|\bBUG\b|Oops|Call Trace:|Kernel panic|Darwin UML panic|"
    r"Darwin POSIX|Kernel mode fault|refusing to freeze|"
    r"Freezing user space processes failed|blocked for more than"
)
# The driver says so when the device had forgotten its streams after a resume.
RESET = "the device was reset: restoring the streams"
TICK = re.compile(r"^CAMSUSP tick frames=(\d+) errors=(\d+) zoom=(0x[0-9a-f]+)\s*$", re.M)
ARMS = ("host", "self")


def last_tick(guest: Guest) -> tuple[int, int, int] | None:
    text = guest.run_command(f"cat {LOG}", 30).output
    ticks = TICK.findall(text)
    if not ticks:
        return None
    frames, errors, zoom = ticks[-1]
    return int(frames), int(errors), int(zoom, 16)


def wait_for_progress(guest: Guest, frames: int, seconds: float, what: str) -> tuple[int, int, int]:
    """The stream's last report once it counts ``frames`` more frames."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        tick = last_tick(guest)
        if tick is not None and tick[0] >= frames:
            return tick
        drain(guest, 0.5)
    raise RuntimeError(f"{what}: fewer than {frames} frames in {seconds:g} s")


def suspend_by_host(guest: Guest, console: ManagementConsole) -> None:
    exits = guest.output().count(SUSPEND_EXIT)
    answer = console.request("suspend")
    if "suspended" not in answer:
        raise RuntimeError(f"host suspend: the guest replied {answer!r}")
    hold_quietly(guest, exits, HOLD_SECONDS, "host suspend")
    answer = console.request("wake")
    if "running" not in answer:
        raise RuntimeError(f"host suspend: wake replied {answer!r}")
    wait_for_exit_line(guest, exits, "host suspend: the guest did not resume")


def suspend_by_guest(guest: Guest, console: ManagementConsole) -> None:
    exits = guest.output().count(SUSPEND_EXIT)
    entries = guest.output().count(SUSPEND_ENTRY)
    guest._write(SELF_SUSPEND)
    wait_for_line(guest, SUSPEND_ENTRY, entries, "guest suspend")
    hold_quietly(guest, exits, HOLD_SECONDS, "guest suspend")
    answer = console.request("wake")
    if "running" not in answer:
        raise RuntimeError(f"guest suspend: wake replied {answer!r}")
    wait_for_exit_line(guest, exits, "guest suspend: the guest did not resume")


def run(arguments: argparse.Namespace, private: pathlib.Path) -> list[str]:
    problems: list[str] = []
    payloads = {
        SOURCE: GuestFile(
            (REPOSITORY / "tests/integration/video/camera_capture_probe.cpp").read_bytes()
        ),
    }
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "1G",
        "tmpfs",
        ("--camera=synthetic", "--camera-profile=tablet", f"ncpus={VCPUS}"),
        overlay_shell=True,
        engine=arguments.engine,
        payloads=payloads,
    ) as guest:
        guest.boot_to_shell(BOOT_SECONDS)
        console = ManagementConsole(guest, private)
        guest.check_command(
            f"c++ -std=c++20 -O2 -o {BINARY} {SOURCE}", BUILD_SECONDS
        )
        request = len(guest.output())
        guest.check_command("rm -f /tmp/camera-stop", 30)
        guest.check_command(
            f"({BINARY} hold </dev/null >{LOG} 2>&1 &)", 30
        )
        deadline = time.monotonic() + READY_SECONDS
        while "CAMSUSP ready" not in guest.run_command(f"cat {LOG}", 30).output:
            if time.monotonic() > deadline:
                raise RuntimeError("the camera stream never started")
            drain(guest, 0.5)
        frames, errors, _ = wait_for_progress(guest, 15, RESUME_SECONDS, "before any suspend")

        for arm in arguments.arm or ARMS:
            (suspend_by_host if arm == "host" else suspend_by_guest)(guest, console)
            try:
                frames, errors, zoom = wait_for_progress(
                    guest, frames + 15, RESUME_SECONDS, f"{arm} suspend"
                )
            except RuntimeError as error:
                problems.append(str(error))
                break
            if errors:
                problems.append(f"{arm} suspend: {errors} failed buffers")
            if zoom != 0x20000:
                problems.append(f"{arm} suspend: the metadata shows zoom {zoom:#x}")
            print(f"{arm} suspend: {frames} frames, zoom {zoom:#x}", flush=True)

        guest.check_command("touch /tmp/camera-stop", 30)
        deadline = time.monotonic() + 60
        while "CAMSUSP PASS" not in (text := guest.run_command(f"cat {LOG}", 30).output):
            if "CAMSUSP FAILED" in text or time.monotonic() > deadline:
                problems.append("the stream's verdict:\n" + text[-2000:])
                break
            drain(guest, 0.5)
        tail = guest.output()[request:]
        if RESET in tail:
            problems.append("the driver found the device reset by a suspend")
        failure = FAILURE.search(tail)
        if failure is not None:
            problems.append(
                f"the guest printed {failure[0]!r}:\n"
                + tail[max(0, failure.start() - 1500):][:5000]
            )
        if not problems:
            guest.check_command("echo alive", 30)
            if not guest.shutdown(60):
                problems.append("the guest did not power off")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--arm", action="append", choices=ARMS)
    arguments = parser.parse_args()
    arguments.emulator = arguments.emulator.resolve()
    arguments.rootfs = arguments.rootfs.resolve()

    # Short, so the management console's directory fits sun_path inside it.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-csusp-"))
    os.environ["TMPDIR"] = str(private)
    tempfile.tempdir = str(private)
    try:
        problems = run(arguments, private)
    finally:
        shutil.rmtree(private, ignore_errors=True)
    if problems:
        raise RuntimeError("\n".join(problems))
    print("the camera stream carried on across a host and a guest suspend")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
