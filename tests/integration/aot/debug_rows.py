# SPDX-License-Identifier: GPL-2.0-or-later
"""The debugger and crash-report rows of the AOT guest runner.

aot_guest_debug.cpp (loaded into a stock process with LD_PRELOAD) crashes and
debugs inside the rewritten libz.so and libc.so. Each mode runs twice without
the component and once with it. A mode that crashes is judged by the
tombstone the crash reporter wrote; a mode that debugs prints its record. The
runs are then compared line by line: a line that names something the engine
itself chooses differently from run to run (a process id, a time, a random
value) is masked field by field where the runs without the component differ,
and every other line must be the same in all of them.

The program runs with address-space randomization off (the guest kernel's
layout), but the image's userspace still places libraries and stacks at
random: a register value that points into a mapping is therefore named by it
(the file and the offset in it, the mapping's name and the offset in it, or
an unnamed mapping's size, protection and place among the mappings of that
shape, and the offset in it) before the runs are compared, and such a name that
differs between two runs of the engine alone fails the row instead of being
masked. Only values that name nothing can be masked, and only a few.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import re
import time
from typing import Callable

PROGRAM = "/data/local/tmp/libaotdebug.so"
# Runs of each mode without the component: a field is masked when any two differ,
# so the more runs, the fewer fields that differ by chance in two runs only.
OFF_RUNS = 3
# crash-sibling prints a register that counts how far a sibling thread got when
# the process crashed. Its spread depends on the host's load: 19 values in 40
# runs on one guest, and in another session one value in half of its runs.
# With three baselines the chance that all show one value and a run with the
# component then differs is about 7 in 100 at that concentration; with eight
# it is about 2 in 1000 (the sum over values of p^n (1 - p)), and with the
# wider spread it is negligible either way. Each run takes seconds.
OFF_RUNS_BY_MODE = {"crash-sibling": 8}


def off_runs(mode: str) -> int:
    return OFF_RUNS_BY_MODE.get(mode, OFF_RUNS)
# The program prints this once address-space randomization is off for it.
RANDOMIZATION_OFF = "randomization: off"
# The most fields of one mode the engine's own runs may differ in once the
# values that point into a mapping are named: process ids and random values.
MAX_MASKED = 16
MODES = ("crash-memcpy", "crash-cross", "crash-abort", "crash-sibling", "ptrace", "sibling")
# The shell's exit status of a process that died of the signal.
CRASH_STATUS = {"crash-memcpy": 139, "crash-cross": 139, "crash-abort": 134, "crash-sibling": 134}
MODE_SECONDS = {"crash-memcpy": 120, "crash-cross": 120, "crash-abort": 120, "crash-sibling": 180,
                "ptrace": 420, "sibling": 360}
TOMBSTONE_SECONDS = 90


class DebugFailure(RuntimeError):
    pass


def command(mode: str) -> str:
    return (f"cd /data/local/tmp && AOT_DEBUG_MODE={mode} LD_PRELOAD={PROGRAM} "
            f"/system/bin/toybox true; echo rc=$?")


def split_blocks(text: str) -> list[str]:
    """A tombstone's blocks: the crashing thread, then one per other thread."""
    return re.split(r"^--- --- ---.*$", text, flags=re.MULTILINE)


MAP_LINE = re.compile(
    r"^[ \t]+([0-9a-f]{8})'?([0-9a-f]{8})-([0-9a-f]{8})'?([0-9a-f]{8}) (\S+)[ \t]+([0-9a-f]+)[ \t]+[0-9a-f]+"
    r"(?:[ \t]+(\S.*?))?(?: \(BuildId: \w+\))?[ \t]*$", re.MULTILINE)


def parse_maps(text: str) -> list[tuple[int, int, int, str, str]]:
    """The tombstone's memory map: (low, high, file offset, name, protection)."""
    return [(int(m.group(1) + m.group(2), 16), int(m.group(3) + m.group(4), 16) + 1,
             int(m.group(6), 16), m.group(7) or "", m.group(5)) for m in MAP_LINE.finditer(text)]


def name_value(value: int, maps: list[tuple[int, int, int, str, str]]) -> str | None:
    """Where a value points, if into a mapping, as the run does not choose it:
    a file's name and the offset in the file, a named anonymous mapping's name
    and the offset in it, or an unnamed mapping's size and protection, its
    place among the unnamed mappings of that size and protection (counted
    from the lowest address; `maps` is in address order), and the offset in
    it. Where the mapping lies is the run's choice and is not part of the
    name; which mapping it is, and where in it, are the guest's."""
    shapes: dict[tuple[int, str], int] = {}
    for low, high, offset, name, protection in maps:
        ordinal = 0
        if not name:
            ordinal = shapes.get((high - low, protection), 0)
            shapes[(high - low, protection)] = ordinal + 1
        if low <= value < high:
            if name.startswith("/"):
                return f"@{name.rsplit('/', 1)[1]}+{offset + value - low:#x}"
            if name:
                # A thread's stack is named by its thread id.
                return f"@{re.sub(r':[0-9]+]$', ':*]', name)}+{value - low:#x}"
            return f"@anon[{high - low:#x},{protection}]#{ordinal}+{value - low:#x}"
    return None


def name_registers(line: str, maps: list[tuple[int, int, int, str, str]]) -> str:
    def replace(found: re.Match) -> str:
        return name_value(int(found.group(0), 16), maps) or found.group(0)
    return re.sub(r"\b[0-9a-f]{16}\b", replace, line)


def digest_tombstone(text: str) -> list[str]:
    """The lines of a tombstone that describe the crash, one per line, tagged
    by what may vary: `~` lines are compared field by field (registers,
    process ids), `=` lines must be equal in every run. A register that
    points into a mapping is named by it (name_value)."""
    blocks = split_blocks(text)
    maps = parse_maps(text)
    out: list[str] = []
    section = "head"
    first_row = 0
    for line in blocks[0].splitlines():
        if line.startswith(("Timestamp:", "Process uptime:")):
            continue
        if line.startswith("memory near "):
            section = "keep" if line.startswith(("memory near pc", "memory near lr")) else "skip"
            first_row = 0
            if section == "keep":
                out.append("= " + re.sub(r"\(.*", "", line).strip())
            continue
        if line.startswith("memory map") or line.startswith("open files"):
            section = "skip"
            continue
        if section == "skip":
            continue
        if not line.strip():
            if section == "keep":
                section = "head"
            continue
        if line.startswith("signal "):
            # The page of a fault address is the process's layout; where in
            # the page it lies is the guest's.
            out.append("= " + re.sub(r"fault addr 0x[0-9a-f]*([0-9a-f]{3}) ", r"fault addr page+0x\1 ", line))
            continue
        if section == "keep":
            row = re.match(r"(\s+)([0-9a-f]{16}) (.*)", line)
            if row:
                # The words of code at a place in the dump: where the dump
                # starts is the run's address, not the guest's.
                address = int(row.group(2), 16)
                first_row = first_row or address
                out.append(f"= {row.group(1)}+{address - first_row:#x} {row.group(3)}")
                continue
        if re.match(r"\s+(x\d+|lr|esr)\s", line):
            out.append("~ " + name_registers(line, maps))
            continue
        out.append(("~ " if line.startswith("pid:") else "= ") + line)
    for block in blocks[1:]:
        for line in block.splitlines():
            match = re.match(r"pid: \d+, ppid: \d+, tid: \d+, name: (\S+)", line)
            if match:
                out.append(f"= thread {match.group(1)}")
                continue
            frame = re.match(r"\s+#(\d+) pc [0-9a-f]+\s+(\S+)(?: \((.*?)(?:\+\d+)?\))?(?: \(BuildId: \w+\))?$", line)
            if frame:
                # Where a thread was caught while it ran is not a fact of the
                # guest: its first frame keeps the library and nothing else.
                if frame.group(1) == "00":
                    out.append(f"= thread frame 00 {frame.group(2)}")
                else:
                    out.append(f"= thread frame {frame.group(1)} {frame.group(2)} {frame.group(3) or ''}")
    return out


def fetch_tombstone(shell: Callable[..., str]) -> str:
    """The tombstone of the process that just died, by the process id the
    crash log names."""
    deadline = time.monotonic() + TOMBSTONE_SECONDS
    pid = None
    while time.monotonic() < deadline:
        log = shell("logcat -b crash -d -v raw", 30)
        found = re.search(r"Fatal signal \d+ .* pid (\d+) \(", log)
        if found and "pbtombstone" in log:
            pid = found.group(1)
            break
        time.sleep(2)
    if pid is None:
        raise DebugFailure("the crash reporter wrote no tombstone")
    # The reporter writes the other threads after the crashing one: the file is
    # complete when its process is gone.
    while time.monotonic() < deadline:
        if not shell("pidof crash_dump64 crash_dump32", 30).strip():
            break
        time.sleep(2)
    time.sleep(2)
    while time.monotonic() < deadline:
        names = shell(f"grep -l '^pid: {pid},' /data/tombstones/tombstone_?? 2>/dev/null", 30).split()
        if names:
            return shell(f"cat {names[-1]}", 60)
        time.sleep(2)
    raise DebugFailure(f"no tombstone file names process {pid}")


def run_mode(shell: Callable[..., str], mode: str) -> list[str]:
    """One run of a mode; returns its record, checked for what it must show."""
    if mode in CRASH_STATUS:
        shell("logcat -b crash -c", 30)
    output = shell(command(mode), MODE_SECONDS[mode])
    lines = output.splitlines()
    status = [line for line in lines if line.startswith("rc=")]
    if not status:
        raise DebugFailure(f"{mode}: no exit status in:\n{output}")
    expected = CRASH_STATUS.get(mode, 0)
    if status[-1] != f"rc={expected}":
        raise DebugFailure(f"{mode}: {status[-1]}, expected rc={expected}:\n{output}")
    failed = [line for line in lines
              if line.startswith("FAIL") or re.search(r"(=|: )NO\b", line) or "IS NOT" in line]
    if failed:
        raise DebugFailure(f"{mode} reported:\n" + "\n".join(failed) + f"\n--- output:\n{output}")
    if lines.count(RANDOMIZATION_OFF) != 1:
        raise DebugFailure(f"{mode}: the program did not run with address-space randomization off "
                           f"(it prints {RANDOMIZATION_OFF!r} once when it does):\n{output}")
    if mode in CRASH_STATUS:
        record = digest_tombstone(fetch_tombstone(shell))
        verify_tombstone(mode, record)
        return record
    record = [line for line in lines if not line.startswith("rc=")]
    verify_record(mode, record)
    return record


def verify_tombstone(mode: str, record: list[str]) -> None:
    text = "\n".join(record)
    frames = [line for line in record if re.match(r"= \s+#\d\d pc ", line)]
    if mode == "crash-memcpy":
        needed = ("SIGSEGV", "SEGV_ACCERR", "libc.so (__mem", "libz.so (updatewindow", "libz.so (inflateSetDictionary", "= memory near pc")
    elif mode == "crash-cross":
        needed = ("SIGSEGV", "SEGV_ACCERR", "libc.so (__memcmp", "= memory near pc")
    else:
        needed = ("SIGABRT", "libc.so (abort+")
    for token in needed:
        if token not in text:
            raise DebugFailure(f"{mode}: the tombstone lacks {token!r}:\n{text}")
    if len(frames) < {"crash-memcpy": 4, "crash-cross": 2}.get(mode, 2):
        raise DebugFailure(f"{mode}: only {len(frames)} frames in the tombstone:\n{text}")
    if mode in ("crash-memcpy", "crash-cross"):
        # A read that runs into an unmapped page is a read fault in it.
        if not re.search(r"fault addr page\+0x[0-9a-f]{3} \(read\)", text):
            raise DebugFailure(f"{mode}: no read fault address in the tombstone:\n{text}")
    if mode == "crash-sibling":
        other = [line for line in record if line.startswith("= thread frame 00 ")]
        if not other or not any("libz.so" in line for line in other):
            raise DebugFailure(f"{mode}: no frame of the running thread in libz.so:\n{text}")


def verify_record(mode: str, record: list[str]) -> None:
    text = "\n".join(record)
    if f"done mode={mode}" not in record:
        raise DebugFailure(f"{mode}: no closing record:\n{text}")
    if mode == "ptrace":
        hits = sum(1 for line in record if line.endswith("at_breakpoint=yes"))
        for needed in ("changed length: result is the library's answer",
                       "changed pc: result is adler32's answer",
                       "detached tracee status=0"):
            if needed not in text:
                raise DebugFailure(f"ptrace: missing {needed!r}:\n{text}")
        steps = sum(1 for line in record if line.startswith("step "))
        library_changes = re.search(r"after \d+ steps: (\d+) library changes, (\d+) instructions in libc", text)
        if hits < 5 or steps < 40 or library_changes is None or int(library_changes.group(1)) < 4 \
                or int(library_changes.group(2)) < 16 or "tracee done sum=" not in text:
            raise DebugFailure(f"ptrace: the record is incomplete ({hits} hits, {steps} steps):\n{text}")
    if mode == "sibling":
        rounds = [line for line in record if "breakpoint hit by the running thread: yes by_sibling=yes pc_at_breakpoint=yes in_libz=yes" in line]
        goes_on = [line for line in record if line.endswith("the thread runs on after the restore: yes")]
        if len(rounds) != 2 or len(goes_on) != 2:
            raise DebugFailure(f"sibling: {len(rounds)} hit rounds and {len(goes_on)} continuations, not 2 and 2:\n{text}")


# Fields of a register record that never differ between two runs of one
# engine, whatever they hold.
STRICT_FIELDS = ("pc=", "sp=", "nzcv=")
# A value named by what it points into (a library, a buffer, a mapping and an
# offset in it): never a value the run chooses, so never masked.
SYMBOLIC = re.compile(r"(?:\w+=)?[@A-Za-z\[][^\s=]*\+(?:0x[0-9a-f]+|0)")


def maskable(line: str) -> bool:
    return line.startswith("~ ") or line.startswith("  regs")


def compare(mode: str, off_runs: list[list[str]], with_it: list[str]) -> tuple[int, int]:
    """The records of the runs without the component and of the run with it;
    returns (fields compared, fields masked). A field is masked when two of
    the runs without the component differ in it and it names nothing (a
    process id, a random value); a pc, sp or nzcv that differs, a named location
    that differs and that the run with the component shows differently from
    every run without it, or more than MAX_MASKED masked fields, fail the row."""
    if len({len(run) for run in off_runs + [with_it]}) != 1:
        raise DebugFailure(f"{mode}: the runs have "
                           f"{[len(run) for run in off_runs + [with_it]]} lines")
    compared = masked = 0
    for number, rows in enumerate(zip(*off_runs, with_it), 1):
        engine, third = rows[:-1], rows[-1]
        if maskable(engine[0]):
            columns = [row.split() for row in rows]
            if len({len(fields) for fields in columns}) != 1:
                raise DebugFailure(f"{mode} line {number}: the fields differ in number:\n" + "\n".join(rows))
            for fields in zip(*columns):
                values = set(fields[:-1])
                if len(values) > 1:
                    if fields[0].startswith(STRICT_FIELDS):
                        raise DebugFailure(f"{mode} line {number}: the engine's own runs differ in "
                                           f"{sorted(values)}:\n" + "\n".join(rows))
                    if any(SYMBOLIC.fullmatch(v) for v in values) and fields[-1] not in values:
                        # A value that names a place and still varies between the
                        # engine's own runs (a register left over from a race in the
                        # guest) is judged by what the engine itself showed: the run
                        # with the component may show any of those values, no other.
                        raise DebugFailure(f"{mode} line {number}: the engine's own runs differ in "
                                           f"{sorted(values)} and the run with the component shows "
                                           f"{fields[-1]!r}:\n" + "\n".join(rows))
                    masked += 1
                    continue
                compared += 1
                if fields[0] != fields[-1]:
                    raise DebugFailure(f"{mode} line {number}: {fields[0]!r} without the component, "
                                       f"{fields[-1]!r} with it:\n{engine[0]}\n{third}")
        else:
            compared += 1
            if len(set(engine)) != 1:
                raise DebugFailure(f"{mode} line {number}: the engine's own runs differ:\n" + "\n".join(engine))
            if engine[0] != third:
                raise DebugFailure(f"{mode} line {number}: {engine[0]!r} without the component, {third!r} with it")
    if masked > MAX_MASKED:
        raise DebugFailure(f"{mode}: {masked} fields differ between the engine's own runs, more than the "
                           f"{MAX_MASKED} that can be process ids and random values")
    return compared, masked
