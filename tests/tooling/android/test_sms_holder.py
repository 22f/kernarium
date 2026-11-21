#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The text message holder on the host, without a guest.

android/lineage-24/overlay/sms/listener/main.cpp is the native daemon that keeps vsock port 5560 from boot, starts
the service through a control property when a connection waits, and passes it the listening socket over its own
unix socket. This test compiles that file as it is (main renamed, -Wall -Wextra -Werror, C++20, as the image
build does) against the stand-ins in tests/tooling/android/sms/holder for the bionic pieces macOS lacks, links it
with holder_test.cpp, and runs the scenarios there: the handoff socket as init makes it (bound, NOT listening),
the vsock port as a unix socket at a path of the test, the property set, the peer's user and domain. What it
holds that no review catches: the holder must listen on the handoff socket itself, and stays in its loop.
Skipped (status 77) where the host has no C++ compiler.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
HOLDER = REPOSITORY / "android" / "lineage-24" / "overlay" / "sms" / "listener" / "main.cpp"
SUPPORT = HERE / "sms" / "holder"
SKIP = 77
SCENARIOS = ("S1", "S2", "S3", "S4", "S5", "S6", "S7", "S8", "S9", "S10", "S11")


def compiler() -> str | None:
    path = shutil.which("clang++")
    if path is None:
        return None
    probe = subprocess.run([path, "--version"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=60, check=False)
    return path if probe.returncode == 0 else None


def run(command: list[str], timeout: int) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, timeout=timeout, check=False)


def main() -> int:
    clang = compiler()
    if clang is None:
        print("SKIP: no C++ compiler (clang++) on PATH")
        return SKIP
    flags = ["-std=c++20", "-Wall", "-Wextra", "-Werror", "-include", str(SUPPORT / "prelude.h"),
             f"-I{SUPPORT / 'stubs'}"]
    with tempfile.TemporaryDirectory(prefix="sms-holder-build-") as build:
        holder = pathlib.Path(build) / "holder.o"
        driver = pathlib.Path(build) / "driver.o"
        binary = pathlib.Path(build) / "holder_test"
        for command in ([clang, *flags, "-Dmain=holder_main", "-c", str(HOLDER), "-o", str(holder)],
                        [clang, *flags, "-c", str(SUPPORT / "holder_test.cpp"), "-o", str(driver)],
                        [clang, str(holder), str(driver), "-o", str(binary), "-pthread"]):
            built = run(command, 300)
            if built.returncode != 0:
                print(f"FAIL: {' '.join(command[:2])} ...\n{built.stdout}", file=sys.stderr)
                return 1
        # A short path: a unix socket's path is at most 104 bytes on macOS.
        with tempfile.TemporaryDirectory(prefix="sms-h-", dir="/tmp") as scratch:
            ran = run([str(binary), scratch], 120)
    for line in ran.stdout.splitlines():
        if line.startswith(("PASS", "FAIL")):
            print(line)
    if ran.returncode != 0:
        print(f"FAIL: the holder's scenarios exited {ran.returncode}:\n{ran.stdout[-3000:]}", file=sys.stderr)
        return 1
    missing = [name for name in SCENARIOS if f"PASS {name}:" not in ran.stdout]
    if missing or not ran.stdout.rstrip().endswith("PASS"):
        print(f"FAIL: scenarios not passed: {missing}\n{ran.stdout[-3000:]}", file=sys.stderr)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
