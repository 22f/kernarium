#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that no x86-64 gadget reads a register a helper call may have clobbered.

A `helper_call` (darwin/cpu/x86_64/gadgets/engine/common.inc) reaches a C++
function through a veneer, and AAPCS64 lets that function overwrite x0 to x17.
x0 and x1 carry its results; any other of them a gadget reads after the call
must have been written after it. This reads the assembled engine, whose
generated banks and tail groups are included into it: it finds the veneers
(a `b` to a symbol the object does not define), then scans each symbol's
instructions in address order, marking x2 to x17 clobbered at every call to a
veneer and clear again where an instruction writes them, and reports a read of
a clobbered one. An unconditional branch, a return and a new symbol end the
scan's path, so a value written on another path to a label that follows a
call is not seen; a call's own fall-through path is. A self-check feeds the
scan listings with and without such a read first.

usage: test_gadget_helper_clobbers.py [--objdump OBJDUMP --object TARGET OBJECT]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import re
import subprocess
import sys
from pathlib import Path

SYMBOL = re.compile(r"^([0-9a-f]+) <([^>]+)>:$")
INSTRUCTION = re.compile(r"^\s+([0-9a-f]+):\s+(\S+)\s*(.*)$")
RELOCATION = re.compile(r"^\s+([0-9a-f]+):\s+ARM64_RELOC_BRANCH26\s+(\S+)$")
REGISTER = re.compile(r"(?<![\w.])[xw](\d{1,2})(?!\w)")
MEMORY = re.compile(r"\[([^\]]*)\](!?)(.*)$")
CLOBBERED = frozenset(f"x{number}" for number in range(2, 18))
# Mnemonics that write no register operand: stores, compares, branches.
NO_DESTINATION = re.compile(
    r"^(?:st(?!xr|lxr|xp|lxp)|cmp|cmn|tst|ccmp|ccmn|cbn?z|tbn?z|b\.|br$|blr$|"
    r"ret$|msr$|prfm|dmb|dsb|isb|fcmp|brk$|hint|nop$)")
STATUS_STORE = re.compile(r"^st(?:l?xr|l?xp)")
PAIR_LOAD = re.compile(r"^ld(?:n?p|a?xp)")
ATOMIC = re.compile(r"^(?:swp|ld(?:add|clr|eor|set|smax|smin|umax|umin))")
# Writes its first operand and reads it too.
READ_MODIFY_WRITE = re.compile(r"^(?:movk|bfi|bfxil|bfm|cas)")
SHOWN = 20


def registers(text: str) -> list[str]:
    return [f"x{number}" for number in REGISTER.findall(text)
            if int(number) <= 30]


def accesses(mnemonic: str, operands: str) -> tuple[list[str], list[str]]:
    """The general registers an instruction reads and writes."""
    operands = operands.split("//")[0]
    memory = MEMORY.search(operands)
    head = operands[:memory.start()] if memory else operands
    reads = registers(memory[1]) if memory else []
    writes = []
    if memory and reads and (memory[2] == "!" or memory[3].strip()):
        writes.append(reads[0])
    named = []
    for operand in head.split(","):
        found = registers(operand)
        named.append(found[0] if found else None)
    present = [register for register in named if register]
    first = named[0] if named else None
    if STATUS_STORE.match(mnemonic):
        writes += [first] if first else []
        reads += [register for register in named[1:] if register]
    elif NO_DESTINATION.match(mnemonic):
        reads += present
    elif PAIR_LOAD.match(mnemonic):
        writes += [register for register in named[:2] if register]
        reads += [register for register in named[2:] if register]
    elif ATOMIC.match(mnemonic):
        reads += [first] if first else []
        writes += [named[1]] if len(named) > 1 and named[1] else []
    elif READ_MODIFY_WRITE.match(mnemonic):
        reads += present
        writes += [first] if first else []
    else:
        writes += [first] if first else []
        reads += [register for register in named[1:] if register]
    return reads, writes


def scan(listing: str) -> tuple[int, list[str]]:
    """The helper calls of a disassembly, and its reads of a register one of
    them may have clobbered."""
    rows: list[tuple[int, str | None, str, str | None]] = []
    relocations: dict[int, str] = {}
    defined: set[str] = set()
    symbol = None
    for line in listing.splitlines():
        found = SYMBOL.match(line)
        if found:
            symbol = found[2]
            defined.add(symbol)
            rows.append((int(found[1], 16), None, "", symbol))
            continue
        found = RELOCATION.match(line)
        if found:
            relocations[int(found[1], 16)] = found[2]
            continue
        found = INSTRUCTION.match(line)
        if found:
            rows.append((int(found[1], 16), found[2], found[3], symbol))
    veneers = {address for address, mnemonic, _, _ in rows
               if mnemonic == "b" and address in relocations and
               relocations[address] not in defined}
    calls = 0
    problems: list[str] = []
    clobbered: set[str] = set()
    call = 0
    for address, mnemonic, operands, symbol in rows:
        if mnemonic is None:
            clobbered = set()
            continue
        if mnemonic == "bl":
            token = operands.split()[0] if operands.split() else ""
            target = int(token, 16) if token.startswith("0x") else None
            external = relocations.get(address)
            if target in veneers or (external and external not in defined):
                clobbered = set(CLOBBERED)
                calls += 1
                call = address
            else:
                clobbered = set()
            continue
        reads, writes = accesses(mnemonic, operands)
        stale = sorted({register for register in reads
                        if register in clobbered})
        if stale:
            problems.append(
                f"{symbol} {address:#x}: `{mnemonic} {operands.strip()}` "
                f"reads {', '.join(stale)} after the helper call at "
                f"{call:#x}")
        clobbered -= set(writes)
        if mnemonic in {"b", "br", "ret", "brk"}:
            clobbered = set()
    return calls, problems


def self_check() -> list[str]:
    """The scan reports a read after a call and nothing else."""
    def listing(*body: str) -> str:
        return "\n".join([
            "0000000000000000 <_gadget>:",
            *body,
            "0000000000000040 <_island>:",
            "      40:      \tb\t0x40 <_island>",
            "\t\t0000000000000040:  ARM64_RELOC_BRANCH26\t_helper",
            "      44:      \tbl\t0x48 <_island+0x8>",
            "      48:      \tret",
        ])
    call = "       4:      \tbl\t0x40 <_island>"
    cases = [
        (listing("       0:      \tmov\tx8, #0x1", call,
                 "       8:      \tstr\tx8, [x20, #0x10]"), True),
        (listing(call, "       8:      \tldrb\tw9, [x21, #0x31]",
                 "       c:      \tbfi\tx9, x17, #0, #8"), True),
        (listing(call, "       8:      \tmov\tx8, x0",
                 "       c:      \tstr\tx8, [x20, #0x10]",
                 "      10:      \tadd\tx9, x1, #0x1"), False),
        (listing(call, "       8:      \tb\t0x10 <_gadget+0x10>",
                 "       c:      \tstr\tx8, [x20, #0x10]"), False),
    ]
    problems = []
    for text, stale in cases:
        calls, found = scan(text)
        if calls != 1 or bool(found) != stale:
            problems.append(
                "self-check: the scan " +
                ("missed a read after a call" if stale else
                 "reported a register written after the call") +
                f" ({calls} calls, {found})")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--objdump")
    parser.add_argument("--object", nargs=2, metavar=("TARGET", "OBJECT"))
    arguments = parser.parse_args()
    if (arguments.objdump is None) != (arguments.object is None):
        parser.error("--objdump and --object go together")
    problems = self_check()
    if arguments.object is not None:
        target, name = arguments.object
        path = Path(name)
        if not path.is_file():
            problems.append(f"{target}: {path} does not exist")
        else:
            listing = subprocess.run(
                [arguments.objdump, "-d", "-r", "--no-show-raw-insn",
                 str(path)],
                check=True, capture_output=True, text=True).stdout
            calls, found = scan(listing)
            problems += [f"{target}: {path.name}: {entry}"
                         for entry in found[:SHOWN]]
            if len(found) > SHOWN:
                problems.append(f"{target}: ... and {len(found) - SHOWN} more")
            if calls == 0:
                problems.append(f"{target}: {path.name}: no helper call "
                                "found; this check has nothing to guard")
            if not problems:
                print(f"{target}: {path.name}: {calls} helper calls, no "
                      "register read before it is written again")
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print("self-check: the scan reports a read after a call")
    return 0


if __name__ == "__main__":
    sys.exit(main())
