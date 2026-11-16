# SPDX-License-Identifier: GPL-2.0-or-later
"""The debugger and crash-report rows of the AOT guest runner.

aot_guest_debug.cpp (loaded into a stock process with LD_PRELOAD) crashes and
debugs inside the rewritten libz.so and libc.so. Each mode runs twice without
the component and once with it. A mode that crashes is judged by the
tombstone the crash reporter wrote; a mode that debugs prints its record. The
runs are then compared line by line: a line that names something the engine
itself chooses differently from run to run (an address under ASLR, a process
id, a time) is masked field by field where the two runs without the component
differ, and every other line must be the same in all three runs.
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


def digest_tombstone(text: str) -> list[str]:
    """The lines of a tombstone that describe the crash, one per line, tagged
    by what may vary: `~` lines are compared field by field (registers,
    addresses, process ids), `=` lines must be equal in every run."""
    blocks = split_blocks(text)
    out: list[str] = []
    section = "head"
    for line in blocks[0].splitlines():
        if line.startswith(("Timestamp:", "Process uptime:")):
            continue
        if line.startswith("memory near "):
            section = "keep" if line.startswith(("memory near pc", "memory near lr")) else "skip"
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
        varying = (line.startswith("pid:") or re.match(r"\s+(x\d+|lr|esr)\s", line)
                   or (section == "keep" and re.match(r"\s+[0-9a-f]{16} ", line)))
        out.append(("~ " if varying else "= ") + line)
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


# Fields of a register record that are symbolic (the position in a library or
# the stack) and so never differ between two runs of one engine.
STRICT_FIELDS = ("pc=", "sp=", "nzcv=")


def maskable(line: str) -> bool:
    return line.startswith("~ ") or line.startswith("  regs")


def compare(mode: str, off_runs: list[list[str]], with_it: list[str]) -> tuple[int, int]:
    """The records of the runs without the component and of the run with it;
    returns (fields compared, fields masked). A field is masked when two of
    the runs without the component differ in it."""
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
                        raise DebugFailure(f"{mode} line {number}: the engine's own runs differ in {sorted(values)}")
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
    return compared, masked
