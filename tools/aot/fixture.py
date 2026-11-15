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
EH_FRAME = 0x2000


def b_cond(cond: int, here: int, target: int) -> int:
    return 0x54000000 | (((target - here) // 4) & 0x7ffff) << 5 | cond


def b(here: int, target: int) -> int:
    return 0x14000000 | (((target - here) // 4) & 0x3ffffff)


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

# A PLT stub: it has no unwind entry, only a .plt section, as in every
# library the linker produced. The GOT slot it reads is 0x100 into the page.
PLT = TEXT + 0x40
PLT_CODE = (
    0x90000010,                                     # adrp x16, #0
    0xf9408211,                                     # ldr x17, [x16, #0x100]
    0x91040210,                                     # add x16, x16, #0x100
    0xd61f0220,                                     # br x17
)


def eh_frame() -> bytes:
    # CIE: version 1, "zR", code alignment 4, data alignment -8, return
    # address x30, pointers are pc-relative signed 32-bit.
    cie_body = (struct.pack("<IBBB", 0, 1, ord("z"), ord("R")) + b"\0" +
                bytes([4, 0x78, 30, 1, 0x1b, 0x0c, 31, 0]))
    cie_body += bytes(-(len(cie_body) + 4) % 8)
    out = bytearray(struct.pack("<I", len(cie_body)) + cie_body)
    for start, size in FUNCTIONS:
        position = len(out)
        field = EH_FRAME + position + 8
        body = struct.pack("<Iii", position + 4, start - field, size)
        body += b"\0"
        body += bytes(-(len(body) + 4) % 8)
        out += struct.pack("<I", len(body)) + body
    out += struct.pack("<I", 0)
    return bytes(out)


def build() -> bytes:
    text = bytearray(PAGE)
    for offset in range(0, PAGE, 4):
        struct.pack_into("<I", text, offset, BRK)
    for address, word in CODE.items():
        struct.pack_into("<I", text, address - TEXT, word)
    for index, word in enumerate(PLT_CODE):
        struct.pack_into("<I", text, PLT - TEXT + 4 * index, word)
    frame = eh_frame()
    names = b"\0.text\0.plt\0.eh_frame\0.shstrtab\0"
    size_load = EH_FRAME + len(frame)
    image = bytearray(size_load)
    image[TEXT:TEXT + PAGE] = text
    image[EH_FRAME:EH_FRAME + len(frame)] = frame
    shstr_offset = len(image)
    image += names
    image += bytes(-len(image) % 8)
    section_offset = len(image)

    def section(name, kind, flags, addr, offset, size, align):
        return struct.pack("<IIQQQQIIQQ", name, kind, flags, addr, offset,
                           size, 0, 0, align, 0)

    image += section(0, 0, 0, 0, 0, 0, 0)
    image += section(1, 1, 6, TEXT, TEXT, PAGE, 4)
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
