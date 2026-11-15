#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run an Android guest with ahead-of-time code off and on and compare the work.

The guest is the release on a fresh data disk, started with the macOS launcher
(tools/boot_android.py) twice: once as the engine runs it, once with
DARWIN_UML_AOT=1 (docs/architecture.md, "Ahead-of-time code"). In each the
runner executes one workload per family of the libraries the framework covers
(zlib, libcrypto, libc, libpcre2 through libselinux, libbinder) and, when
given, the guest probe (aot_guest_probe.cpp), which takes faults inside
rewritten libz code and patches covered text at run time.

Checked:

- every workload prints the same digest of its output in both arms and in
  every repetition: a result that depends on the component is a failure;
- the probe prints the same text in both arms. Registers that differ between
  two runs of the same engine (stack addresses under ASLR) are masked; the
  others are compared for each fault;
- with --diagnostics (a framework built with UML_AOT_DIAGNOSTICS, which writes
  the report named by DARWIN_UML_AOT_REPORT): the AOT arm entered rewritten
  code, and the arm with the component off did not.

The seconds each workload took in each arm are printed, not judged: speed is
measured by the campaign that owns it, on a quiet host.

Every wait has a deadline; the guest's launcher is stopped through its own
process on any exit, and the guest is reached through the shared adb server,
always named with `-s`.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time
from typing import Callable

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
LAUNCHER = REPOSITORY / "tools" / "boot_android.py"
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import slot_ports  # noqa: E402

DEFAULT_ADB_PORT = 5585
SETTLE_SECONDS = 20
WORKLOAD_SECONDS = 900
PROBE_SECONDS = 120
TERMINATION_SIGNALS = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)

# One line per run: <name> <run> <seconds> <digest>. Each workload's output is
# a pure function of the image, so the digest is the same in every run.
WORKLOADS = r"""
T=/system/bin/toybox; D=/data/local/tmp; cd $D
F=/system/lib64/libandroid_runtime.so; G=/system/lib64/libhwui.so
$T seq 1 100000 > $D/n.txt
$T gzip -c $F > $D/w.gz
run() {
  n=$1; shift
  for i in 1 2 3; do
    r=$( { $T time -p sh -c "$*" > $D/o.out; } 2>&1 | $T grep real | $T cut -d" " -f2 )
    echo "$n $i $r $($T md5sum $D/o.out | $T cut -d" " -f1)"
  done
}
run zlib_deflate "$T gzip -c $F"
run zlib_inflate "$T gzip -cd $D/w.gz"
run crypto_sha256 "$T sha256sum $F $G"
run libc_sort "$T sort -r $D/n.txt"
run libc_grep "$T grep -c 9.*9.*9 $D/n.txt"
run pcre2_restorecon "restorecon -n -R /system/lib64; echo done"
run binder_cmd "i=0; while [ \$i -lt 5 ]; do cmd package list packages; i=\$((i+1)); done | $T wc -l"
rm -f $D/w.gz $D/n.txt $D/o.out
"""


class Failure(RuntimeError):
    pass


def ignore_termination() -> None:
    for name in TERMINATION_SIGNALS:
        signal.signal(name, signal.SIG_IGN)


def terminate_on_signal(signum: int, _frame: object) -> None:
    ignore_termination()
    raise SystemExit(128 + signum)


class Device:
    """The guest as the host's adb sees it."""

    def __init__(self, adb: str, serial: str) -> None:
        self.adb = adb
        self.serial = serial

    def run(self, *arguments: str, timeout: int = 30) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, *arguments], text=True, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
            check=False)

    def connect(self) -> bool:
        try:
            self.run("connect", self.serial, timeout=15)
            state = self.run("-s", self.serial, "get-state", timeout=15)
        except subprocess.TimeoutExpired:
            return False
        return state.returncode == 0 and state.stdout.strip() == "device"

    def shell(self, command: str, timeout: int = 30) -> str:
        result = self.run("-s", self.serial, "shell", command, timeout=timeout)
        return result.stdout

    def getprop(self, name: str) -> str:
        return self.shell(f"getprop {name}").strip()


def wait_for(description: str, probe: Callable[[], bool], seconds: int,
             guest: subprocess.Popen | None) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if guest is not None and guest.poll() is not None:
            raise Failure(f"the launcher exited ({guest.returncode}) while "
                          f"waiting for {description}")
        try:
            if probe():
                return
        except subprocess.TimeoutExpired:
            pass
        time.sleep(1)
    raise Failure(f"{description}: not within {seconds} s")


def stop_launcher(guest: subprocess.Popen) -> None:
    if guest.poll() is None:
        guest.send_signal(signal.SIGTERM)
        try:
            guest.wait(timeout=120)
        except subprocess.TimeoutExpired:
            guest.kill()
            guest.wait(timeout=30)


def parse_workloads(text: str) -> dict[tuple[str, int], tuple[float, str]]:
    runs: dict[tuple[str, int], tuple[float, str]] = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[1].isdigit():
            try:
                runs[(fields[0], int(fields[1]))] = (float(fields[2]), fields[3])
            except ValueError:
                raise Failure(f"unreadable workload line: {line!r}")
    return runs


def register_fields(line: str) -> dict[str, str]:
    return dict(re.findall(r"(x\d\d)=(\S+)", line))


def compare_probe(first: str, second: str, aot: str) -> int:
    """Lines of the engine's two runs and the AOT run; returns the number of
    registers compared. A register that differs between the two engine runs
    is masked."""
    one, two, three = (text.splitlines() for text in (first, second, aot))
    if not (len(one) == len(two) == len(three)):
        raise Failure(f"the probe printed {len(one)}, {len(two)} and "
                      f"{len(three)} lines")
    checked = 0
    for number, (a, b, c) in enumerate(zip(one, two, three), 1):
        if a.startswith("  regs"):
            ra, rb, rc = register_fields(a), register_fields(b), register_fields(c)
            for name, value in ra.items():
                if value != rb.get(name):
                    continue
                checked += 1
                if value != rc.get(name):
                    raise Failure(f"probe line {number}: {name} is {value} "
                                  f"without the component, {rc.get(name)} with it")
        elif a != c:
            raise Failure(f"probe line {number}: {a!r} without the component, "
                          f"{c!r} with it")
    return checked


def read_report(path: pathlib.Path) -> dict[str, int]:
    values: dict[str, int] = {}
    if path.is_file():
        for line in path.read_text(errors="replace").splitlines():
            fields = line.split()
            if fields and fields[0] == "binds":
                values = {fields[i]: int(fields[i + 1])
                          for i in range(0, len(fields) - 1, 2)}
                break
    return values


class Arm:
    """One boot of the guest."""

    def __init__(self, label: str, aot: bool, options: argparse.Namespace,
                 scratch: pathlib.Path) -> None:
        self.label = label
        self.aot = aot
        self.options = options
        self.scratch = scratch
        self.workloads: dict[tuple[str, int], tuple[float, str]] = {}
        self.probe: list[str] = []
        self.report: dict[str, int] = {}

    def run(self) -> None:
        options = self.options
        report = self.scratch / f"report-{self.label}.txt"
        environment = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}
        environment.pop("DARWIN_UML_AOT", None)
        environment.pop("DARWIN_UML_AOT_REPORT", None)
        if self.aot:
            environment["DARWIN_UML_AOT"] = "1"
        if options.diagnostics:
            environment["DARWIN_UML_AOT_REPORT"] = str(report)
        serial = f"127.0.0.1:{options.adb_port}"
        command = [sys.executable, str(LAUNCHER),
                   "--instance", str(self.scratch / f"instance-{self.label}"),
                   "--adb", options.adb, "--adb-port", str(options.adb_port),
                   "--display", "offscreen", "--cpus", str(options.cpus),
                   "--boot-timeout", str(options.boot_timeout)]
        if options.release:
            command += ["--release", str(options.release)]
        if options.emulator:
            command += ["--emulator", str(options.emulator)]
        log_path = self.scratch / f"launcher-{self.label}.log"
        guest: subprocess.Popen | None = None
        device = Device(options.adb, serial)
        try:
            with open(log_path, "wb") as log:
                guest = subprocess.Popen(
                    command, stdin=subprocess.DEVNULL, stdout=log,
                    stderr=subprocess.STDOUT, env=environment)
            wait_for("the guest to be booted",
                     lambda: device.connect() and
                     device.getprop("sys.boot_completed") == "1",
                     options.boot_timeout, guest)
            time.sleep(SETTLE_SECONDS)
            output = device.run("-s", serial, "shell", WORKLOADS,
                                timeout=WORKLOAD_SECONDS).stdout
            self.workloads = parse_workloads(output)
            if not self.workloads:
                raise Failure(f"{self.label}: no workload ran:\n{output}")
            if options.probe:
                device.run("-s", serial, "push", str(options.probe),
                           "/data/local/tmp/libaotprobe.so", timeout=60)
                for _ in range(2 if not self.aot else 1):
                    self.probe.append(device.run(
                        "-s", serial, "shell",
                        "cd /data/local/tmp && LD_PRELOAD=/data/local/tmp/"
                        "libaotprobe.so /system/bin/toybox true; echo rc=$?",
                        timeout=PROBE_SECONDS).stdout)
            time.sleep(3)
            self.report = read_report(report)
        except (Failure, subprocess.TimeoutExpired, OSError) as problem:
            tail = ""
            if log_path.is_file():
                tail = "\nlauncher log tail:\n" + "\n".join(
                    log_path.read_text(errors="replace").splitlines()[-30:])
            raise Failure(f"{self.label}: {problem}{tail}") from problem
        finally:
            ignore_termination()
            if guest is not None:
                stop_launcher(guest)
                subprocess.run([options.adb, "disconnect", serial],
                               stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=30,
                               check=False)
            for name in TERMINATION_SIGNALS:
                signal.signal(name, terminate_on_signal)


def check(off: Arm, on: Arm, options: argparse.Namespace) -> None:
    names = sorted({name for name, _ in off.workloads})
    for arm in (off, on):
        for name in names:
            digests = {digest for (n, _), (_, digest) in arm.workloads.items()
                       if n == name}
            if len(digests) != 1:
                raise Failure(f"{arm.label}: {name} printed {len(digests)} "
                              "different outputs")
    for name in names:
        before = {d for (n, _), (_, d) in off.workloads.items() if n == name}
        after = {d for (n, _), (_, d) in on.workloads.items() if n == name}
        if before != after:
            raise Failure(f"{name}: the output differs with the component on")
        seconds = [statistics.median(s for (n, _), (s, _) in arm.workloads.items()
                                     if n == name) for arm in (off, on)]
        print(f"workload {name}: off {seconds[0]:.3f} s, on {seconds[1]:.3f} s, "
              f"output {next(iter(before))}")
    if options.probe:
        checked = compare_probe(off.probe[0], off.probe[1], on.probe[0])
        print(f"probe: {checked} registers compared, identical")
    if options.diagnostics:
        if on.report.get("enters", 0) == 0:
            raise Failure("the AOT arm never entered rewritten code")
        if off.report.get("enters", 0) != 0:
            raise Failure("rewritten code ran with the component off")
        print("report: enters {enters}, steps {steps}, poisoned {poisoned}, "
              "refills {refills}".format(**{k: on.report.get(k, 0) for k in (
                  "enters", "steps", "poisoned", "refills")}))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--release", type=pathlib.Path,
                        help="release directory (default: the accepted release)")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--emulator", type=pathlib.Path,
                        help="the Android macOS executable")
    parser.add_argument("--adb-port", type=int, default=slot_ports.slot_port(
        slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT))
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--boot-timeout", type=int, default=900)
    parser.add_argument("--probe", type=pathlib.Path,
                        help="the built guest probe (libaotprobe.so)")
    parser.add_argument("--diagnostics", action="store_true",
                        help="the framework is a diagnostics build")
    return parser


def main() -> int:
    options = build_parser().parse_args()
    options.adb = shutil.which(options.adb) or options.adb
    for name in TERMINATION_SIGNALS:
        signal.signal(name, terminate_on_signal)
    with tempfile.TemporaryDirectory(prefix="aot-guest-") as scratch_name:
        scratch = pathlib.Path(scratch_name)
        try:
            off = Arm("off", False, options, scratch)
            on = Arm("on", True, options, scratch)
            off.run()
            on.run()
            check(off, on, options)
            print("PASS")
            return 0
        except Failure as problem:
            print(f"FAIL: {problem}", file=sys.stderr)
            return 1


if __name__ == "__main__":
    raise SystemExit(main())
