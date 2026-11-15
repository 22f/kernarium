# SPDX-License-Identifier: GPL-2.0-or-later
"""A64 encoders and a small label-resolving assembler with branch relaxation.

Only the instructions the rewriter emits are encoded here. Everything the
guest wrote is copied or re-encoded from its own word, never re-assembled.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import dataclasses
import struct

XZR = 31
SP = 31


class AsmError(Exception):
    pass


def _u(value: int, bits: int) -> int:
    if not 0 <= value < (1 << bits):
        raise AsmError(f"{value} does not fit in {bits} bits")
    return value


def _s(value: int, bits: int) -> int:
    low = -(1 << (bits - 1))
    if not low <= value < (1 << (bits - 1)):
        raise AsmError(f"{value} does not fit in {bits} signed bits")
    return value & ((1 << bits) - 1)


# ---- data processing ----------------------------------------------------

def add_imm(rd: int, rn: int, imm: int, *, sf: int = 1, sub: bool = False,
            shift12: bool = False) -> int:
    return ((sf << 31) | (int(sub) << 30) | (0b100010 << 23) |
            (int(shift12) << 22) | (_u(imm, 12) << 10) | (rn << 5) | rd)


def movz(rd: int, imm16: int, hw: int = 0, *, sf: int = 1) -> int:
    return ((sf << 31) | (0b10 << 29) | (0b100101 << 23) | (hw << 21) |
            (_u(imm16, 16) << 5) | rd)


def movk(rd: int, imm16: int, hw: int = 0, *, sf: int = 1) -> int:
    return ((sf << 31) | (0b11 << 29) | (0b100101 << 23) | (hw << 21) |
            (_u(imm16, 16) << 5) | rd)


def ubfm(rd: int, rn: int, immr: int, imms: int, *, sf: int = 1) -> int:
    return ((sf << 31) | (0b10 << 29) | (0b100110 << 23) | (sf << 22) |
            (_u(immr, 6) << 16) | (_u(imms, 6) << 10) | (rn << 5) | rd)


def ubfx(rd: int, rn: int, lsb: int, width: int) -> int:
    return ubfm(rd, rn, lsb, lsb + width - 1)


def lsr_imm(rd: int, rn: int, amount: int) -> int:
    return ubfm(rd, rn, amount, 63)


def ror_imm(rd: int, rn: int, amount: int) -> int:
    return ((1 << 31) | (0b100111 << 23) | (1 << 22) | (rn << 16) |
            (_u(amount, 6) << 10) | (rn << 5) | rd)


def add_reg(rd: int, rn: int, rm: int, *, shift: int = 0, amount: int = 0,
            sub: bool = False) -> int:
    return ((1 << 31) | (int(sub) << 30) | (0b01011 << 24) |
            (shift << 22) | (rm << 16) | (_u(amount, 6) << 10) | (rn << 5) | rd)


def add_ext(rd: int, rn: int, rm: int, option: int, amount: int) -> int:
    """ADD (extended register), 64-bit: option 0b010 UXTW, 0b011 UXTX/LSL,
    0b110 SXTW, 0b111 SXTX."""
    return ((1 << 31) | (0b01011001 << 21) | (rm << 16) | (option << 13) |
            (_u(amount, 3) << 10) | (rn << 5) | rd)


def eor_reg(rd: int, rn: int, rm: int, *, shift: int = 0, amount: int = 0
            ) -> int:
    return ((1 << 31) | (0b10 << 29) | (0b01010 << 24) | (shift << 22) |
            (rm << 16) | (_u(amount, 6) << 10) | (rn << 5) | rd)


def mov_reg(rd: int, rm: int) -> int:
    return (1 << 31) | (0b01 << 29) | (0b01010 << 24) | (rm << 16) | \
        (XZR << 5) | rd


def nop() -> int:
    return 0xd503201f


# ---- memory --------------------------------------------------------------

def ldst_imm(rt: int, rn: int, imm: int, size: int, *, load: bool,
             vector: bool = False, opc: int | None = None) -> int:
    """Unsigned-offset LDR/STR; imm is in bytes and a multiple of the size."""
    if opc is None:
        opc = 1 if load else 0
    scale = size if not (vector and opc & 2) else 4
    if imm % (1 << scale):
        raise AsmError("unaligned scaled offset")
    return ((size << 30) | (0b111 << 27) | (int(vector) << 26) | (1 << 24) |
            (opc << 22) | (_u(imm >> scale, 12) << 10) | (rn << 5) | rt)


def ldr_x(rt: int, rn: int, imm: int) -> int:
    return ldst_imm(rt, rn, imm, 3, load=True)


def str_x(rt: int, rn: int, imm: int) -> int:
    return ldst_imm(rt, rn, imm, 3, load=False)


def ldr_w(rt: int, rn: int, imm: int) -> int:
    return ldst_imm(rt, rn, imm, 2, load=True)


def str_w(rt: int, rn: int, imm: int) -> int:
    return ldst_imm(rt, rn, imm, 2, load=False)


def ldrb_w(rt: int, rn: int, imm: int) -> int:
    return ldst_imm(rt, rn, imm, 0, load=True)


def strb_w(rt: int, rn: int, imm: int) -> int:
    return ldst_imm(rt, rn, imm, 0, load=False)


def ldr_q(rt: int, rn: int, imm: int) -> int:
    return ldst_imm(rt, rn, imm, 0, load=True, vector=True, opc=3)


def str_q(rt: int, rn: int, imm: int) -> int:
    return ldst_imm(rt, rn, imm, 0, load=False, vector=True, opc=2)


def ldr_reg_x(rt: int, rn: int, rm: int) -> int:
    """LDR Xt, [Xn, Xm] (register offset, LSL #0)."""
    return 0xf8606800 | (rm << 16) | (rn << 5) | rt


def ldr_reg_w(rt: int, rn: int, rm: int) -> int:
    """LDR Wt, [Xn, Xm]."""
    return 0xb8606800 | (rm << 16) | (rn << 5) | rt


def ldp_x(rt: int, rt2: int, rn: int, imm: int, *, load: bool = True) -> int:
    return ((0b10 << 30) | (0b101 << 27) | (0b010 << 23) |
            (int(load) << 22) | (_s(imm >> 3, 7) << 15) | (rt2 << 10) |
            (rn << 5) | rt)


def stp_x(rt: int, rt2: int, rn: int, imm: int) -> int:
    return ldp_x(rt, rt2, rn, imm, load=False)


def ldp_q(rt: int, rt2: int, rn: int, imm: int, *, load: bool = True) -> int:
    return ((0b10 << 30) | (0b101 << 27) | (1 << 26) | (0b010 << 23) |
            (int(load) << 22) | (_s(imm >> 4, 7) << 15) | (rt2 << 10) |
            (rn << 5) | rt)


def stp_q(rt: int, rt2: int, rn: int, imm: int) -> int:
    return ldp_q(rt, rt2, rn, imm, load=False)


def ldp_d(rt: int, rt2: int, rn: int, imm: int, *, load: bool = True) -> int:
    return ((0b01 << 30) | (0b101 << 27) | (1 << 26) | (0b010 << 23) |
            (int(load) << 22) | (_s(imm >> 3, 7) << 15) | (rt2 << 10) |
            (rn << 5) | rt)


# ---- system --------------------------------------------------------------

SYSREG = {"nzcv": 0xd53b4200, "fpcr": 0xd53b4400, "fpsr": 0xd53b4420}


def mrs(rt: int, name: str) -> int:
    return SYSREG[name] | rt


def msr(name: str, rt: int) -> int:
    return (SYSREG[name] ^ 0x00200000) | rt


def ret_() -> int:
    return 0xd65f03c0


def blr(rn: int) -> int:
    return 0xd63f0000 | (rn << 5)


def br(rn: int) -> int:
    return 0xd61f0000 | (rn << 5)


def ldrb_zero_ext(rt: int, rn: int, imm: int) -> int:
    return ldrb_w(rt, rn, imm)


# ---- control flow words, targets patched by the assembler ----------------

def b_word(delta_words: int) -> int:
    return (0b000101 << 26) | _s(delta_words, 26)


def bcond_word(cond: int, delta_words: int) -> int:
    return 0x54000000 | (_s(delta_words, 19) << 5) | cond


def cbz_word(rt: int, delta_words: int, *, sf: int = 1, nz: bool = False
             ) -> int:
    return ((sf << 31) | (0b011010 << 25) | (int(nz) << 24) |
            (_s(delta_words, 19) << 5) | rt)


def tbz_word(rt: int, bit: int, delta_words: int, *, nz: bool = False) -> int:
    return (((bit >> 5) << 31) | (0b011011 << 25) | (int(nz) << 24) |
            ((bit & 31) << 19) | (_s(delta_words, 14) << 5) | rt)


def adr_word(rd: int, delta: int) -> int:
    delta &= (1 << 21) - 1
    return ((delta & 3) << 29) | (0b10000 << 24) | ((delta >> 2) << 5) | rd


def adrp_word(rd: int, delta_pages: int) -> int:
    delta_pages &= (1 << 21) - 1
    return (1 << 31) | ((delta_pages & 3) << 29) | (0b10000 << 24) | \
        ((delta_pages >> 2) << 5) | rd


# ---- the assembler -------------------------------------------------------

@dataclasses.dataclass
class _Item:
    kind: str
    args: tuple
    long: bool = False
    address: int = 0

    def size(self) -> int:
        if self.kind == "word":
            return 4
        if self.kind == "align":
            return 0  # computed in layout
        if self.kind == "adrp_add":
            return 8
        if self.kind in ("bc", "cbz", "tbz"):
            return 8 if self.long else 4
        if self.kind == "b":
            return 4
        if self.kind == "data":
            return len(self.args[0])
        raise AsmError(self.kind)


class Assembler:
    """Collects words and label references, then lays them out."""

    def __init__(self) -> None:
        self._items: list[_Item] = []
        self._labels: dict[str, int] = {}
        self._label_at: dict[int, list[str]] = {}
        self._pending_labels: list[str] = []

    # emission
    def label(self, name: str) -> None:
        if name in self._labels or name in self._pending_labels:
            raise AsmError(f"duplicate label {name}")
        self._pending_labels.append(name)

    def _push(self, item: _Item) -> None:
        index = len(self._items)
        for name in self._pending_labels:
            self._labels[name] = index
        self._pending_labels.clear()
        self._items.append(item)

    def word(self, value: int) -> None:
        self._push(_Item("word", (value & 0xffffffff,)))

    def words(self, values) -> None:
        for value in values:
            self.word(value)

    def data(self, blob: bytes) -> None:
        self._push(_Item("data", (bytes(blob),)))

    def align(self, boundary: int) -> None:
        self._push(_Item("align", (boundary,)))

    def b(self, target: str) -> None:
        self._push(_Item("b", (target,)))

    def bcond(self, cond: int, target: str) -> None:
        self._push(_Item("bc", (cond, target)))

    def cbz(self, rt: int, target: str, *, sf: int = 1, nz: bool = False
            ) -> None:
        self._push(_Item("cbz", (rt, target, sf, nz)))

    def tbz(self, rt: int, bit: int, target: str, *, nz: bool = False
            ) -> None:
        self._push(_Item("tbz", (rt, bit, target, nz)))

    def adrp_add(self, rd: int, target: str) -> None:
        self._push(_Item("adrp_add", (rd, target)))

    # layout
    def _layout(self) -> int:
        address = 0
        for item in self._items:
            if item.kind == "align":
                boundary = item.args[0]
                padded = (address + boundary - 1) // boundary * boundary
                item.address = address
                item.args = (boundary, padded - address)
                address = padded
            else:
                item.address = address
                address += item.size()
        return address

    def _label_address(self, name: str) -> int:
        try:
            index = self._labels[name]
        except KeyError:
            raise AsmError(f"undefined label {name}") from None
        if index == len(self._items):
            return self._end
        return self._items[index].address

    def resolve(self) -> bytes:
        """Lay out with relaxation, then return the encoded bytes."""
        if self._pending_labels:
            for name in self._pending_labels:
                self._labels[name] = len(self._items)
            self._pending_labels.clear()
        while True:
            self._end = self._layout()
            changed = False
            for item in self._items:
                if item.kind in ("bc", "cbz", "tbz") and not item.long:
                    target = item.args[1] if item.kind != "tbz" else item.args[2]
                    delta = self._label_address(target) - item.address
                    reach = 1 << (15 if item.kind == "tbz" else 20)
                    if not -reach <= delta < reach:
                        item.long = True
                        changed = True
            if not changed:
                break
        out = bytearray()
        for item in self._items:
            assert len(out) == item.address, (len(out), item.address)
            out += self._encode(item)
        assert len(out) == self._end
        return bytes(out)

    def label_address(self, name: str) -> int:
        return self._label_address(name)

    def _encode(self, item: _Item) -> bytes:
        kind = item.kind
        if kind == "word":
            return struct.pack("<I", item.args[0])
        if kind == "data":
            return item.args[0]
        if kind == "align":
            return b"\0" * item.args[1]
        here = item.address
        if kind == "b":
            delta = self._label_address(item.args[0]) - here
            return struct.pack("<I", b_word(delta // 4))
        if kind == "adrp_add":
            rd, target = item.args
            address = self._label_address(target)
            pages = (address >> 12) - (here >> 12)
            return struct.pack("<II", adrp_word(rd, pages),
                               add_imm(rd, rd, address & 0xfff))
        if kind == "bc":
            cond, target = item.args
            delta = self._label_address(target) - here
            if not item.long:
                return struct.pack("<I", bcond_word(cond, delta // 4))
            skip = bcond_word(cond ^ 1, 2)
            return struct.pack("<II", skip, b_word((delta - 4) // 4))
        if kind == "cbz":
            rt, target, sf, nz = item.args
            delta = self._label_address(target) - here
            if not item.long:
                return struct.pack("<I", cbz_word(rt, delta // 4, sf=sf, nz=nz))
            skip = cbz_word(rt, 2, sf=sf, nz=not nz)
            return struct.pack("<II", skip, b_word((delta - 4) // 4))
        if kind == "tbz":
            rt, bit, target, nz = item.args
            delta = self._label_address(target) - here
            if not item.long:
                return struct.pack("<I", tbz_word(rt, bit, delta // 4, nz=nz))
            skip = tbz_word(rt, bit, 2, nz=not nz)
            return struct.pack("<II", skip, b_word((delta - 4) // 4))
        raise AsmError(kind)
