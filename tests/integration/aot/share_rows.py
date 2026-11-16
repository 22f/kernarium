# SPDX-License-Identifier: GPL-2.0-or-later
"""The shared-verification rows of the AOT guest runner.

aot_guest_share.cpp (loaded into a stock process with LD_PRELOAD) runs the
rewritten libz.so in many processes: the library of the read-only file system,
a byte-identical copy on a writable one, a page of the library copied by a
write, one whose protection changed, and a library whose page-cache pages were
dropped between rounds. Every mode runs once without the component and once
with it, and the lines it prints must be equal. In a diagnostics build the
runner also reads how often the framework hashed a page and how often a
verdict it kept answered instead (`shared_hits`), around each mode.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import re
from typing import Callable

PROGRAM = "/data/local/tmp/libaotshare.so"
MODES = ("fork", "copy", "cow", "mprotect", "evict")
MODE_SECONDS = 300
# The children of a mode, one after another or together, and the sums they print.
CHILD = re.compile(r"^(\S+(?: \S+)*) child (\d+) sum=0x[0-9a-f]+$")
# The least children whose pages the kernel marks immutable that the framework
# must have found verified (fork: sixteen children each run the same pages).
FORK_MIN_SHARED = 16


class ShareFailure(RuntimeError):
    pass


def command(mode: str) -> str:
    return (f"cd /data/local/tmp && AOT_SHARE_MODE={mode} LD_PRELOAD={PROGRAM} "
            f"/system/bin/toybox true; echo rc=$?")


EXPECTED_CHILDREN = {"fork": 16, "copy": 16, "cow": 10, "mprotect": 9, "evict": 18}


def run_mode(shell: Callable[..., str], mode: str) -> list[str]:
    """One run of a mode; returns its record, checked for what it must show."""
    output = shell(command(mode), MODE_SECONDS)
    lines = output.splitlines()
    status = [line for line in lines if line.startswith("rc=")]
    if not status or status[-1] != "rc=0":
        raise ShareFailure(f"{mode}: {status[-1] if status else 'no exit status'}:\n{output}")
    failed = [line for line in lines if line.startswith("FAIL") or re.search(r"(=|: )NO\b", line)]
    if failed:
        raise ShareFailure(f"{mode} reported:\n" + "\n".join(failed) + f"\n--- output:\n{output}")
    record = [line for line in lines if not line.startswith("rc=")]
    if f"done mode={mode}" not in record:
        raise ShareFailure(f"{mode}: no closing record:\n{output}")
    children = [line for line in record if CHILD.match(line)]
    if len(children) != EXPECTED_CHILDREN[mode]:
        raise ShareFailure(f"{mode}: {len(children)} children reported, "
                           f"not {EXPECTED_CHILDREN[mode]}:\n{output}")
    return record


def compare(mode: str, off: list[str], on: list[str]) -> int:
    """The record without the component and with it must be equal; returns
    the number of lines compared."""
    if off != on:
        for number, (before, after) in enumerate(zip(off, on), 1):
            if before != after:
                raise ShareFailure(f"{mode} line {number}: {before!r} without the "
                                   f"component, {after!r} with it")
        raise ShareFailure(f"{mode}: {len(off)} lines without the component, {len(on)} with it")
    return len(on)


def check_counts(mode: str, delta: dict[str, int], fork_shared: int | None) -> str:
    """The framework's counters around a mode in a diagnostics build; returns
    a line to print, or raises. `fork_shared` is the `shared_hits` of the fork
    mode."""
    shared = delta.get("shared_hits", 0)
    hashed = delta.get("verifications", 0)
    text = f"share row {mode}: shared_hits {shared}, verifications {hashed}, pages_hashed {delta.get('pages_hashed', 0)}"
    if mode == "fork" and shared < FORK_MIN_SHARED:
        raise ShareFailure(f"{text}: sixteen children over the read-only library "
                           f"found fewer than {FORK_MIN_SHARED} verdicts to reuse")
    if mode == "copy":
        if fork_shared is None or shared * 4 > fork_shared:
            raise ShareFailure(f"{text}: a copy on a writable file system shared "
                               f"verdicts (fork mode: {fork_shared})")
        if hashed < 12:
            raise ShareFailure(f"{text}: the copy's children hashed no pages")
    return text
