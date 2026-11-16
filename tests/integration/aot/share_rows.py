# SPDX-License-Identifier: GPL-2.0-or-later
"""The shared-verification rows of the AOT guest runner.

aot_guest_share.cpp (loaded into a stock process with LD_PRELOAD) runs the
rewritten libz.so in many processes: the library of the read-only file system,
a byte-identical copy on a writable one, a page of the library copied by a
write (in the children, or in their parent before they are made), one whose
protection changed with and without a store, a library whose pages the kernel
was made to reclaim between rounds, and one whose pages were dropped from the
page cache. Every mode runs once without the component and once with it, and
the lines it prints must be equal. In a diagnostics build the runner also
reads how often the framework hashed a page, how often a verdict it kept
answered instead (`shared_hits`) and how often it refused a page whose mapping
lacks the kernel's immutable fact, around each mode, and each mode states what
those counts must show.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import re
from typing import Callable

PROGRAM = "/data/local/tmp/libaotshare.so"
MODES = ("fork", "copy", "cow", "cow-parent", "mprotect", "mprotect-write", "pressure", "evict")
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


EXPECTED_CHILDREN = {"fork": 16, "copy": 16, "cow": 10, "cow-parent": 7, "mprotect": 6,
                     "mprotect-write": 6, "pressure": 15, "evict": 6}
# Children that write a text page of their own copy, in the modes that make them,
# and the children that run the library's unchanged pages.
COW_WRITERS = {"cow": 3, "cow-parent": 7, "mprotect-write": 3}
UNCHANGED_CHILDREN = {"cow": 7, "cow-parent": 0, "mprotect": 6, "mprotect-write": 3, "pressure": 15, "evict": 6}


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


def informational(record: list[str]) -> list[str]:
    """The lines a mode prints for the reader, that depend on the guest's
    memory and not on the engine: they start with '# ' and are not compared."""
    return [line for line in record if line.startswith("# ")]


def compare(mode: str, off: list[str], on: list[str]) -> int:
    """The record without the component and with it must be equal; returns
    the number of lines compared."""
    off = [line for line in off if not line.startswith("# ")]
    on = [line for line in on if not line.startswith("# ")]
    if off != on:
        for number, (before, after) in enumerate(zip(off, on), 1):
            if before != after:
                raise ShareFailure(f"{mode} line {number}: {before!r} without the "
                                   f"component, {after!r} with it")
        raise ShareFailure(f"{mode}: {len(off)} lines without the component, {len(on)} with it")
    return len(on)


CONTROL = re.compile(r"^# control: (\d+) of 256 pages cached before, (\d+) after$")
LIBZ = re.compile(r"^# libz\.so: (\d+) pages cached before the eviction, (\d+) after$")


def check_counts(mode: str, delta: dict[str, int], fork_shared: int | None,
                 record: list[str] | None = None) -> str:
    """The framework's counters of libz.so around a mode in a diagnostics
    build: pages verified by their hash, verifications a verdict of another
    address space answered, and pages refused for lack of the kernel's
    immutable fact. Returns a line to print, or raises. `fork_shared` is
    `shared` of the fork mode; `record` is the mode's record."""
    shared = delta.get("shared", 0)
    hashed = delta.get("verified", 0)
    refused = delta.get("mutable", 0)
    text = (f"share row {mode}: libz.so pages verified by hash {hashed}, answered by a "
            f"shared verdict {shared}, refused for lack of the immutable fact {refused}")
    if mode == "fork":
        if shared < FORK_MIN_SHARED:
            raise ShareFailure(f"{text}: sixteen children over the read-only library "
                               f"found fewer than {FORK_MIN_SHARED} verdicts to reuse")
        if refused:
            raise ShareFailure(f"{text}: a page of the read-only library was refused")
    elif mode == "copy":
        if fork_shared is None or shared * 4 > fork_shared:
            raise ShareFailure(f"{text}: a copy on a writable file system shared "
                               f"verdicts (fork mode: {fork_shared})")
        # The kernel does not declare the copy's pages immutable: rewritten
        # code is not entered for them, and the page verification says so.
        if refused < 12:
            raise ShareFailure(f"{text}: the copy's pages were not refused")
    elif mode in ("cow", "cow-parent", "mprotect-write"):
        # A page that is a copy in the child's address space is not the file
        # system's, whatever its mapping says: each child that ran it must
        # have been refused it, and the pages nothing changed still shared.
        if refused < COW_WRITERS[mode]:
            raise ShareFailure(f"{text}: {COW_WRITERS[mode]} children ran a copied page and "
                               f"it was refused fewer times")
        if shared < max(UNCHANGED_CHILDREN[mode], 4):
            raise ShareFailure(f"{text}: the library's unchanged pages were not shared")
    elif mode == "mprotect":
        # A protection that went to writable and back with no store leaves the
        # page the file system's: nothing is refused, and the pages share.
        if refused:
            raise ShareFailure(f"{text}: a page nothing wrote was refused")
        if shared < UNCHANGED_CHILDREN[mode]:
            raise ShareFailure(f"{text}: the library's pages were not shared")
    elif mode == "pressure":
        if shared < UNCHANGED_CHILDREN[mode] or refused:
            raise ShareFailure(f"{text}: the pages of the read-only library were not shared "
                               "(or one was refused)")
    elif mode == "evict":
        text += evict_check(record or [], hashed, shared, refused)
    return text


def evict_check(record: list[str], hashed: int, shared: int, refused: int) -> str:
    """The evict mode: the control must have lost its pages (the program fails
    itself otherwise), the pages libz.so lost are counted, and children that
    run it again must verify what it lost afresh, never take a verdict of the
    frames it had. Returns what to add to the line."""
    control = next((m for m in map(CONTROL.match, record) if m), None)
    libz = next((m for m in map(LIBZ.match, record) if m), None)
    if control is None or libz is None:
        raise ShareFailure("the evict record lacks its control or libz.so counts")
    if int(control.group(1)) < 128 or int(control.group(2)) != 0:
        raise ShareFailure("the control file was not evicted: the row cannot observe eviction")
    evicted = int(libz.group(1)) - int(libz.group(2))
    if refused:
        raise ShareFailure("a page of the read-only library was refused")
    if evicted > 0:
        # The children after the eviction run pages the page cache had to read
        # again: each of them is verified from its bytes at least once.
        if hashed < 1:
            raise ShareFailure(f"{evicted} pages of libz.so were evicted and no page "
                               "was hashed again")
    elif shared < 3:
        raise ShareFailure("the pages of the read-only library were not shared")
    return f"; the control lost its pages, libz.so lost {max(evicted, 0)} of {libz.group(1)}"
