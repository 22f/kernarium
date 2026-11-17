#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that no x86-64 gadget reads a register a helper call may have clobbered.

A `helper_call` (darwin/cpu/x86_64/gadgets/engine/common.inc) reaches a C++
function through a veneer, and AAPCS64 lets that function overwrite x0 to x17.
x0 and x1 carry its results; any other of them a gadget reads after the call
must have been written after it. This reads the assembled engine, whose
generated banks and tail groups are included into it: it finds the veneers
(a `b` to a symbol the object does not define), and from every call to one it
follows the call's own path, with x2 to x17 marked clobbered until an
instruction writes them, and reports a read of a clobbered one. The path runs
through fall-through and unconditional branches, the cold section's included;
at a conditional branch it takes the fall-through, because a path through a
branch's target can be one no run takes (two tests of one bit, or a bank's
unreachable arm). It ends at an indirect branch (the next gadget's dispatch),
a return, a trap, another call, or once every register is written again. A
self-check feeds the scan listings with and without such a read first.

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
ADDEND = re.compile(r"^\s+([0-9a-f]+):\s+ARM64_RELOC_ADDEND\s+0x([0-9a-f]+)$")
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


def target_of(operands: str) -> int | None:
    """The address a direct branch names in the disassembly."""
    found = re.findall(r"0x([0-9a-f]+)(?=\s*<|\s*$)", operands)
    return int(found[-1], 16) if found else None


def scan(listing: str, symbols: str) -> tuple[int, list[str]]:
    """The helper calls of a disassembly, and its reads of a register one of
    them may have clobbered. `symbols` is the object's symbol table."""
    defined: dict[str, int] = {}
    undefined: set[str] = set()
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[-2] == "*UND*":
            undefined.add(fields[-1])
        elif len(fields) >= 3 and re.fullmatch(r"[0-9a-f]+", fields[0]):
            defined[fields[-1]] = int(fields[0], 16)
    rows: dict[int, tuple[str, str, str | None]] = {}
    order: list[int] = []
    branch: dict[int, str] = {}
    addend: dict[int, int] = {}
    symbol = None
    for line in listing.splitlines():
        found = SYMBOL.match(line)
        if found:
            symbol = found[2]
            continue
        found = RELOCATION.match(line)
        if found:
            branch[int(found[1], 16)] = found[2]
            continue
        found = ADDEND.match(line)
        if found:
            addend[int(found[1], 16)] = int(found[2], 16)
            continue
        found = INSTRUCTION.match(line)
        if found:
            address = int(found[1], 16)
            rows[address] = (found[2], found[3], symbol)
            order.append(address)
    following = dict(zip(order, order[1:]))

    def destination(address: int) -> int | None:
        name = branch.get(address)
        if name is not None:
            if name in defined:
                return defined[name] + addend.get(address, 0)
            return None
        return target_of(rows[address][1])

    veneers = {address for address, (mnemonic, _, _) in rows.items()
               if mnemonic == "b" and branch.get(address) in undefined}

    def helper_call(address: int) -> bool:
        mnemonic = rows[address][0]
        if mnemonic != "bl":
            return False
        name = branch.get(address)
        if name is not None and name in undefined:
            return True
        return destination(address) in veneers

    calls = 0
    problems: list[str] = []
    reported: set[int] = set()
    for call in order:
        if not helper_call(call):
            continue
        calls += 1
        start = following.get(call)
        pending = [(start, frozenset(CLOBBERED))] if start is not None else []
        seen: set[tuple[int, frozenset[str]]] = set()
        while pending:
            address, clobbered = pending.pop()
            if address not in rows or (address, clobbered) in seen:
                continue
            seen.add((address, clobbered))
            mnemonic, operands, symbol = rows[address]
            if mnemonic in {"bl", "br", "blr", "ret", "brk"}:
                continue
            reads, writes = accesses(mnemonic, operands)
            stale = sorted({register for register in reads
                            if register in clobbered})
            if stale and address not in reported:
                reported.add(address)
                problems.append(
                    f"{symbol} {address:#x}: `{mnemonic} {operands.strip()}` "
                    f"reads {', '.join(stale)} after the helper call at "
                    f"{call:#x}")
            clobbered = clobbered - set(writes)
            if not clobbered:
                continue
            step = (destination(address) if mnemonic == "b"
                    else following.get(address))
            if step is not None:
                pending.append((step, clobbered))
    return calls, problems


def self_check() -> list[str]:
    """The scan reports a read after a call on any path, and nothing else."""
    symbols = "\n".join([
        "0000000000000000 g     F __TEXT,__text _gadget",
        "0000000000000040 g     F __TEXT,__text _island",
        "0000000000000000         *UND* _helper",
    ])

    def listing(*body: str) -> str:
        return "\n".join([
            "0000000000000000 <_gadget>:",
            *body,
            "0000000000000040 <_island>:",
            "      40:      \tb\t0x40 <_island>",
            "\t\t0000000000000040:  ARM64_RELOC_BRANCH26\t_helper",
            "      44:      \tret",
        ])
    call = "       4:      \tbl\t0x40 <_island>"
    store = "      10:      \tstr\tx8, [x20, #0x10]"
    cases = [
        ("a read on the fall-through path",
         listing("       0:      \tmov\tx8, #0x1", call,
                 "       8:      \tstr\tx8, [x20, #0x10]"), True),
        ("a register read and written by one instruction",
         listing(call, "       8:      \tldrb\tw9, [x21, #0x31]",
                 "       c:      \tbfi\tx9, x17, #0, #8"), True),
        ("a read behind a branch",
         listing(call, "       8:      \tb\t0x10 <_gadget+0x10>",
                 "       c:      \tmov\tx8, x0", store), True),
        ("a read past a conditional branch",
         listing(call, "       8:      \tcbz\tx0, 0x14 <_gadget+0x14>",
                 "       c:      \tnop", store,
                 "      14:      \tret"), True),
        ("registers written after the call, and the results",
         listing(call, "       8:      \tmov\tx8, x0",
                 "       c:      \tadd\tx9, x1, #0x1", store), False),
        ("a read no path from the call reaches",
         listing(call, "       8:      \tb\t0x14 <_gadget+0x14>",
                 "       c:      \tnop", store,
                 "      14:      \tret"), False),
    ]
    problems = []
    for name, text, stale in cases:
        calls, found = scan(text, symbols)
        if calls != 1 or bool(found) != stale:
            problems.append(f"self-check: {name}: {calls} calls, "
                            f"{len(found)} reads reported")
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
            symbols = subprocess.run(
                [arguments.objdump, "-t", str(path)],
                check=True, capture_output=True, text=True).stdout
            calls, found = scan(listing, symbols)
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
