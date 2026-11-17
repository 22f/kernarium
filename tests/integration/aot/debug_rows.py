# SPDX-License-Identifier: GPL-2.0-or-later
"""The debugger and crash-report rows of the AOT guest runner.

aot_guest_debug.cpp (loaded into a stock process with LD_PRELOAD) crashes and
debugs inside the rewritten libz.so and libc.so. Each mode runs several times
without the component and once with it. A mode that crashes is judged by the
tombstone the crash reporter wrote; a mode that debugs prints its record.

Everything compared is a field of a record (records.py): a record kind, a
field name and a value that is a number, a place (a mapping and the offset in
it) or a text compared whole. The guest program prints its records in an
encoding of its own (one field per line, escaped names, every record whole or
a failure); a tombstone is outside text, and digest_tombstone is the one place
that reads free text. It builds the same records and fails on a line it cannot
read where it expects a register, on a register seen twice, and on a value
that is no value. After that nothing reads text: compare() works field by
field, by name.

The runs of a mode are compared record by record: a record kind that is not
one of the engine's own varying ones (a process id, a random register) must be
equal in every run, and the fields of those that are may differ between the
runs without the component only as a number or a place in a named mapping,
and few of them: the mask is the set of (record kind, field) positions where
the baselines differ, counted, and a place in a named mapping that varies is
judged by the engine's own values. The registers sp, pc and nzcv never vary.

One rule covers a pointer into an unnamed mapping (check_unnamed_field): its
identity is proved over the whole record, field by name, or the row fails and
the field is never masked. Every run compared (the baselines and the run with
the component) must record the field as such a pointer, into mappings of one
shape, counted among the same number of them, and the baselines must record
it alike. Equal counts are necessary, not sufficient, for the same set of
mappings: a mapping that replaced another of its shape at the same place in
the order is not told apart.

Every error the guest program can print is a line of its own that begins with
FAIL; the runner rejects an output that holds FAIL anywhere, a line that is no
record line, and a record that is not whole.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import re
import time
from dataclasses import dataclass, field
from typing import Callable

import records
from records import Named, Record, RecordError, Unnamed, Value

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


class DebugFailure(RuntimeError):
    pass


# ---- tombstones: the one reader of free text --------------------------------

TOMBSTONE_REGISTER = re.compile(r"\s+(?:x\d+|lr|sp|pc|pst|esr|vg)\s")
TOMBSTONE_PAIRS = re.compile(r"\s*(x\d+|lr|sp|pc|pst|esr|vg)\s+([0-9a-f]{16})")
PROCESS_LINE = re.compile(r"pid: (\d+), ppid: (\d+), tid: (\d+), name: (.*?)\s+>>> (.*) <<<")
SIGNAL_LINE = re.compile(r"signal (-?\d+) \((\w+)\), code (-?\d+) \((\w+)\), fault addr "
                         r"(--------|0x[0-9a-f]+)(?: \((read|write)\))?")
FRAME_LINE = re.compile(r"\s+#(\d+) pc ([0-9a-f]+)\s+(\S+)(?:\s+(\S.*))?")
THREAD_FRAME_LINE = re.compile(r"\s+#(\d+) pc [0-9a-f]+\s+(\S+)(?:\s+(\S.*))?")
CODE_ROW = re.compile(r"\s+([0-9a-f]{16})((?: [0-9a-f]{16})+)  (.*)")
FRAME_COUNT = re.compile(r"(\d+) total frames")
HEAD_KEY = re.compile(r"([A-Za-z][A-Za-z _]*): (.*)")


def digest_tombstone(text: str) -> list[Record]:
    """The records of a tombstone, in the order it shows them: the process, the
    signal, the registers (a value that points into a mapping is a place, as
    the run does not choose it), the frames, the code around the pc and the
    lr, the other threads' names and frames, and the head's remaining lines as
    text. A register seen twice, a register line it cannot read, a value that
    is no value, a process, signal, frame or code line that does not read
    whole, and a key seen twice are failures, never guesses."""
    try:
        return _digest(text)
    except RecordError as problem:
        raise DebugFailure(f"the tombstone: {problem}") from None


def _digest(text: str) -> list[Record]:
    blocks = split_blocks(text)
    maps = parse_maps(text)
    keys: list[tuple[str, Value]] = []
    texts: list[tuple[str, Value]] = []
    out: dict[str, list[Record]] = {name: [] for name in
                                    ("process", "signal", "frames", "frame", "code", "thread")}
    registers: list[tuple[str, Value]] = []
    section = "head"
    region = ""
    first_row = 0
    for number, line in enumerate(blocks[0].splitlines(), 1):
        if line.startswith(("Timestamp:", "Process uptime:")):
            continue
        if line.startswith("memory near "):
            section = "keep" if line.startswith(("memory near pc", "memory near lr")) else "skip"
            region = "pc" if line.startswith("memory near pc") else "lr"
            first_row = 0
            continue
        if line.startswith("memory map") or line.startswith("open files"):
            section = "skip"
            continue
        if section == "skip":
            continue
        if not line.strip():
            section = "head"
            continue
        if section == "keep":
            row = CODE_ROW.fullmatch(line)
            if row is None:
                raise RecordError(f"line {number}: a line it cannot read where it expects code: {line!r}")
            address = int(row.group(1), 16)
            first_row = first_row or address
            out["code"].append(Record("tomb.code", (("region", region), ("offset", address - first_row),
                                                    ("words", row.group(2).strip()), ("ascii", row.group(3)))))
            continue
        if line.startswith("signal "):
            signal = SIGNAL_LINE.fullmatch(line)
            if signal is None:
                raise RecordError(f"line {number}: a signal line it cannot read: {line!r}")
            fault = signal.group(5)
            # The page of a fault address is the process's layout; where in
            # the page it lies is the guest's.
            out["signal"].append(Record("tomb.signal", (
                ("signo", int(signal.group(1))), ("name", signal.group(2)), ("code", int(signal.group(3))),
                ("code_name", signal.group(4)), ("fault", "none" if fault == "--------" else "address"),
                ("page_offset", 0 if fault == "--------" else int(fault, 16) & 0xfff),
                ("access", signal.group(6) or "none"))))
            continue
        if TOMBSTONE_REGISTER.match(line):
            end = 0
            for pair in TOMBSTONE_PAIRS.finditer(line):
                if pair.start() != end:
                    break
                end = pair.end()
                registers.append((pair.group(1), records.resolve(int(pair.group(2), 16), maps, True)))
            if end != len(line.rstrip()):
                raise RecordError(f"line {number}: a register line it cannot read: {line!r}")
            continue
        if line.startswith("pid:"):
            process = PROCESS_LINE.fullmatch(line)
            if process is None:
                raise RecordError(f"line {number}: a process line it cannot read: {line!r}")
            out["process"].append(Record("tomb.process", (
                ("pid", int(process.group(1))), ("ppid", int(process.group(2))), ("tid", int(process.group(3))),
                ("name", process.group(4)), ("executable", process.group(5)))))
            continue
        count = FRAME_COUNT.fullmatch(line)
        if count:
            out["frames"].append(Record("tomb.frames", (("count", int(count.group(1))),)))
            continue
        if line == "backtrace:":
            continue
        frame = FRAME_LINE.fullmatch(line) if line.lstrip().startswith("#") else None
        if line.lstrip().startswith("#"):
            if frame is None:
                raise RecordError(f"line {number}: a frame it cannot read: {line!r}")
            out["frame"].append(Record("tomb.frame", (
                ("index", int(frame.group(1))), ("pc", int(frame.group(2), 16)), ("path", frame.group(3)),
                ("detail", frame.group(4) or ""))))
            continue
        key = HEAD_KEY.fullmatch(line)
        if key and not line.startswith(" "):
            keys.append((key.group(1), key.group(2)))
        else:
            texts.append((f"line{len(texts)}", line))
    if registers:
        out["registers"] = [Record("tomb.regs", tuple(registers))]
    for block in blocks[1:]:
        for line in block.splitlines():
            if line.startswith("pid:"):
                process = PROCESS_LINE.fullmatch(line)
                if process is None:
                    raise RecordError(f"a thread's process line it cannot read: {line!r}")
                out["thread"].append(Record("tomb.thread", (("name", process.group(4)),)))
            elif re.match(r"\s+#\d+ pc ", line):
                frame = THREAD_FRAME_LINE.fullmatch(line)
                if frame is None:
                    raise RecordError(f"a thread's frame it cannot read: {line!r}")
                # Where a thread was caught while it ran is not a fact of the
                # guest: its first frame keeps the library and nothing else,
                # and the others the symbol without its offset.
                symbol = re.sub(r"\+\d+\)$", ")", re.sub(r" \(BuildId: \w+\)$", "", frame.group(3) or ""))
                out["thread"].append(Record("tomb.thread_frame", (
                    ("index", int(frame.group(1))), ("path", frame.group(2)),
                    ("symbol", "" if frame.group(1) == "00" else symbol))))
    ordered = [Record("tomb.head", tuple(keys)), Record("tomb.text", tuple(texts))]
    for name in ("process", "signal", "registers", "frames", "frame", "code", "thread"):
        ordered.extend(out.get(name, []))
    return ordered


def field_of(rec: Record, name: str) -> Value:
    try:
        return rec.get(name)
    except RecordError as problem:
        raise DebugFailure(str(problem)) from None


def of_kind(recs: list[Record], kind: str) -> list[Record]:
    return [rec for rec in recs if rec.kind == kind]


def verify_tombstone(mode: str, recs: list[Record]) -> None:
    signal = of_kind(recs, "tomb.signal")
    frames = of_kind(recs, "tomb.frame")
    if len(signal) != 1:
        raise DebugFailure(f"{mode}: {len(signal)} signal lines in the tombstone")
    sig = signal[0]
    if len(of_kind(recs, "tomb.regs")) != 1 or not any(name == "pc" for name, _ in of_kind(recs, "tomb.regs")[0].fields):
        raise DebugFailure(f"{mode}: the tombstone has no register record with a pc")
    shown = [f"{field_of(frame, 'path')} {field_of(frame, 'detail')}" for frame in frames]
    if mode == "crash-memcpy":
        needed = ("SIGSEGV", "SEGV_ACCERR", "libc.so (__mem", "libz.so (updatewindow", "libz.so (inflateSetDictionary")
    elif mode == "crash-cross":
        needed = ("SIGSEGV", "SEGV_ACCERR", "libc.so (__memcmp")
    else:
        needed = ("SIGABRT", "libc.so (abort+")
    present = {field_of(sig, "name"), field_of(sig, "code_name")}
    for token in needed:
        if token.startswith("SIG") or token.startswith("SEGV"):
            found = token in present
        else:
            found = any(token in line for line in shown)
        if not found:
            raise DebugFailure(f"{mode}: the tombstone lacks {token!r}: " +
                               "; ".join(str(rec) for rec in recs if rec.kind in ("tomb.signal", "tomb.frame")))
    if mode in ("crash-memcpy", "crash-cross"):
        if not any(row.get("region") == "pc" for row in of_kind(recs, "tomb.code")):
            raise DebugFailure(f"{mode}: the tombstone lacks the code around the pc")
    if len(frames) < {"crash-memcpy": 4, "crash-cross": 2}.get(mode, 2):
        raise DebugFailure(f"{mode}: only {len(frames)} frames in the tombstone")
    if mode in ("crash-memcpy", "crash-cross"):
        # A read that runs into an unmapped page is a read fault in it.
        if field_of(sig, "fault") != "address" or field_of(sig, "access") != "read":
            raise DebugFailure(f"{mode}: no read fault address in the tombstone: {sig}")
    if mode == "crash-sibling":
        other = [rec for rec in of_kind(recs, "tomb.thread_frame") if field_of(rec, "index") == 0]
        if not other or not any("libz.so" in str(field_of(rec, "path")) for rec in other):
            raise DebugFailure(f"{mode}: no frame of the running thread in libz.so")


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


def run_mode(shell: Callable[..., str], mode: str) -> list[Record]:
    """One run of a mode; returns its records, checked for what it must show."""
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
    failed = [line for line in lines if "FAIL" in line]
    if failed:
        raise DebugFailure(f"{mode} reported:\n" + "\n".join(failed) + f"\n--- output:\n{output}")
    try:
        printed = records.decode([line for line in lines if line != "" and not line.startswith("rc=")])
    except RecordError as problem:
        raise DebugFailure(f"{mode}: the program's output is not a whole record: {problem}\n--- output:\n{output}") \
            from None
    refused = [rec for rec in printed for _, value in rec.fields if value in ("NO", "IS NOT")]
    if refused:
        raise DebugFailure(f"{mode} reported:\n" + "\n".join(str(rec) for rec in refused))
    states = [rec for rec in printed if rec.kind == "randomization"]
    if len(states) != 1 or states[0].get("state") != "off":
        raise DebugFailure(f"{mode}: the program did not run with address-space randomization off "
                           f"(it prints the record randomization once when it does):\n{output}")
    if mode in CRASH_STATUS:
        record = digest_tombstone(fetch_tombstone(shell))
        verify_tombstone(mode, record)
        return record
    verify_record(mode, printed)
    return printed


def verify_record(mode: str, recs: list[Record]) -> None:
    done = [rec for rec in recs if rec.kind == "done"]
    if len(done) != 1 or done[0].get("mode") != mode or recs[-1] is not done[0]:
        raise DebugFailure(f"{mode}: no closing record: " + "; ".join(str(rec) for rec in recs[-3:]))
    if mode == "ptrace":
        hits = sum(1 for rec in recs if rec.kind == "breakpoint" and rec.get("at_breakpoint") == "yes")
        changed = {(rec.get("what"), rec.get("verdict")) for rec in recs if rec.kind == "changed"}
        for needed in (("length", "is"), ("pc", "is")):
            if needed not in changed:
                raise DebugFailure(f"ptrace: missing the changed {needed[0]} record that says it is the library's answer")
        if not any(rec.kind == "detached" and rec.get("status") == 0 for rec in recs):
            raise DebugFailure("ptrace: missing the record that the tracee detached with status 0")
        steps = sum(1 for rec in recs if rec.kind == "step")
        summary = [rec for rec in recs if rec.kind == "steps"]
        library_changes = summary[0].get("changes") if len(summary) == 1 else None
        instructions = summary[0].get("libc") if len(summary) == 1 else None
        if hits < 5 or steps < 40 or library_changes is None or library_changes < 4 or instructions < 16 \
                or not any(rec.kind == "tracee_done" for rec in recs):
            raise DebugFailure(f"ptrace: the record is incomplete ({hits} hits, {steps} steps)")
    if mode == "sibling":
        rounds = [rec for rec in recs if rec.kind in ("sibling_entry", "sibling_loop")
                  and all(rec.get(name) == "yes" for name in ("hit", "by_sibling", "pc_at_breakpoint", "in_libz"))]
        goes_on = [rec for rec in recs if rec.kind == "sibling_runs_on" and rec.get("yes") == "yes"]
        if len(rounds) != 2 or len(goes_on) != 2:
            raise DebugFailure(f"sibling: {len(rounds)} hit rounds and {len(goes_on)} continuations, not 2 and 2")


# ---- the comparison ---------------------------------------------------------

# The kinds of record whose fields may differ between two runs of one engine
# (a process id, a random register). Every other kind must be the same in
# every run.
MASKABLE = frozenset({"tomb.process", "tomb.regs", "regs"})
# Fields that never differ between two runs of one engine, whatever they hold.
STRICT_FIELDS = {"regs": frozenset({"sp", "pc", "nzcv"})}


@dataclass
class Comparison:
    compared: int = 0
    masked: int = 0
    mask: set[tuple[str, str]] = field(default_factory=set)


def check_unnamed_field(mode: str, number: int, kind: str, name: str, values: list[Value]) -> None:
    """The one rule for a pointer into an unnamed mapping: its identity is
    proved over the whole record, by the field's name, or it has none and the
    row fails. The runs compared (the baselines and the run with the
    component) must all record the field as a pointer into an unnamed mapping,
    of one shape, counted among the same number of mappings of that shape, and
    the baselines must record it alike; nothing is masked. Equal counts are
    necessary and not sufficient for the same set of mappings."""
    where = f"{mode} record {number} ({kind}) field {name}"
    runs = "\n".join(f"  {'run with the component' if n == len(values) else f'run {n} without'}: "
                     f"{records.describe(value)}" for n, value in enumerate(values, 1))
    places = [value for value in values if isinstance(value, Unnamed)]
    if len(places) != len(values):
        raise DebugFailure(f"{where}: ambiguous identity: {name} points into an unnamed mapping in {len(places)} "
                           f"of {len(values)} runs and is something else in the others, so no run is proved "
                           f"the same place as another:\n{runs}")
    shapes = sorted({place.shape() for place in places})
    if len(shapes) > 1:
        raise DebugFailure(f"{where}: ambiguous identity: {name} points into unnamed mappings of different "
                           f"shapes ({' and '.join(shapes)}):\n{runs}")
    counts = sorted({place.count for place in places})
    if len(counts) > 1:
        spelled = (f"{counts[0]} in one run and {counts[1]} in another" if len(counts) == 2 else
                   f"{', '.join(map(str, counts[:-1]))} and {counts[-1]} in different runs")
        raise DebugFailure(f"{where}: ambiguous identity: the unnamed mappings of {shapes[0]} number {spelled}, "
                           f"so the place {name} points to among them is not one place:\n{runs}")
    if len(set(values[:-1])) > 1:
        raise DebugFailure(f"{where}: the engine's own runs differ in {name}, a pointer into an unnamed "
                           f"mapping, whose place is never masked:\n{runs}")


def compare_fields(mode: str, off_runs: list[list[Record]], with_it: list[Record]) -> Comparison:
    """The records of the runs without the component and of the run with it,
    field by field and by name. A field is masked when two of the runs without
    the component differ in it, it belongs to a kind whose fields may differ
    (a process id, a random register) and it is a number or a place in a named
    mapping; an sp, pc or nzcv that differs, a place in a named mapping that
    differs and that the run with the component shows differently from every
    run without it, a text or any field of another kind that differs, a
    pointer into an unnamed mapping that cannot be proved the same place
    (check_unnamed_field), or more than MAX_MASKED masked fields, fail the
    row."""
    runs = off_runs + [with_it]
    if len({len(run) for run in runs}) != 1:
        raise DebugFailure(f"{mode}: the runs have {[len(run) for run in runs]} records")
    result = Comparison()
    for number, group in enumerate(zip(*runs), 1):
        first = group[0]
        for other in group[1:]:
            if other.kind != first.kind or other.names() != first.names():
                raise DebugFailure(f"{mode} record {number}: the runs have {first.kind}{list(first.names())} "
                                   f"and {other.kind}{list(other.names())}")
        for index, name in enumerate(first.names()):
            values = [rec.fields[index][1] for rec in group]
            baselines, shown = values[:-1], values[-1]
            where = f"{mode} record {number} ({first.kind}) field {name}"
            if any(isinstance(value, Unnamed) for value in values):
                check_unnamed_field(mode, number, first.kind, name, values)
            if len(set(baselines)) == 1:
                result.compared += 1
                if baselines[0] != shown:
                    raise DebugFailure(f"{where}: {records.describe(baselines[0])} without the component, "
                                       f"{records.describe(shown)} with it:\n{first}\n{group[-1]}")
                continue
            engine = sorted({records.describe(value) for value in baselines})
            if (first.kind not in MASKABLE or name in STRICT_FIELDS.get(first.kind, ())
                    or any(isinstance(value, str) for value in values)):
                raise DebugFailure(f"{where}: the engine's own runs differ in {engine}:\n" +
                                   "\n".join(str(rec) for rec in group))
            if any(isinstance(value, Named) for value in baselines) and shown not in baselines:
                # A value that names a place and still varies between the
                # engine's own runs (a register left over from a race in the
                # guest) is judged by what the engine itself showed: the run
                # with the component may show any of those values, no other.
                raise DebugFailure(f"{where}: the engine's own runs differ in {engine} and the run with the "
                                   f"component shows {records.describe(shown)!r}:\n" +
                                   "\n".join(str(rec) for rec in group))
            result.masked += 1
            result.mask.add((first.kind, name))
    if result.masked > MAX_MASKED:
        raise DebugFailure(f"{mode}: {result.masked} fields differ between the engine's own runs, more than the "
                           f"{MAX_MASKED} that can be process ids and random values")
    return result


def compare(mode: str, off_runs: list[list[Record]], with_it: list[Record]) -> tuple[int, int]:
    """compare_fields, as the counts the runner reports: (fields compared,
    fields masked)."""
    result = compare_fields(mode, off_runs, with_it)
    return result.compared, result.masked
