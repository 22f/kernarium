#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Write a tiny arm64 shared object whose code is the AOT test fixture.

Real libraries cannot be relied on to contain particular control flow, so
the differential test (tests/unit/arm64/arm64_aot_differential_tests.cpp)
also rewrites this file. It holds a loop that runs through the interior of a
second unwind range: a function whose backward branch lands inside another
function. The file is generated into the build tree of a diagnostics build
and is never part of a product.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import struct
import sys

PAGE = 4096
TEXT = 0x1000
TEXT_PAGES = 4
EH_FRAME = TEXT + TEXT_PAGES * PAGE


def b_cond(cond: int, here: int, target: int) -> int:
    return 0x54000000 | (((target - here) // 4) & 0x7ffff) << 5 | cond


def b(here: int, target: int) -> int:
    return 0x14000000 | (((target - here) // 4) & 0x3ffffff)


def bl(here: int, target: int) -> int:
    return 0x94000000 | (((target - here) // 4) & 0x3ffffff)


def cbz(rt: int, here: int, target: int) -> int:
    return 0xb4000000 | (((target - here) // 4) & 0x7ffff) << 5 | rt


def tbz(rt: int, bit: int, here: int, target: int) -> int:
    return 0x36000000 | bit << 19 | (((target - here) // 4) & 0x3fff) << 5 | rt


def adr(rd: int, here: int, target: int) -> int:
    delta = target - here
    return 0x10000000 | (delta & 3) << 29 | ((delta >> 2) & 0x7ffff) << 5 | rd


NOP = 0xd503201f
RET = 0xd65f03c0
BRK = 0xd4200000

# Function A (0x1000..0x1010) enters the loop; function B (0x1020..0x1030)
# closes it with a backward branch to 0x1004, inside A, after A jumped into
# B's interior. The loop passes through no function entry. The two ranges
# are not adjacent, so they stay two functions.
LOOP_A = TEXT
LOOP_B = TEXT + 0x20
CODE = {
    LOOP_A + 0x00: 0xd2800000,                      # movz x0, #0
    LOOP_A + 0x04: 0x91000400,                      # add x0, x0, #1
    LOOP_A + 0x08: b(LOOP_A + 0x08, LOOP_B + 4),    # b LOOP_B+4
    LOOP_A + 0x0c: RET,
    LOOP_B + 0x00: 0x91000421,                      # add x1, x1, #1
    LOOP_B + 0x04: 0xeb03001f,                      # cmp x0, x3
    LOOP_B + 0x08: b_cond(3, LOOP_B + 0x08, LOOP_A + 0x04),  # b.lo A+4
    LOOP_B + 0x0c: RET,
}
FUNCTIONS = ((LOOP_A, 0x10), (LOOP_B, 0x10))

# Page edges. The functions below are on one text page and leave it for the
# next by every kind of edge the rewritten code can take; the target there
# adds 100 to x0 and returns through x15. The differential test maps the
# next page with other bytes than the framework was built from, so an edge
# that runs the stored code of a page it was not compared with gives another
# x0 than the engine does.
SRC_PAGE = TEXT + 2 * PAGE
DST_PAGE = TEXT + 3 * PAGE
TARGET = DST_PAGE + 0x100
MOV_X15_X30 = 0xaa1e03ef
CMP_X1_X1 = 0xeb01003f
MOV_X2_0 = 0xd2800002
BR_X16, BLR_X16, RET_X15 = 0xd61f0200, 0xd63f0200, 0xd65f01e0
EDGES = {
    "fall": SRC_PAGE + 0xff8,
    "b": SRC_PAGE,
    "bl": SRC_PAGE + 0x20,
    "bcond": SRC_PAGE + 0x40,
    "cbz": SRC_PAGE + 0x60,
    "tbz": SRC_PAGE + 0x80,
    "br": SRC_PAGE + 0xa0,
    "blr": SRC_PAGE + 0xc0,
    "ret": SRC_PAGE + 0xe0,
    "reg": SRC_PAGE + 0x100,
}


def edge_code() -> dict[int, int]:
    code: dict[int, int] = {}

    def put(at: int, *words: int) -> None:
        for index, word in enumerate(words):
            code[at + 4 * index] = word

    e = EDGES
    # Falls into the next page: 1 + 2 on this one, 4 on the next.
    put(e["fall"], 0x91000400, 0x91000800, 0x91001000, RET)
    put(TARGET, 0x91019000, RET_X15)
    put(e["b"], MOV_X15_X30, b(e["b"] + 4, TARGET))
    put(e["bl"], MOV_X15_X30, bl(e["bl"] + 4, TARGET), RET)
    put(e["bcond"], MOV_X15_X30, CMP_X1_X1,
        b_cond(0, e["bcond"] + 8, TARGET), RET)
    put(e["cbz"], MOV_X15_X30, MOV_X2_0, cbz(2, e["cbz"] + 8, TARGET), RET)
    put(e["tbz"], MOV_X15_X30, MOV_X2_0, tbz(2, 0, e["tbz"] + 8, TARGET), RET)
    put(e["br"], MOV_X15_X30, adr(16, e["br"] + 4, TARGET), BR_X16)
    put(e["blr"], MOV_X15_X30, adr(16, e["blr"] + 4, TARGET), BLR_X16, RET)
    put(e["ret"], MOV_X15_X30, adr(30, e["ret"] + 4, TARGET), RET)
    # x16 comes from the case: the other instance of the library.
    put(e["reg"], MOV_X15_X30, BR_X16)
    return code


def edge_functions() -> tuple[tuple[int, int], ...]:
    sizes = {"fall": 0x10, "bl": 0x10, "bcond": 0x10, "cbz": 0x10, "tbz": 0x10,
             "blr": 0x10, "ret": 0x10, "b": 8, "br": 0x0c, "reg": 8}
    return tuple((EDGES[name], size) for name, size in sizes.items()
                 ) + ((TARGET, 8),)


# A PLT stub: it has no unwind entry, only a .plt section, as in every
# library the linker produced. The GOT slot it reads is 0x100 into the page.
PLT = TEXT + 0x40
PLT_CODE = (
    0x90000010,                                     # adrp x16, #0
    0xf9408211,                                     # ldr x17, [x16, #0x100]
    0x91040210,                                     # add x16, x16, #0x100
    0xd61f0220,                                     # br x17
)


# ---- instruction forms ----------------------------------------------------
# One small function per form: the instructions of the form, then RET. The
# differential test starts cases at every function, so each form runs from
# random states against the engine, whatever the library it was written for
# contains. The words are built from the architecture's field layouts and
# checked against an assembler by the tooling test.

FORMS_AT = TEXT + 0x100
FORM_SIZE = 0x14           # a function of 0x10 bytes and a gap that keeps
                           # neighbouring ranges from merging
SIZES = ("b", "h", "w", "x")
ORDERINGS = ("", "a", "l", "al")      # none, acquire, release, both


def ordered(load: bool, size: int, rn: int, rt: int) -> int:
    return (size << 30) | (0x08dffc00 if load else 0x089ffc00) | \
        (rn << 5) | rt


def amo(size: int, acquire: int, release: int, rs: int, op: int, swap: bool,
        rn: int, rt: int) -> int:
    return (size << 30) | 0x38200000 | (acquire << 23) | (release << 22) | \
        (rs << 16) | (int(swap) << 15) | (op << 12) | (rn << 5) | rt


def cas(size: int, acquire: int, release: int, rs: int, rn: int, rt: int
        ) -> int:
    return (size << 30) | 0x08a07c00 | (acquire << 22) | (release << 15) | \
        (rs << 16) | (rn << 5) | rt


def mops(kind: str, stage: int, hint: int, rd: int, rn: int, rs: int) -> int:
    """kind: 'cpyf', 'cpy' or 'set'; hint is the option bits 15:12 (or, for
    SET, bits 13:12)."""
    if kind == "set":
        return 0x19c00400 | (rs << 16) | (stage << 14) | ((hint & 3) << 12) \
            | (rn << 5) | rd
    base = 0x19000400 if kind == "cpyf" else 0x1d000400
    return base | (stage << 22) | (rs << 16) | (hint << 12) | (rn << 5) | rd


def forms() -> list[tuple[str, list[int]]]:
    """(name, words) of every form; the words end before the RET."""
    out: list[tuple[str, list[int]]] = []
    # LDAR and STLR, by width, with real, virtual and zero registers and a
    # stack base (which steps).
    for size in range(4):
        for load in (True, False):
            tag = "ldar" if load else "stlr"
            out.append((f"{tag}{size}", [ordered(load, size, 1, 2)]))
            out.append((f"{tag}{size}_vt", [ordered(load, size, 1, 24)]))
            out.append((f"{tag}{size}_vb", [ordered(load, size, 25, 2)]))
            out.append((f"{tag}{size}_zt", [ordered(load, size, 1, 31)]))
            out.append((f"{tag}{size}_sp", [ordered(load, size, 31, 2)]))
            out.append((f"{tag}{size}_vv", [ordered(load, size, 18, 28)]))
    # The atomic memory operations: every operation, ordering and width.
    for op in range(8):
        for size in range(4):
            for order, (a, r) in enumerate(((0, 0), (1, 0), (0, 1), (1, 1))):
                out.append((f"amo{op}_{size}_{order}",
                            [amo(size, a, r, 3, op, False, 1, 2)]))
    for size in range(4):
        for order, (a, r) in enumerate(((0, 0), (1, 0), (0, 1), (1, 1))):
            out.append((f"swp_{size}_{order}",
                        [amo(size, a, r, 3, 0, True, 1, 2)]))
            out.append((f"cas_{size}_{order}",
                        [cas(size, a, r, 3, 1, 2)]))
    # Register variants of one operation of each kind: virtual operands,
    # equal operands, the zero register, a stack or virtual base.
    for rs, rn, rt, name in ((24, 1, 2, "vs"), (3, 1, 28, "vt"),
                             (24, 1, 28, "vsvt"), (25, 1, 25, "same"),
                             (31, 1, 2, "zs"), (3, 1, 31, "zt"),
                             (3, 18, 2, "vb"), (3, 31, 2, "sp"),
                             (2, 1, 2, "rsrt"), (2, 2, 3, "rnrs"),
                             (3, 3, 3, "all")):
        out.append((f"amo_add_{name}", [amo(3, 1, 1, rs, 0, False, rn, rt)]))
        out.append((f"swp_{name}", [amo(2, 0, 1, rs, 0, True, rn, rt)]))
        out.append((f"cas_{name}", [cas(3, 1, 1, rs, rn, rt)]))
        out.append((f"casb_{name}", [cas(0, 1, 0, rs, rn, rt)]))
    # Other words of the same groups: LDAPR (steps), the unallocated
    # neighbours of the atomic and ordered groups.
    out.append(("ldapr", [0xf8bfc022]))
    out.append(("ldlar", [0xc8df7c22]))
    out.append(("stllr", [0xc89f7c22]))
    out.append(("ldapr_b", [0x38bfc022]))
    # FEAT_MOPS: the three stages in a row, from the same registers, with
    # every option set, then variants with virtual registers (one is
    # inlined, two step) and the zero fill register.
    for kind in ("cpyf", "cpy", "set"):
        hints = (0, 1, 2, 3) if kind == "set" else (0, 4, 8, 12, 1, 2, 5, 10)
        for hint in hints:
            out.append((f"{kind}_{hint}", [mops(kind, 0, hint, 0, 2, 1),
                                           mops(kind, 1, hint, 0, 2, 1),
                                           mops(kind, 2, hint, 0, 2, 1)]))
    for name, d, n, s in (("vn", 0, 24, 1), ("vd", 25, 2, 1),
                          ("vs", 0, 2, 26), ("vdvn", 24, 25, 1),
                          ("vnvs", 0, 18, 28), ("same", 0, 0, 1),
                          ("zs", 0, 2, 31), ("xzr_d", 31, 2, 1),
                          ("high", 17, 16, 15)):
        for kind in ("cpyf", "cpy", "set"):
            out.append((f"{kind}_{name}",
                        [mops(kind, 0, 0, d, n, s), mops(kind, 1, 0, d, n, s),
                         mops(kind, 2, 0, d, n, s)]))
    # A prologue repeated without its epilogue, and a main stage alone.
    for kind in ("cpyf", "cpy", "set"):
        out.append((f"{kind}_pp", [mops(kind, 0, 0, 0, 2, 1),
                                   mops(kind, 0, 0, 0, 2, 1)]))
        out.append((f"{kind}_m", [mops(kind, 1, 0, 0, 2, 1)]))
        out.append((f"{kind}_e", [mops(kind, 2, 0, 0, 2, 1)]))
    # An ISB, which the engine runs, then a MOV: the code-change scenario
    # rewrites the MOV while the ISB's function is being entered.
    out.append(("isb_mov", [0xd5033fdf, 0xd2800020]))
    return out


def form_code() -> dict[int, int]:
    code = {}
    for index, (_, words) in enumerate(forms()):
        base = FORMS_AT + index * FORM_SIZE
        if len(words) + 1 > (FORM_SIZE - 4) // 4:
            raise ValueError("a form is too long")
        for position, word in enumerate(words):
            code[base + 4 * position] = word
        code[base + 4 * len(words)] = RET
    return code


def form_functions() -> tuple[tuple[int, int], ...]:
    return tuple((FORMS_AT + index * FORM_SIZE, FORM_SIZE - 4)
                 for index in range(len(forms())))


# ---- shared-memory loops --------------------------------------------------
# Three loops that add one to the 64-bit cell at [x2], x0 times, each by a
# different instruction family: LDADD (inlined as a native atomic), a CAS
# retry loop (inlined) and LDXR/STXR (left to the engine, which opens and
# closes the exclusive monitor between entries of rewritten code). The
# differential test runs one in rewritten code and another in the engine
# on the same cell, so a lost update shows as a final count that is short.
HAMMER_AT = TEXT + 0x1a00
HAMMER_ADD = HAMMER_AT
HAMMER_CAS = HAMMER_AT + 0x40
HAMMER_LLSC = HAMMER_AT + 0x80
SUBS_X0_X0_1 = 0xf1000400
MOV_X7_X5 = 0xaa0503e7
CMP_X7_X5 = 0xeb0500ff


def cbnz_w(rt: int, here: int, target: int) -> int:
    return 0x35000000 | (((target - here) // 4) & 0x7ffff) << 5 | rt


def hammer_code() -> dict[int, int]:
    code: dict[int, int] = {}

    def put(at: int, *words: int) -> None:
        for index, word in enumerate(words):
            code[at + 4 * index] = word

    a = HAMMER_ADD
    put(a, 0xd2800023,                              # movz x3, #1
        amo(3, 0, 0, 3, 0, False, 2, 4),            # ldadd x3, x4, [x2]
        SUBS_X0_X0_1, b_cond(1, a + 12, a + 4), RET)
    c = HAMMER_CAS
    put(c, 0xf9400045,                              # ldr x5, [x2]
        0x910004a6,                                 # add x6, x5, #1
        MOV_X7_X5,
        cas(3, 0, 0, 7, 2, 6),                      # cas x7, x6, [x2]
        CMP_X7_X5, b_cond(1, c + 20, c),            # retry on a lost race
        SUBS_X0_X0_1, b_cond(1, c + 28, c), RET)
    d = HAMMER_LLSC
    put(d, 0xc85f7c45,                              # ldxr x5, [x2]
        0x910004a5,                                 # add x5, x5, #1
        0xc8067c45,                                 # stxr w6, x5, [x2]
        cbnz_w(6, d + 12, d),
        SUBS_X0_X0_1, b_cond(1, d + 20, d), RET)
    return code


def hammer_functions() -> tuple[tuple[int, int], ...]:
    return ((HAMMER_ADD, 0x14), (HAMMER_CAS, 0x24), (HAMMER_LLSC, 0x1c))


# ---- the memory copy and set stages ---------------------------------------
# Three-stage sequences with fixed registers, which the differential test
# starts from chosen states (x0 destination, x1 source or fill value, x2
# count; the second set of rows names x17, x15 and x16). The words are the
# prologue, main and epilogue stages and a RET.
MOPS_AT = TEXT + 0x1b00
MOPS_STEP = 0x20
MOPS_ROWS = (("cpyf", 0, 2, 1), ("cpy", 0, 2, 1), ("set", 0, 2, 1),
             ("cpyf", 17, 16, 15), ("cpy", 17, 16, 15), ("set", 17, 16, 15),
             ("set", 0, 2, 31))


def mops_code() -> dict[int, int]:
    code: dict[int, int] = {}
    for index, (kind, d, n, s) in enumerate(MOPS_ROWS):
        base = MOPS_AT + index * MOPS_STEP
        for stage in range(3):
            code[base + 4 * stage] = mops(kind, stage, 0, d, n, s)
        code[base + 12] = RET
    return code


def mops_functions() -> tuple[tuple[int, int], ...]:
    return tuple((MOPS_AT + index * MOPS_STEP, 0x10)
                 for index in range(len(MOPS_ROWS)))


def eh_frame() -> bytes:
    # CIE: version 1, "zR", code alignment 4, data alignment -8, return
    # address x30, pointers are pc-relative signed 32-bit.
    cie_body = (struct.pack("<IBBB", 0, 1, ord("z"), ord("R")) + b"\0" +
                bytes([4, 0x78, 30, 1, 0x1b, 0x0c, 31, 0]))
    cie_body += bytes(-(len(cie_body) + 4) % 8)
    out = bytearray(struct.pack("<I", len(cie_body)) + cie_body)
    for start, size in (FUNCTIONS + form_functions() + edge_functions() +
                        hammer_functions() + mops_functions()):
        position = len(out)
        field = EH_FRAME + position + 8
        body = struct.pack("<Iii", position + 4, start - field, size)
        body += b"\0"
        body += bytes(-(len(body) + 4) % 8)
        out += struct.pack("<I", len(body)) + body
    out += struct.pack("<I", 0)
    return bytes(out)


def build() -> bytes:
    text = bytearray(PAGE * TEXT_PAGES)
    for offset in range(0, len(text), 4):
        struct.pack_into("<I", text, offset, BRK)
    for address, word in CODE.items():
        struct.pack_into("<I", text, address - TEXT, word)
    for address, word in form_code().items():
        struct.pack_into("<I", text, address - TEXT, word)
    for address, word in edge_code().items():
        struct.pack_into("<I", text, address - TEXT, word)
    if FORMS_AT + len(forms()) * FORM_SIZE > HAMMER_AT:
        raise ValueError("the forms run into the shared-memory loops")
    for address, word in hammer_code().items():
        struct.pack_into("<I", text, address - TEXT, word)
    for address, word in mops_code().items():
        struct.pack_into("<I", text, address - TEXT, word)
    for index, word in enumerate(PLT_CODE):
        struct.pack_into("<I", text, PLT - TEXT + 4 * index, word)
    frame = eh_frame()
    names = b"\0.text\0.plt\0.eh_frame\0.shstrtab\0"
    size_load = EH_FRAME + len(frame)
    image = bytearray(size_load)
    image[TEXT:TEXT + len(text)] = text
    image[EH_FRAME:EH_FRAME + len(frame)] = frame
    shstr_offset = len(image)
    image += names
    image += bytes(-len(image) % 8)
    section_offset = len(image)

    def section(name, kind, flags, addr, offset, size, align):
        return struct.pack("<IIQQQQIIQQ", name, kind, flags, addr, offset,
                           size, 0, 0, align, 0)

    image += section(0, 0, 0, 0, 0, 0, 0)
    image += section(1, 1, 6, TEXT, TEXT, len(text), 4)
    image += section(7, 1, 6, PLT, PLT, 4 * len(PLT_CODE), 16)
    image += section(12, 1, 2, EH_FRAME, EH_FRAME, len(frame), 8)
    image += section(22, 3, 0, 0, shstr_offset, len(names), 1)
    header = (b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8) +
              struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, 0, 64, section_offset,
                          0, 64, 56, 1, 64, 5, 4))
    program = struct.pack("<IIQQQQQQ", 1, 5, 0, 0, 0, size_load, size_load,
                          PAGE)
    image[0:len(header)] = header
    image[64:64 + len(program)] = program
    return bytes(image)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    arguments = parser.parse_args(argv)
    arguments.out.parent.mkdir(parents=True, exist_ok=True)
    arguments.out.write_bytes(build())
    return 0


if __name__ == "__main__":
    sys.exit(main())
