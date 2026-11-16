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

- every workload ran to its end, in all three repetitions, in both arms: each
  row has a zero exit status and an output that is not empty, and a missing or
  truncated row fails instead of being skipped;
- every workload prints the same digest of its output in both arms and in
  every repetition: a result that depends on the component is a failure;
- the probe ran to its end (exit status, the closing "done" record), took
  every one of its scenarios, took faults, and did not skip the scenarios that
  patch covered text; it prints the same text in both arms. Registers that
  differ between two runs of the same engine (stack addresses under ASLR) are
  masked; the others are compared for each fault, and at least one must be;
- when given, the debug rows (debug_rows.py) and the share rows
  (share_rows.py) print the same records in both arms, and, with
  --diagnostics, the AOT arm reuses verdicts of the read-only file system
  across address spaces and hashes the pages of a copy on a writable one;
- every adb command and push reports success;
- without --diagnostics (the product emulator, which states nothing itself):
  residency evidence that the AOT arm touched the framework, judged from
  outside by the resident size of the framework's text in the emulator
  process (vmmap), which is a few pages with the component off and megabytes
  once it has been used. Reading the text makes it resident as well, so this
  is not evidence that rewritten instructions ran; only --diagnostics counts
  entries;
- with --diagnostics (a framework built with UML_AOT_DIAGNOSTICS, which writes
  the report named by DARWIN_UML_AOT_REPORT): the AOT arm entered rewritten
  code, and the arm with the component off did not. With --manifest the
  report is printed per library, and every library named by --expect-entered
  was entered at all: a library the guest runs but the component never
  enters is a result, not a quiet zero.

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
import json
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
from typing import Callable, NamedTuple

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
LAUNCHER = REPOSITORY / "tools" / "boot_android.py"
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import debug_rows  # noqa: E402
import share_rows  # noqa: E402
import slot_ports  # noqa: E402

DEFAULT_ADB_PORT = 5585
SETTLE_SECONDS = 20
REPORT_SECONDS = 3
# The resident size of the framework's text that separates "loaded" from
# "touched": a component that is off leaves a few pages, one that was used
# has megabytes. It is residency evidence, not proof of execution.
MIN_RESIDENT_BYTES = 1 << 20
WORKLOAD_SECONDS = 900
# The stack the memory layer's calls below a refill and a signal frame need,
# which the sampling points above them cannot see (the differential test
# paints a 64 KiB stack and measures the calls).
MIN_STACK_FREE = 16 * 1024
PROBE_SECONDS = 120
TERMINATION_SIGNALS = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)

# One line per run: <name> <run> <seconds> <digest> <exit status>. Each
# workload's output is a pure function of the image, so the digest is the same
# in every run.
WORKLOADS = r"""
T=/system/bin/toybox; D=/data/local/tmp; cd $D
F=/system/lib64/libandroid_runtime.so; G=/system/lib64/libhwui.so
$T seq 1 100000 > $D/n.txt
$T gzip -c $F > $D/w.gz
run() {
  n=$1; shift
  for i in 1 2 3; do
    $T rm -f $D/rc
    $T time -p sh -c "$*; echo \$? > $D/rc" > $D/o.out 2> $D/t.err
    r=$( $T grep real $D/t.err | $T cut -d" " -f2 )
    echo "$n $i $r $($T md5sum $D/o.out | $T cut -d" " -f1) $($T cat $D/rc 2>/dev/null || echo missing)"
  done
}
run zlib_deflate "$T gzip -c $F"
run zlib_inflate "$T gzip -cd $D/w.gz"
run crypto_sha256 "$T sha256sum $F $G"
run libc_sort "$T sort -r $D/n.txt"
run libc_grep "$T grep -c 9.*9.*9 $D/n.txt"
run pcre2_restorecon "restorecon -n -R /system/lib64 && echo done"
run binder_cmd "i=0; while [ \$i -lt 5 ]; do cmd package list packages; i=\$((i+1)); done | $T wc -l"
rm -f $D/w.gz $D/n.txt $D/o.out $D/t.err $D/rc
echo workloads-done
"""

# One row per workload and repetition, printed by the script above.
WORKLOAD_NAMES = tuple(re.findall(r"^run (\w+) ", WORKLOADS, re.MULTILINE))
REPETITIONS = (1, 2, 3)
WORKLOADS_FINISHED = "workloads-done"
# The MD5 of no output at all: a workload that failed before it printed.
EMPTY_DIGEST = "d41d8cd98f00b204e9800998ecf8427e"

# The scenarios aot_guest_probe.cpp takes, in the order it takes them.
PROBE_CASES = (
    "many-calls", "adler32-guard", "crc32-guard", "crc32_z-wholly-unmapped",
    "adler32-null", "crc32-tagged-good", "crc32-tagged-guard", "compress2",
    "inflate-output-guard", "inflate-input-guard", "inflate-output-readonly",
    "inflate-whole", "adler32-repaired", "crc32-repaired",
    "inflate-output-repaired", "patch-before", "patch-same-bytes",
    "patch-change", "patch-restore", "patch-after")


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


class Run(NamedTuple):
    seconds: float
    digest: str
    status: str


def parse_workloads(text: str) -> dict[tuple[str, int], Run]:
    runs: dict[tuple[str, int], Run] = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 5 and fields[1].isdigit():
            try:
                runs[(fields[0], int(fields[1]))] = Run(
                    float(fields[2]), fields[3], fields[4])
            except ValueError:
                raise Failure(f"unreadable workload line: {line!r}")
    return runs


def verify_workloads(output: str,
                     runs: dict[tuple[str, int], Run]) -> None:
    """Every workload ran to its end, in every repetition, and succeeded."""
    if WORKLOADS_FINISHED not in output.split():
        raise Failure(f"the workloads did not run to their end "
                      f"(no {WORKLOADS_FINISHED!r} line):\n{output}")
    expected = {(name, number) for name in WORKLOAD_NAMES
                for number in REPETITIONS}
    missing = sorted(expected - set(runs))
    if missing:
        raise Failure(f"workload rows missing: " + ", ".join(
            f"{name} #{number}" for name, number in missing))
    unknown = sorted(set(runs) - expected)
    if unknown:
        raise Failure(f"unexpected workload rows: " + ", ".join(
            f"{name} #{number}" for name, number in unknown))
    for key in sorted(expected):
        run = runs[key]
        if run.status != "0":
            raise Failure(f"{key[0]} #{key[1]} exited with "
                          f"status {run.status}")
        if run.digest == EMPTY_DIGEST:
            raise Failure(f"{key[0]} #{key[1]} printed nothing")


def verify_probe(text: str) -> int:
    """The probe ran to its end, took every scenario and its faults; returns
    the number of fault records with registers."""
    lines = text.splitlines()
    cases = {line[5:] for line in lines if line.startswith("case ")}
    absent = [case for case in PROBE_CASES if case not in cases]
    if absent:
        raise Failure(f"the probe did not take: " + ", ".join(absent))
    status = [line for line in lines if line.startswith("rc=")]
    if status != ["rc=0"] or lines[-1] != "rc=0":
        raise Failure(f"the probe's last record is not rc=0:\n{text}")
    done = [line for line in lines if line.startswith("done faults=")]
    if len(done) != 1 or not done[0][len("done faults="):].isdigit() or \
            int(done[0][len("done faults="):]) == 0:
        raise Failure(f"the probe's closing record is {done!r}")
    for problem in ("  patch cases skipped", "mmap failed"):
        if problem in lines:
            raise Failure(f"the probe reported {problem.strip()!r}")
    if any("output DIFFERS" in line for line in lines):
        raise Failure(f"a probe scenario's output differs from its "
                      "source")
    records = sum(1 for line in lines if line.startswith("  regs"))
    if records == 0:
        raise Failure(f"the probe recorded no fault registers")
    return records


def emulator_pid(root: int) -> int | None:
    """The emulator process below the launcher `root`, or None."""
    rows = subprocess.run(
        ["ps", "-axo", "pid=,ppid=,command="], text=True,
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, check=False, timeout=30).stdout
    children: dict[int, list[tuple[int, str]]] = {}
    for line in rows.splitlines():
        fields = line.split(None, 2)
        if len(fields) == 3 and fields[0].isdigit() and fields[1].isdigit():
            children.setdefault(int(fields[1]), []).append(
                (int(fields[0]), fields[2]))
    stack = [root]
    while stack:
        for pid, command in children.get(stack.pop(), []):
            if pathlib.PurePosixPath(command.split(" ")[0]).name == \
                    "uml-darwin-android17":
                return pid
            stack.append(pid)
    return None


def size_bytes(text: str) -> int:
    """A vmmap size such as 16K, 19.3M or 1.2G."""
    units = {"B": 1, "K": 1 << 10, "M": 1 << 20, "G": 1 << 30}
    if not text or text[-1] not in units or not text[:-1].replace(".", "", 1).isdigit():
        raise Failure(f"unreadable size {text!r}")
    return int(float(text[:-1]) * units[text[-1]])


def framework_resident(vmmap: str, pid: int) -> int:
    """The resident bytes of the AOT framework's text in process `pid`."""
    result = subprocess.run(
        [vmmap, "-wide", str(pid)], text=True, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
        timeout=120)
    if result.returncode != 0:
        raise Failure(f"vmmap exited {result.returncode}:\n{result.stdout}")
    for line in result.stdout.splitlines():
        match = re.match(r"__TEXT\s+\S+\s+\[\s*(\S+)\s+(\S+)\s", line)
        if match and "UmlArm64Aot" in line:
            return size_bytes(match.group(2))
    raise Failure("vmmap shows no text of the AOT framework in the emulator")


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
    """The counters of the report, flat: `enters`, `find.hit`,
    `cross_miss.unbound`, `library.3.enters`, ..."""
    values: dict[str, int] = {}
    if not path.is_file():
        return values
    for line in path.read_text(errors="replace").splitlines():
        fields = line.split()
        if not fields:
            continue
        head, rest = fields[0], fields[1:]
        prefix = ""
        if head == "library" and rest:
            prefix, rest = f"library.{rest[0]}.", rest[1:]
        elif head in ("find", "cross_miss", "refused"):
            prefix = head + "."
        elif head in ("binds", "max_retired", "stack_free"):
            rest = fields
        else:
            continue
        for key, number in zip(rest[0::2], rest[1::2]):
            if number.isdigit():
                values[prefix + key] = int(number)
    return values


def library_names(manifest: pathlib.Path) -> list[str]:
    """The guest path of each library of the framework, by report index."""
    return [library["path"] for library in
            json.loads(manifest.read_text())["libraries"]]


class Arm:
    """One boot of the guest."""

    def __init__(self, label: str, aot: bool, options: argparse.Namespace,
                 scratch: pathlib.Path) -> None:
        self.label = label
        self.aot = aot
        self.options = options
        self.scratch = scratch
        self.workloads: dict[tuple[str, int], Run] = {}
        self.probe: list[str] = []
        self.debug: dict[str, list[list[str]]] = {}
        self.share: dict[str, list[str]] = {}
        self.share_counts: dict[str, dict[str, int]] = {}
        self.report: dict[str, int] = {}
        self.resident: int | None = None

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
            result = device.run("-s", serial, "shell", WORKLOADS,
                                timeout=WORKLOAD_SECONDS)
            self.workloads = parse_workloads(result.stdout)
            verify_workloads(result.stdout, self.workloads)
            if result.returncode != 0:
                raise Failure(f"the workload command exited "
                              f"{result.returncode}:\n{result.stdout}")
            if options.probe:
                pushed = device.run("-s", serial, "push", str(options.probe),
                                    "/data/local/tmp/libaotprobe.so",
                                    timeout=60)
                if pushed.returncode != 0:
                    raise Failure(f"pushing the probe exited "
                                  f"{pushed.returncode}:\n{pushed.stdout}")
                for _ in range(2 if not self.aot else 1):
                    ran = device.run(
                        "-s", serial, "shell",
                        "cd /data/local/tmp && LD_PRELOAD=/data/local/tmp/"
                        "libaotprobe.so /system/bin/toybox true; echo rc=$?",
                        timeout=PROBE_SECONDS)
                    if ran.returncode != 0:
                        raise Failure(f"the probe command "
                                      f"exited {ran.returncode}:\n{ran.stdout}")
                    verify_probe(ran.stdout)
                    self.probe.append(ran.stdout)
            if options.debug_program:
                self.run_debug_rows(device, serial)
            if options.share_program:
                self.run_share_rows(device, serial, report)
            if guest.poll() is not None:
                raise Failure(f"the launcher exited ({guest.returncode}) "
                              "before the guest's work was done")
            if not options.diagnostics:
                pid = emulator_pid(guest.pid)
                if pid is None:
                    raise Failure("no emulator process below the launcher")
                self.resident = framework_resident(options.vmmap, pid)
            time.sleep(REPORT_SECONDS)
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


    def run_debug_rows(self, device: "Device", serial: str) -> None:
        """The debugger and crash-report rows (debug_rows.py): every mode
        several times without the component, once with it."""
        pushed = device.run("-s", serial, "push", str(self.options.debug_program),
                            debug_rows.PROGRAM, timeout=60)
        if pushed.returncode != 0:
            raise Failure(f"pushing the debug program exited "
                          f"{pushed.returncode}:\n{pushed.stdout}")
        for mode in debug_rows.MODES:
            for _ in range(1 if self.aot else debug_rows.OFF_RUNS):
                try:
                    record = debug_rows.run_mode(
                        lambda text, seconds=60: device.shell(text, timeout=seconds),
                        mode)
                except (debug_rows.DebugFailure, subprocess.TimeoutExpired) as problem:
                    raise Failure(f"{self.label}: debug row {mode}: {problem}")
                self.debug.setdefault(mode, []).append(record)


    def run_share_rows(self, device: "Device", serial: str,
                       report: pathlib.Path) -> None:
        """The shared-verification rows (share_rows.py): every mode once. In
        a diagnostics build of the component the report's counters are read
        around each mode."""
        pushed = device.run("-s", serial, "push", str(self.options.share_program),
                            share_rows.PROGRAM, timeout=60)
        if pushed.returncode != 0:
            raise Failure(f"pushing the share program exited "
                          f"{pushed.returncode}:\n{pushed.stdout}")
        counting = self.options.diagnostics and self.aot and self.options.manifest
        libz = None
        if counting:
            names = library_names(self.options.manifest)
            libz = names.index("/system/lib64/libz.so")

        def counters() -> dict[str, int]:
            # The report is written a few times a second.
            time.sleep(REPORT_SECONDS)
            return read_report(report)

        for mode in share_rows.MODES:
            before = counters() if counting else {}
            try:
                record = share_rows.run_mode(
                    lambda text, seconds=60: device.shell(text, timeout=seconds),
                    mode)
            except (share_rows.ShareFailure, subprocess.TimeoutExpired) as problem:
                raise Failure(f"{self.label}: share row {mode}: {problem}")
            self.share[mode] = record
            if counting:
                after = counters()
                self.share_counts[mode] = {
                    key: after.get(f"library.{libz}.{field}", 0) - before.get(f"library.{libz}.{field}", 0)
                    for key, field in (("shared", "shared"), ("verified", "verified"),
                                       ("mutable", "mutable"))}


def check(off: Arm, on: Arm, options: argparse.Namespace) -> None:
    names = sorted({name for name, _ in off.workloads})
    for arm in (off, on):
        for name in names:
            digests = {run.digest for (n, _), run in arm.workloads.items()
                       if n == name}
            if len(digests) != 1:
                raise Failure(f"{arm.label}: {name} printed {len(digests)} "
                              "different outputs")
    for name in names:
        before = {run.digest for (n, _), run in off.workloads.items()
                  if n == name}
        after = {run.digest for (n, _), run in on.workloads.items()
                 if n == name}
        if before != after:
            raise Failure(f"{name}: the output differs with the component on")
        seconds = [statistics.median(run.seconds for (n, _), run in
                                     arm.workloads.items() if n == name)
                   for arm in (off, on)]
        print(f"workload {name}: off {seconds[0]:.3f} s, on {seconds[1]:.3f} s, "
              f"output {next(iter(before))}")
    if options.probe:
        if len(off.probe) != 2 or len(on.probe) != 1:
            raise Failure(f"the probe ran {len(off.probe)} times without the "
                          f"component and {len(on.probe)} with it, not 2 and 1")
        checked = compare_probe(off.probe[0], off.probe[1], on.probe[0])
        if checked == 0:
            raise Failure("the probe compared no registers")
        print(f"probe: {checked} registers compared, identical")
    if options.debug_program:
        for mode in debug_rows.MODES:
            if len(off.debug.get(mode, [])) != debug_rows.OFF_RUNS or len(on.debug.get(mode, [])) != 1:
                raise Failure(f"debug row {mode} ran {len(off.debug.get(mode, []))} times "
                              f"without the component and {len(on.debug.get(mode, []))} with it, "
                              f"not {debug_rows.OFF_RUNS} and 1")
            try:
                compared, masked = debug_rows.compare(mode, off.debug[mode], on.debug[mode][0])
            except debug_rows.DebugFailure as problem:
                raise Failure(f"debug row {problem}")
            print(f"debug row {mode}: {len(on.debug[mode][0])} lines, "
                  f"{compared} fields compared, {masked} masked, identical")
    if options.share_program:
        fork_shared = None
        for mode in share_rows.MODES:
            if mode not in off.share or mode not in on.share:
                raise Failure(f"share row {mode} did not run in both arms")
            try:
                lines = share_rows.compare(mode, off.share[mode], on.share[mode])
                print(f"share row {mode}: {lines} lines, identical")
                for note in share_rows.informational(on.share[mode]):
                    print("  " + note)
                if on.share_counts:
                    print(share_rows.check_counts(mode, on.share_counts[mode], fork_shared))
            except share_rows.ShareFailure as problem:
                raise Failure(f"share row {problem}")
            if mode == "fork" and on.share_counts:
                fork_shared = on.share_counts[mode].get("shared", 0)
    if not options.diagnostics:
        assert off.resident is not None and on.resident is not None
        if on.resident < MIN_RESIDENT_BYTES:
            raise Failure("no residency evidence that the AOT arm used the "
                          "framework (its text is "
                          f"{on.resident} bytes resident)")
        if off.resident >= MIN_RESIDENT_BYTES:
            raise Failure("the framework's text is resident with the "
                          f"component off ({off.resident} bytes resident)")
        print(f"framework text resident (residency evidence): off "
              f"{off.resident >> 10} KiB, on {on.resident >> 10} KiB")
    if options.diagnostics:
        if on.report.get("enters", 0) == 0:
            raise Failure("the AOT arm never entered rewritten code")
        if off.report.get("enters", 0) != 0:
            raise Failure("rewritten code ran with the component off")
        print("report: enters {enters}, steps {steps}, poisoned {poisoned}, "
              "refills {refills}".format(**{k: on.report.get(k, 0) for k in (
                  "enters", "steps", "poisoned", "refills")}))
        left = on.report.get("stack_free")
        if left is not None:
            print(f"stack: the least a thread had left where the runtime is deepest "
                  f"(entry, refill, page verification): {left} bytes of "
                  f"{on.report.get('stack_size', 0)}")
            if left < MIN_STACK_FREE:
                raise Failure(f"a thread had {left} bytes of stack left in the runtime, "
                              f"less than the {MIN_STACK_FREE} the memory layer's own calls "
                              "and a signal frame need")
        report_libraries(on.report, options)


def report_libraries(report: dict[str, int],
                     options: argparse.Namespace) -> None:
    """Print what happened to each library and require the expected ones to
    have been entered."""
    if options.manifest is None:
        return
    names = library_names(options.manifest)
    print("find: " + ", ".join(f"{key[5:]} {value}" for key, value in
                               sorted(report.items())
                               if key.startswith("find.")))
    print("cross-library misses: " + ", ".join(
        f"{key[11:]} {value}" for key, value in sorted(report.items())
        if key.startswith("cross_miss.")))
    entered = {}
    for index, name in enumerate(names):
        row = {key.split(".", 2)[2]: value for key, value in report.items()
               if key.startswith(f"library.{index}.")}
        entered[pathlib.PurePosixPath(name).name] = row.get("enters", 0)
        if row:
            print(f"library {index} {name}: " + ", ".join(
                f"{key} {value}" for key, value in row.items() if value))
    for wanted in options.expect_entered:
        if wanted not in entered:
            raise Failure(f"--expect-entered names {wanted}, which the "
                          "framework does not cover")
        if entered[wanted] == 0:
            raise Failure(f"the AOT arm never entered {wanted}")


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
    parser.add_argument("--debug-program", type=pathlib.Path,
                        help="the built guest debug program "
                        "(libaotdebug.so): breakpoints, single steps, a "
                        "thread nobody stops, and crash reports")
    parser.add_argument("--share-program", type=pathlib.Path,
                        help="the built guest share program "
                        "(libaotshare.so): the rewritten libz.so run in many "
                        "processes, a copy of it, a copied page, and a "
                        "library whose page cache was dropped")
    parser.add_argument("--vmmap", default="vmmap",
                        help="the tool that lists the emulator's regions "
                        "(the product mode reads the framework's residency)")
    parser.add_argument("--diagnostics", action="store_true",
                        help="the framework is a diagnostics build")
    parser.add_argument("--manifest", type=pathlib.Path,
                        help="the framework's manifest.json, to name the "
                        "libraries of the report")
    parser.add_argument("--expect-entered", default="",
                        type=lambda text: [n for n in text.split(",") if n],
                        help="comma-separated library file names the AOT arm "
                        "must have entered (with --diagnostics --manifest)")
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
