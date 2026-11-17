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
a failure). A tombstone is read from the protobuf file the crash reporter
writes beside the text one (tombstone_pb.py, over the strict wire reader
protowire.py), into the same records: nothing in the comparison reads text.
compare() works field by field, by name.

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

import base64
import binascii
import hashlib
import re
import time
from dataclasses import dataclass, field
from typing import Callable

import protowire
import records
import tombstone_pb
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


# What the shell that ran the program says when the program died of the signal
# a crash mode exists to raise: a line of its own, after the program's records.
DEATH_NOTICE = {"crash-memcpy": "Segmentation fault", "crash-cross": "Segmentation fault",
                "crash-abort": "Aborted", "crash-sibling": "Aborted"}


def command(mode: str) -> str:
    """The program in a shell of its own that says its process id first, and
    keeps it by executing the program in place: the tombstone of that process
    is the one to read."""
    return (f'sh -c "echo pid=\\$\\$; cd /data/local/tmp && export AOT_DEBUG_MODE={mode} '
            f'LD_PRELOAD={PROGRAM} && exec /system/bin/toybox true"; echo rc=$?')


def digest_tombstone(data: bytes) -> tombstone_pb.Digest:
    """The records of a tombstone file (tombstone_pb.py): a file that is not
    whole protobuf, a field of the wrong kind, a message the model needs that
    is missing and a register missing from the crashing thread are failures,
    never guesses."""
    try:
        return tombstone_pb.read(data)
    except (protowire.WireError, tombstone_pb.TombstoneError, RecordError) as problem:
        raise DebugFailure(f"the tombstone: {problem}") from None


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
    regs = of_kind(recs, "tomb.regs")
    if len(signal) != 1 or len(regs) != 1 or "pc" not in regs[0].names():
        raise DebugFailure(f"{mode}: the tombstone has {len(signal)} signal records and {len(regs)} register "
                           f"records, and the registers must hold a pc")
    sig = signal[0]
    shown = [(str(field_of(frame, "file")), str(field_of(frame, "function"))) for frame in frames]
    if mode == "crash-memcpy":
        needed = ("SIGSEGV", "SEGV_ACCERR", ("libc.so", "__mem"), ("libz.so", "updatewindow"),
                  ("libz.so", "inflateSetDictionary"))
    elif mode == "crash-cross":
        needed = ("SIGSEGV", "SEGV_ACCERR", ("libc.so", "__memcmp"))
    else:
        needed = ("SIGABRT", ("libc.so", "abort"))
    present = {field_of(sig, "name"), field_of(sig, "code_name")}
    for token in needed:
        if isinstance(token, str):
            found = token in present
        else:
            found = any(path.endswith("/" + token[0]) and function.startswith(token[1]) for path, function in shown)
        if not found:
            raise DebugFailure(f"{mode}: the tombstone lacks {token!r}: " +
                               "; ".join(str(rec) for rec in recs if rec.kind in ("tomb.signal", "tomb.frame")))
    if mode in ("crash-memcpy", "crash-cross"):
        if not any(row.get("region") == "pc" and row.get("bytes") for row in of_kind(recs, "tomb.code")):
            raise DebugFailure(f"{mode}: the tombstone lacks the code around the pc")
    if len(frames) < {"crash-memcpy": 4, "crash-cross": 2}.get(mode, 2):
        raise DebugFailure(f"{mode}: only {len(frames)} frames in the tombstone")
    if mode in ("crash-memcpy", "crash-cross"):
        if field_of(sig, "fault") != "address":
            raise DebugFailure(f"{mode}: no fault address in the tombstone: {sig}")
    if mode == "crash-sibling":
        other = [rec for rec in of_kind(recs, "tomb.thread_frame") if field_of(rec, "index") == 0]
        if not other or not any(str(field_of(rec, "file")).endswith("/libz.so") for rec in other):
            raise DebugFailure(f"{mode}: no frame of the running thread in libz.so")


def transfer(shell: Callable[..., str], name: str) -> bytes:
    """The bytes of a file of the guest, through its shell: the size and the
    SHA-256 the guest computes, and the file in base64. The bytes are those of
    the file only if the transfer says so, and a file that changed while it
    was being read does not."""
    for _ in range(3):
        text = shell(f"f={name}; echo size=$(wc -c < $f); echo sha256=$(sha256sum $f); "
                     f"echo b64=$(base64 -w 0 $f)", 90)
        found = {key: re.findall(rf"^{key}=(.*)$", text, re.MULTILINE) for key in ("size", "sha256", "b64")}
        if any(len(values) != 1 for values in found.values()):
            raise DebugFailure(f"{name}: the transfer printed no size, digest and data once each:\n{text[:400]}")
        try:
            data = base64.b64decode(found["b64"][0].strip(), validate=True)
        except binascii.Error as problem:
            raise DebugFailure(f"{name}: the base64 of the transfer does not decode: {problem}") from None
        digest = found["sha256"][0].split()
        if (found["size"][0].strip() == str(len(data)) and digest and digest[0] == hashlib.sha256(data).hexdigest()):
            return data
        time.sleep(1)
    raise DebugFailure(f"{name}: the bytes that arrived are not the file the guest measured")


def fetch_tombstone(shell: Callable[..., str], pid: int) -> bytes:
    """The protobuf tombstone of the process `pid`, which has just died. The
    reporter writes the other threads after the crashing one, so the file is
    complete when its process is gone; the newest files are read until one is
    whole and names the process, and a file that does not decode is retried
    until the deadline and then reported."""
    deadline = time.monotonic() + TOMBSTONE_SECONDS
    while time.monotonic() < deadline:
        if not shell("pidof crash_dump64 crash_dump32", 30).strip():
            break
        time.sleep(2)
    time.sleep(2)
    last = "no tombstone file"
    while time.monotonic() < deadline:
        listing = shell("ls -t /data/tombstones/*.pb 2>/dev/null | head -8", 30)
        for name in re.findall(r"^/data/tombstones/tombstone_[0-9]{2}\.pb$", listing, re.MULTILINE):
            data = transfer(shell, name)
            try:
                found = protowire.decode(data, "Tombstone", tombstone_pb.TABLES).values["pid"]
            except protowire.WireError as problem:
                last = f"{name} does not decode: {problem}"
                continue
            if found == pid:
                return data
            last = f"{name} is the tombstone of process {found}"
        time.sleep(2)
    raise DebugFailure(f"no protobuf tombstone of process {pid} ({last})")


def run_mode(shell: Callable[..., str], mode: str) -> list[Record]:
    """One run of a mode; returns its records, checked for what it must show."""
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
    pids = [line for line in lines if line.startswith("pid=")]
    if len(pids) != 1 or not re.fullmatch(r"pid=[0-9]+", pids[0]):
        raise DebugFailure(f"{mode}: the shell did not say the program's process id once:\n{output}")
    notice = DEATH_NOTICE.get(mode)
    notices = [line for line in lines if notice and line.rstrip() == notice]
    if len(notices) > 1:
        raise DebugFailure(f"{mode}: the shell reports the program's death {len(notices)} times:\n{output}")
    try:
        printed = records.decode([line for line in lines if line != "" and not line.startswith(("rc=", "pid="))
                                  and not (notice and line.rstrip() == notice)])
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
        record = digest_tombstone(fetch_tombstone(shell, int(pids[0][4:])))
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
