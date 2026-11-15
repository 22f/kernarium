#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Rewrite arm64 Linux libraries into one position-independent AOT blob.

The blob is the whole content of the AOT framework's __TEXT,__text: the
entry/exit routines, the rewritten code, the per-instruction ENTRY tables and
the page digests that bind the code to the guest bytes it stands for. See
docs/architecture.md (ahead-of-time code) for the contract this implements.

Every guest instruction is either inlined or becomes a STEP: AOT code leaves
at that instruction with committed state and the present engine executes it.
The rewriter therefore never needs to refuse for correctness.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import collections
import dataclasses
import json
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import a64decode  # noqa: E402
import asm as A  # noqa: E402
import elf64  # noqa: E402

# ---- the register plan (docs/architecture.md, section "AOT code") -------

BUD, TLB, T0, T1, T2 = 24, 25, 26, 27, 28
SCRATCH = (T0, T1, T2)

FRAME = 0x100
SLOT_CPU, SLOT_BIAS, SLOT_BUD0, SLOT_SP = 0xA0, 0xA8, 0xB0, 0xB8
SLOT_REFILL = 0xF0
# The refill thunk's own frame: x0-x17, x30, NZCV, then q0-q31.
REFILL_FRAME = 0x2a0
VSLOT = {18: 0xC0, 24: 0xC8, 25: 0xD0, 26: 0xD8, 27: 0xE0, 28: 0xE8}
VIRTUAL = frozenset(VSLOT) | {"sp"}

EXIT_BUDGET, EXIT_BRANCH, EXIT_STEP = 0, 1, 2
ABSOLUTE = 0x100

PAGE = 4096
ENTRY_MAGIC = b"UMLAOTB1"
HEADER_SIZE = 256
LIB_RECORD_SIZE = 256


def slot_of(guest) -> int:
    return SLOT_SP if guest == "sp" else VSLOT[guest]


def sext(value: int, bits: int) -> int:
    return (value & ((1 << bits) - 1)) - ((value & (1 << (bits - 1))) << 1)


# ---- the CPU state layout, read from the repository's own contract -------

def read_layout(header: pathlib.Path) -> dict[str, int]:
    values: dict[str, int] = {}
    pattern = re.compile(r"^#define\s+(\w+)\s+(.+?)\s*(?:/\*.*)?$")
    for line in header.read_text().splitlines():
        match = pattern.match(line)
        if not match:
            continue
        expression = match.group(2)
        try:
            values[match.group(1)] = int(eval(  # noqa: S307
                expression, {"__builtins__": {}}, dict(values)))
        except (NameError, SyntaxError, TypeError):
            pass
    return values


LAYOUT_HEADER = (pathlib.Path(__file__).resolve().parents[2] / "darwin" /
                 "cpu" / "arm64" / "layout" / "assembly_offsets.h")


def crc32c_table() -> list[int]:
    table = []
    for index in range(256):
        value = index
        for _ in range(8):
            value = (value >> 1) ^ (0x82F63B78 if value & 1 else 0)
        table.append(value)
    return table


_CRC_TABLE = crc32c_table()


def crc32c_raw(data: bytes, crc: int) -> int:
    """The hardware CRC32C instruction folded over `data`: no pre/post xor."""
    table = _CRC_TABLE
    for byte in data:
        crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8)
    return crc


def page_digest(page: bytes) -> int:
    """The 64-bit identity of one 4 KiB page (see aot_digest.hpp)."""
    low = crc32c_raw(page, 0)
    high = crc32c_raw(page, 0x9e3779b9)
    digest = (high << 32) | low
    return digest or 1


# ---- classification policy ----------------------------------------------

BASE_COPY_CLASSES = {
    "addsub_imm", "addsub_shift", "addsub_ext", "addsub_carry", "log_imm",
    "log_shift", "movewide", "bitfield", "extract", "condsel", "condcmp_reg",
    "condcmp_imm", "dp_3src",
}
DP1_ALLOWED = {"RBIT", "REV16", "REV", "REV32", "CLZ", "CLS"}
DP2_ALLOWED = {"UDIV", "SDIV", "LSLV", "LSRV", "ASRV", "RORV", "CRC32B",
               "CRC32H", "CRC32W", "CRC32X", "CRC32CB", "CRC32CH",
               "CRC32CW", "CRC32CX"}
SIMD_CLASSES = {
    "asimdsame", "asimdmisc", "asimdall", "asimddiff", "asimdelem",
    "asimdext", "asimdperm", "asimdins", "asimdshf", "asimdtbl", "asimdimm",
    "asisdsame", "asisdmisc", "asisdone", "asisdpair", "asisdshf",
    "asisddiff", "asisdelem", "cryptoaes", "cryptosha2", "cryptosha3",
}
SIMD_FEATURES = {"FEAT_AdvSIMD", "FEAT_AES", "FEAT_PMULL", "FEAT_SHA1",
                 "FEAT_SHA256"}
FP_NAMES = {"SCVTF", "UCVTF"}
MEM_CLASSES = {
    "ldst_pos", "ldst_unscaled", "ldst_immpre", "ldst_immpost", "ldst_regoff",
    "ldstpair_off", "ldstpair_pre", "ldstpair_post", "ldstnapair_offs",
    "loadlit",
}
PAIR_CLASSES = {"ldstpair_off", "ldstpair_pre", "ldstpair_post",
                "ldstnapair_offs"}
MEM_FEATURES = {"baseline", "FEAT_FP"}
TPIDR_EL0 = 0xd53bd040
TPIDR_MASK = 0xffffffe0


@dataclasses.dataclass
class Stats:
    functions: int = 0
    instructions: int = 0
    inlined: int = 0
    steps: int = 0
    steps_by_cause: collections.Counter = dataclasses.field(
        default_factory=collections.Counter)
    kinds: collections.Counter = dataclasses.field(
        default_factory=collections.Counter)


class Library:
    """One library's rewrite: classification, then emission into the blob."""

    def __init__(self, index: int, name: str, elf: elf64.Elf,
                 decoder: a64decode.Decoder, layout: dict[str, int]) -> None:
        self.index = index
        self.name = name
        self.elf = elf
        self.decoder = decoder
        self.layout = layout
        self.stats = Stats()
        segment = elf.executable_segment()
        self.segment = segment
        self.table_base = segment.vaddr & ~(PAGE - 1)
        end = (segment.vaddr + segment.filesz + PAGE - 1) & ~(PAGE - 1)
        self.table_end = end
        self.table_words = (end - self.table_base) // 4
        span = end - self.table_base
        self.log_span = max(span - 1, 1).bit_length()
        self.functions = [f for f in elf.functions()
                          if segment.vaddr <= f.start and
                          f.end <= segment.vaddr + segment.filesz and
                          f.start % 4 == 0 and f.end % 4 == 0]
        self.covered: set[int] = set()
        for function in self.functions:
            self.covered.update(range(function.start, function.end, 4))
        self.words = {}
        for function in self.functions:
            data = elf.read_vaddr(function.start, function.end - function.start)
            for offset in range(0, len(data), 4):
                self.words[function.start + offset] = struct.unpack_from(
                    "<I", data, offset)[0]
        self.step_sites: set[int] = set()
        self.miss_sites: dict[int, str] = {}
        self.budget_sites: dict[int, int] = {}
        self.exit_targets: set[int] = set()
        self.entry_labels: list[int] = []

    # labels
    def lab(self, kind: str, value: int) -> str:
        return f"L{self.index}{kind}{value:x}"

    def goto_label(self, target: int) -> str:
        if target in self.covered:
            return self.lab("G", target)
        self.exit_targets.add(target)
        return self.lab("XB", target)

    # emission -------------------------------------------------------------
    def emit(self, asm: A.Assembler) -> None:
        self.asm = asm
        for function in self.functions:
            self.stats.functions += 1
            self._emit_function(function)
        self._emit_stubs()

    def _loop_headers(self, function: elf64.Function) -> dict[int, int]:
        headers: dict[int, int] = {}
        for offset in range(function.start, function.end, 4):
            word = self.words[offset]
            target = self._direct_target(word, offset)
            if target is None or not (function.start <= target <= offset):
                continue
            extent = (offset - target) // 4 + 1
            headers[target] = max(headers.get(target, 0), extent)
        return headers

    def _direct_target(self, word: int, offset: int) -> int | None:
        encoding = self.decoder.decode(word)
        if encoding is None:
            return None
        cls = encoding.decode_class
        if cls == "branch_imm":
            return offset + sext(word & 0x3ffffff, 26) * 4
        if cls == "condbranch" and encoding.mnemonic == "B":
            return offset + sext((word >> 5) & 0x7ffff, 19) * 4
        if cls == "compbranch":
            return offset + sext((word >> 5) & 0x7ffff, 19) * 4
        if cls == "testbranch":
            return offset + sext((word >> 5) & 0x3fff, 14) * 4
        return None

    def _emit_function(self, function: elf64.Function) -> None:
        asm = self.asm
        headers = self._loop_headers(function)
        last_falls_through = True
        for offset in range(function.start, function.end, 4):
            word = self.words[offset]
            asm.label(self.lab("G", offset))
            extent = 8 if offset == function.start else 0
            if offset in headers:
                extent = max(extent, headers[offset])
            if extent:
                extent = min(extent, 4095)
                site = self.lab("XU", offset)
                self.budget_sites[offset] = extent
                asm.word(A.add_imm(BUD, BUD, extent, sub=True))
                asm.tbz(BUD, 63, site, nz=True)
            asm.label(self.lab("E", offset))
            self.entry_labels.append(offset)
            self.stats.instructions += 1
            last_falls_through = self._emit_instruction(offset, word)
        if last_falls_through:
            asm.b(self.goto_label(function.end))

    # classification of one instruction; returns whether it falls through
    def _emit_instruction(self, offset: int, word: int) -> bool:
        encoding = self.decoder.decode(word)
        if encoding is None:
            return self._step(offset, "unallocated")
        cls, mnemonic, feature = (encoding.decode_class, encoding.mnemonic,
                                  encoding.feature)
        if word == 0xd503201f:
            self.stats.inlined += 1
            self.stats.kinds["nop"] += 1
            return True
        if cls == "branch_imm":
            return self._branch_imm(offset, word, mnemonic == "BL")
        if cls == "condbranch" and mnemonic == "B":
            target = offset + sext((word >> 5) & 0x7ffff, 19) * 4
            return self._cond_branch(offset, "bcond", word & 15, target)
        if cls == "compbranch":
            target = offset + sext((word >> 5) & 0x7ffff, 19) * 4
            return self._compare_branch(offset, word, target)
        if cls == "testbranch":
            target = offset + sext((word >> 5) & 0x3fff, 14) * 4
            return self._test_branch(offset, word, target)
        if cls == "branch_reg" and mnemonic in ("BR", "BLR", "RET"):
            return self._indirect_branch(offset, word, mnemonic)
        if cls == "pcreladdr":
            return self._pc_relative(offset, word, mnemonic == "ADRP")
        if cls in MEM_CLASSES and feature in MEM_FEATURES:
            return self._memory(offset, word, encoding)
        if cls == "systemmove" and (word & TPIDR_MASK) in (
                TPIDR_EL0, TPIDR_EL0 ^ 0x00200000):
            return self._tpidr(offset, word)
        if cls == "systemmove" and (word & 0xffffffe0) in (
                0xd53b4200, 0xd51b4200):
            return self._copy(offset, word, encoding, [(0, False)])
        if cls == "barriers" and mnemonic == "DMB":
            self.asm.word(word)
            self.stats.inlined += 1
            self.stats.kinds["dmb"] += 1
            return True
        fields = self._copy_fields(word, encoding)
        if fields is not None:
            return self._copy(offset, word, encoding, fields)
        return self._step(offset, f"{cls}:{mnemonic}:{feature}")

    def _copy_fields(self, word: int, encoding: a64decode.Encoding):
        cls, mnemonic, feature = (encoding.decode_class, encoding.mnemonic,
                                  encoding.feature)
        if cls in BASE_COPY_CLASSES and feature == "baseline":
            s_bit = (word >> 29) & 1
            if cls == "addsub_imm":
                return [(0, not s_bit), (5, True)]
            if cls == "addsub_ext":
                return [(0, not s_bit), (5, True), (16, False)]
            if cls in ("addsub_shift", "addsub_carry", "log_shift", "condsel",
                       "extract"):
                return [(0, False), (5, False), (16, False)]
            if cls == "log_imm":
                return [(0, ((word >> 29) & 3) != 3), (5, False)]
            if cls == "movewide":
                return [(0, False)]
            if cls == "bitfield":
                return [(0, False), (5, False)]
            if cls == "condcmp_reg":
                return [(5, False), (16, False)]
            if cls == "condcmp_imm":
                return [(5, False)]
            if cls == "dp_3src":
                return [(0, False), (5, False), (10, False), (16, False)]
        if cls == "dp_1src" and feature == "baseline" and \
                mnemonic in DP1_ALLOWED:
            return [(0, False), (5, False)]
        if cls == "dp_2src" and feature in ("baseline", "FEAT_CRC32") and \
                mnemonic in DP2_ALLOWED:
            return [(0, False), (5, False), (16, False)]
        if cls == "float2int" and mnemonic == "FMOV" and \
                feature in ("FEAT_FP", "baseline"):
            opcode = (word >> 16) & 7
            if opcode == 6:
                return [(0, False)]
            if opcode == 7:
                return [(5, False)]
            return None
        if cls in SIMD_CLASSES and feature in SIMD_FEATURES and \
                mnemonic not in FP_NAMES and not mnemonic.startswith("F"):
            if cls == "asimdins":
                imm4 = (word >> 11) & 15
                if imm4 in (1, 3):
                    return [(5, False)]
                if imm4 in (5, 7):
                    return [(0, False)]
            return []
        return None

    # --- generic helpers ---------------------------------------------------
    def _resolve(self, value: int, sp_capable: bool):
        if value == 31:
            return "sp" if sp_capable else "zr"
        return value

    def _step(self, offset: int, cause: str) -> bool:
        self.step_sites.add(offset)
        self.asm.b(self.lab("XS", offset))
        self.stats.steps += 1
        self.stats.steps_by_cause[cause] += 1
        return False

    def _step_label(self, offset: int) -> str:
        self.step_sites.add(offset)
        return self.lab("XS", offset)

    def _load_guest(self, host: int, guest) -> None:
        self.asm.word(A.ldr_x(host, 31, slot_of(guest)))

    def _store_guest(self, host: int, guest) -> None:
        self.asm.word(A.str_x(host, 31, slot_of(guest)))

    def _materialize(self, host: int, vaddr: int) -> None:
        """host = BIAS + vaddr (a guest virtual address)."""
        asm = self.asm
        asm.word(A.ldr_x(host, 31, SLOT_BIAS))
        low = vaddr & 0xfff
        high = (vaddr >> 12) & 0xfff
        if vaddr >= (1 << 24):
            asm.word(A.movz(T1 if host != T1 else T2, vaddr & 0xffff))
            scratch = T1 if host != T1 else T2
            asm.word(A.movk(scratch, (vaddr >> 16) & 0xffff, 1))
            if vaddr >> 32:
                asm.word(A.movk(scratch, (vaddr >> 32) & 0xffff, 2))
            asm.word(A.add_reg(host, host, scratch))
            return
        if low:
            asm.word(A.add_imm(host, host, low))
        if high:
            asm.word(A.add_imm(host, host, high, shift12=True))

    # --- plain instructions ---------------------------------------------------
    def _copy(self, offset: int, word: int, encoding, fields) -> bool:
        needed: list = []
        for lsb, sp_capable in fields:
            guest = self._resolve((word >> lsb) & 31, sp_capable)
            if guest in VIRTUAL and guest not in needed:
                needed.append(guest)
        if not needed:
            self.asm.word(word)
        elif len(needed) > len(SCRATCH):
            return self._step(offset, "operands")
        else:
            mapping = dict(zip(needed, SCRATCH))
            patched = word
            for guest in needed:
                self._load_guest(mapping[guest], guest)
            for lsb, sp_capable in fields:
                guest = self._resolve((word >> lsb) & 31, sp_capable)
                if guest in mapping:
                    patched = (patched & ~(31 << lsb)) | (mapping[guest] << lsb)
            self.asm.word(patched)
            for guest in needed:
                self._store_guest(mapping[guest], guest)
        self.stats.inlined += 1
        self.stats.kinds[encoding.mnemonic] += 1
        return True

    def _tpidr(self, offset: int, word: int) -> bool:
        asm = self.asm
        rt = word & 31
        is_read = (word & 0x00200000) != 0
        guest = self._resolve(rt, False)
        asm.word(A.ldr_x(T0, 31, SLOT_CPU))
        tls = self.layout["UML_GADGET_CPU_TLS"]
        if is_read:
            if guest == "zr":
                pass
            elif guest in VIRTUAL:
                asm.word(A.ldr_x(T1, T0, tls))
                self._store_guest(T1, guest)
            else:
                asm.word(A.ldr_x(rt, T0, tls))
        else:
            if guest == "zr":
                asm.word(A.str_x(31, T0, tls))
            elif guest in VIRTUAL:
                self._load_guest(T1, guest)
                asm.word(A.str_x(T1, T0, tls))
            else:
                asm.word(A.str_x(rt, T0, tls))
        self.stats.inlined += 1
        self.stats.kinds["tpidr"] += 1
        return True

    # --- control flow -------------------------------------------------------
    def _branch_imm(self, offset: int, word: int, link: bool) -> bool:
        target = offset + sext(word & 0x3ffffff, 26) * 4
        if link:
            self._materialize(30, offset + 4)
        self.asm.b(self.goto_label(target))
        self.stats.inlined += 1
        self.stats.kinds["bl" if link else "b"] += 1
        return False

    def _cond_branch(self, offset: int, kind: str, cond: int, target: int
                     ) -> bool:
        self.asm.bcond(cond, self.goto_label(target))
        self.stats.inlined += 1
        self.stats.kinds["bcond"] += 1
        return True

    def _compare_branch(self, offset: int, word: int, target: int) -> bool:
        sf = word >> 31
        nz = bool((word >> 24) & 1)
        guest = self._resolve(word & 31, False)
        rt = word & 31
        if guest in VIRTUAL:
            self._load_guest(T0, guest)
            rt = T0
        self.asm.cbz(rt, self.goto_label(target), sf=sf, nz=nz)
        self.stats.inlined += 1
        self.stats.kinds["cbz"] += 1
        return True

    def _test_branch(self, offset: int, word: int, target: int) -> bool:
        nz = bool((word >> 24) & 1)
        bit = ((word >> 31) << 5) | ((word >> 19) & 31)
        guest = self._resolve(word & 31, False)
        rt = word & 31
        if guest in VIRTUAL:
            self._load_guest(T0, guest)
            rt = T0
        self.asm.tbz(rt, bit, self.goto_label(target), nz=nz)
        self.stats.inlined += 1
        self.stats.kinds["tbz"] += 1
        return True

    def _indirect_branch(self, offset: int, word: int, mnemonic: str) -> bool:
        asm = self.asm
        rn = (word >> 5) & 31
        site = self.lab("XU", offset)
        self.budget_sites[offset] = 4
        asm.word(A.add_imm(BUD, BUD, 4, sub=True))
        asm.tbz(BUD, 63, site, nz=True)
        guest = self._resolve(rn, False)
        if guest in VIRTUAL:
            self._load_guest(T0, guest)
        else:
            asm.word(A.mov_reg(T0, rn))
        if mnemonic == "BLR":
            self._materialize(30, offset + 4)
        asm.word(A.ldr_x(T1, 31, SLOT_BIAS))
        asm.word(A.add_reg(T1, T0, T1, sub=True))  # vaddr
        base_pages = self.table_base >> 12
        if base_pages:
            asm.word(A.add_imm(T1, T1, base_pages, sub=True, shift12=True))
        asm.word(A.ror_imm(T2, T1, 2))
        asm.word(A.lsr_imm(T2, T2, self.log_span - 2))
        asm.cbz(T2, "EXIT_BRANCH_ABS", nz=True)
        # The test above passes anything below the next power of two; the
        # table covers exactly the code span.
        span = self.table_end - self.table_base
        if (span >> 12) < 4096 and span % 4096 == 0:
            asm.word(A.add_imm(T2, T1, span >> 12, sub=True, shift12=True))
        else:
            asm.word(A.movz(T2, span & 0xffff))
            asm.word(A.movk(T2, (span >> 16) & 0xffff, 1))
            asm.word(A.add_reg(T2, T1, T2, sub=True))
        asm.tbz(T2, 63, "EXIT_BRANCH_ABS", nz=False)
        asm.adrp_add(T2, self.lab("TAB", 0))
        asm.word(A.ldr_reg_w(T1, T2, T1))
        asm.cbz(T1, "EXIT_BRANCH_ABS", sf=0, nz=False)
        asm.adrp_add(T2, "BLOB")
        asm.word(A.add_reg(T2, T2, T1))
        asm.word(A.br(T2))
        self.stats.inlined += 1
        self.stats.kinds[mnemonic.lower()] += 1
        return False

    def _pc_relative(self, offset: int, word: int, page: bool) -> bool:
        immlo = (word >> 29) & 3
        immhi = (word >> 5) & 0x7ffff
        imm = sext((immhi << 2) | immlo, 21)
        target = ((offset & ~0xfff) + (imm << 12)) if page else offset + imm
        rd = word & 31
        guest = self._resolve(rd, False)
        if guest == "zr":
            pass
        elif guest in VIRTUAL:
            self._materialize(T0, target & 0xffffffffffffffff)
            self._store_guest(T0, guest)
        else:
            self._materialize(rd, target & 0xffffffffffffffff)
        self.stats.inlined += 1
        self.stats.kinds["adrp" if page else "adr"] += 1
        return True

    # --- memory --------------------------------------------------------------
    def _memory(self, offset: int, word: int, encoding) -> bool:
        cls = encoding.decode_class
        asm = self.asm
        pair = cls in PAIR_CLASSES
        literal = None
        regoff = None
        wb_mode = "none"
        base = None
        imm = 0
        rt2 = 0
        if cls == "loadlit":
            opc, vector = word >> 30, (word >> 26) & 1
            rt = word & 31
            if not vector and opc == 3:
                return self._nop_prefetch(encoding)
            literal = offset + sext((word >> 5) & 0x7ffff, 19) * 4
            size, mem_opc = {(0, 0): (2, 1), (0, 1): (3, 1), (0, 2): (2, 2),
                             (1, 0): (2, 1), (1, 1): (3, 1),
                             (1, 2): (0, 3)}[(vector, opc)]
            n = 16 if (vector and opc == 2) else 1 << size
            load = True
        elif pair:
            opc, vector = word >> 30, (word >> 26) & 1
            load = bool((word >> 22) & 1)
            imm7 = sext((word >> 15) & 0x7f, 7)
            rt2, base, rt = (word >> 10) & 31, (word >> 5) & 31, word & 31
            if vector:
                if opc == 3:
                    return self._step(offset, "pair-reserved")
                n = {0: 4, 1: 8, 2: 16}[opc]
            else:
                if opc == 3:
                    return self._step(offset, "pair-reserved")
                n = {0: 4, 1: 4, 2: 8}[opc]
            imm = imm7 * n
            wb_mode = {"ldstpair_off": "none", "ldstnapair_offs": "none",
                       "ldstpair_pre": "pre", "ldstpair_post": "post"}[cls]
        else:
            size = word >> 30
            vector = (word >> 26) & 1
            mem_opc = (word >> 22) & 3
            base, rt = (word >> 5) & 31, word & 31
            if not vector and size == 3 and mem_opc == 2:
                return self._nop_prefetch(encoding)
            n = 16 if (vector and size == 0 and mem_opc >= 2) else 1 << size
            load = bool(mem_opc & 1) if vector else mem_opc != 0
            if cls == "ldst_pos":
                imm = ((word >> 10) & 0xfff) * n
            elif cls in ("ldst_unscaled", "ldst_immpre", "ldst_immpost"):
                imm = sext((word >> 12) & 0x1ff, 9)
                wb_mode = {"ldst_unscaled": "none", "ldst_immpre": "pre",
                           "ldst_immpost": "post"}[cls]
            else:  # ldst_regoff
                regoff = ((word >> 16) & 31, (word >> 13) & 7,
                          (word >> 12) & 1)
        total = 2 * n if pair else n
        base_g = None if literal is not None else self._resolve(base, True)
        if vector:
            operands = []
        else:
            operands = [self._resolve(rt, False)]
            if pair:
                operands.append(self._resolve(rt2, False))
        if pair and load and not vector and rt == rt2 and rt != 31:
            return self._step(offset, "pair-same-register")
        if wb_mode != "none" and any(o == base_g for o in operands):
            return self._step(offset, "writeback-overlap")
        base_virtual = base_g in VIRTUAL
        virtual_operands = [o for o in operands if o in VIRTUAL]
        if pair and base_virtual and virtual_operands and wb_mode != "none":
            return self._step(offset, "pair-virtual-writeback")
        if pair and len(virtual_operands) == 2 and virtual_operands[0] == \
                virtual_operands[1]:
            return self._step(offset, "pair-same-virtual")
        step = self._step_label(offset)
        # ---- the effective address, in a host register
        if literal is not None:
            self._materialize(T0, literal)
            ea = T0
        elif regoff is not None:
            rm, option, s_bit = regoff
            if option not in (2, 3, 6, 7):
                return self._step(offset, "regoff-option")
            idx_g = self._resolve(rm, False)
            amount = (4 if n == 16 else size) if s_bit else 0
            if base_virtual:
                self._load_guest(T0, base_g)
                base_host = T0
            else:
                base_host = base
            if idx_g in VIRTUAL:
                self._load_guest(T1, idx_g)
                idx_host = T1
            else:
                idx_host = rm
            asm.word(A.add_ext(T0, base_host, idx_host, option, amount))
            ea = T0
        else:
            if base_virtual:
                self._load_guest(T0, base_g)
                host_base = T0
            else:
                host_base = base
            effective = 0 if wb_mode == "post" else imm
            if effective == 0:
                ea = host_base
            else:
                self._add_immediate(T0, host_base, effective)
                ea = T0
        # ---- the TLB sequence: first byte's page must be the entry's page.
        # The engine keys its entries by the address without its top byte
        # (TBI is on for every Android process, and the executor enters AOT
        # code only then), so the probe and the access use that address.
        if literal is None:
            asm.word(A.ubfx(T0, ea, 0, 56))
            ea = T0
        entry = 0 if load else 16
        # A miss asks the host to fill the entry and probes again; the
        # instruction steps only when the host cannot (a fault).
        self.miss_sites[offset] = "LOAD" if load else "STORE"
        asm.label(self.lab("MR", offset))
        asm.word(A.ubfx(T2, ea, 12, 14))
        asm.word(A.add_reg(T2, TLB, T2, shift=0, amount=5))
        asm.word(A.ldp_x(T1, T2, T2, entry))
        asm.word(A.eor_reg(T1, T1, ea, shift=1, amount=12))
        asm.cbz(T1, self.lab("MM", offset), sf=1, nz=True)
        if total > 1:
            asm.word(A.add_imm(T1, ea, total - 1))
            asm.word(A.eor_reg(T1, T1, ea))
            asm.tbz(T1, 12, step, nz=True)
        # ---- the access, through the entry's host delta
        if pair:
            asm.word(A.add_reg(T2, T2, ea))
            keep = word & ((3 << 30) | (1 << 26) | (1 << 22))
            hosts = []
            if vector:
                hosts = [(rt, None), (rt2, None)]
            else:
                scratch = [T0, T1]
                for guest, field in zip(operands, (rt, rt2)):
                    if guest in VIRTUAL:
                        hosts.append((scratch.pop(0), guest))
                    elif guest == "zr":
                        hosts.append((31, None))
                    else:
                        hosts.append((field, None))
                if not load:
                    for host, guest in hosts:
                        if guest is not None:
                            self._load_guest(host, guest)
            asm.word(keep | (0b101 << 27) | (0b010 << 23) |
                     (hosts[1][0] << 10) | (T2 << 5) | hosts[0][0])
            if load:
                for host, guest in hosts:
                    if guest is not None:
                        self._store_guest(host, guest)
        else:
            host_rt = rt
            guest_rt = None if vector else operands[0]
            if guest_rt in VIRTUAL:
                host_rt = T1
                if not load:
                    self._load_guest(T1, guest_rt)
            elif guest_rt == "zr":
                host_rt = 31
            asm.word((size << 30) | (0b111 << 27) | (vector << 26) |
                     (mem_opc << 22) | (1 << 21) | (ea << 16) |
                     (0b011 << 13) | (0b10 << 10) | (T2 << 5) | host_rt)
            if guest_rt in VIRTUAL and load:
                self._store_guest(T1, guest_rt)
        # ---- writeback of the base register, after the access succeeded
        if wb_mode != "none":
            if base_virtual:
                # T0 holds the address without its tag: the base register
                # keeps its own.
                self._load_guest(T0, base_g)
                self._add_immediate(T0, T0, imm)
                self._store_guest(T0, base_g)
            else:
                self._add_immediate(base, base, imm)
        self.stats.inlined += 1
        self.stats.kinds["pair" if pair else "mem"] += 1
        return True

    def _nop_prefetch(self, encoding) -> bool:
        self.stats.inlined += 1
        self.stats.kinds["prefetch"] += 1
        return True

    def _add_immediate(self, rd: int, rn: int, imm: int) -> None:
        asm = self.asm
        sub = imm < 0
        magnitude = -imm if sub else imm
        low, high = magnitude & 0xfff, (magnitude >> 12) & 0xfff
        if magnitude >> 24:
            raise A.AsmError("offset too large")
        source = rn
        if low or not high:
            asm.word(A.add_imm(rd, source, low, sub=sub))
            source = rd
        if high:
            asm.word(A.add_imm(rd, source, high, sub=sub, shift12=True))

    # --- cold stubs --------------------------------------------------------------
    def _stub(self, label: str, offset: int, exit_label: str) -> None:
        asm = self.asm
        asm.label(label)
        asm.word(A.movz(T0, offset & 0xffff))
        if offset >> 16:
            asm.word(A.movk(T0, (offset >> 16) & 0xffff, 1))
        asm.b(exit_label)

    def _emit_stubs(self) -> None:
        for offset in sorted(self.miss_sites):
            asm = self.asm
            asm.label(self.lab("MM", offset))
            asm.adrp_add(T1, self.lab("MF", offset))
            asm.b("REFILL_" + self.miss_sites[offset])
            asm.label(self.lab("MF", offset))
            asm.b(self.lab("XS", offset))
            asm.b(self.lab("MR", offset))
        for offset in sorted(self.step_sites):
            self._stub(self.lab("XS", offset), offset, "EXIT_STEP")
        for offset in sorted(self.budget_sites):
            self._stub(self.lab("XU", offset), offset, "EXIT_BUDGET")
        for target in sorted(self.exit_targets):
            self._stub(self.lab("XB", target), target, "EXIT_BRANCH")


# ---- the entry and exit routines -----------------------------------------

def emit_runtime(asm: A.Assembler, layout: dict[str, int]) -> None:
    L = layout
    cpu_regs = L["UML_GADGET_CPU_X0"]
    v0 = L["UML_GADGET_CPU_V0"]
    asm.label("ENTER")
    asm.word(A.add_imm(31, 31, FRAME, sub=True))
    asm.word(A.stp_x(29, 30, 31, 0))
    for index, pair in enumerate(((19, 20), (21, 22), (23, 24), (25, 26),
                                  (27, 28))):
        asm.word(A.stp_x(pair[0], pair[1], 31, 16 + 16 * index))
    for index in range(4):
        asm.word(A.ldp_d(8 + 2 * index, 9 + 2 * index, 31, 0x60 + 16 * index,
                         load=False))
    asm.word(A.str_x(0, 31, SLOT_CPU))
    asm.word(A.str_x(2, 31, SLOT_BIAS))
    asm.word(A.str_x(4, 31, SLOT_BUD0))
    asm.word(A.str_x(5, 31, SLOT_REFILL))
    asm.word(A.mov_reg(TLB, 1))
    asm.word(A.mov_reg(BUD, 4))
    asm.word(A.mov_reg(T0, 3))
    asm.word(A.mov_reg(16, 0))
    # activate the guest floating-point environment when it is not live
    fp_active = L["UML_GADGET_CPU_FP_ACTIVE"]
    asm.word(A.ldrb_w(9, 16, fp_active))
    asm.cbz(9, "ENTER_FP", sf=0, nz=False)
    asm.b("ENTER_FP_DONE")
    asm.label("ENTER_FP")
    asm.word(A.mrs(10, "fpcr"))
    asm.word(A.mrs(11, "fpsr"))
    asm.word(A.str_x(10, 16, L["UML_GADGET_CPU_HOST_FPCR"]))
    asm.word(A.str_x(11, 16, L["UML_GADGET_CPU_HOST_FPSR"]))
    asm.word(A.movz(9, 1, sf=0))
    asm.word(A.strb_w(9, 16, fp_active))
    asm.word(A.ldr_w(9, 16, L["UML_GADGET_CPU_FPCR"]))
    asm.word(A.movz(10, L["UML_ARM64_FPCR_TRAP_ENABLES"], sf=0))
    asm.word(0x2a0a0129)  # orr w9, w9, w10
    asm.word(0x4a0a0129)  # eor w9, w9, w10
    asm.word(A.msr("fpcr", 9))
    asm.word(A.ldr_w(9, 16, L["UML_GADGET_CPU_FPSR"]))
    asm.word(A.msr("fpsr", 9))
    asm.label("ENTER_FP_DONE")
    for index in range(16):
        asm.word(A.ldp_q(2 * index, 2 * index + 1, 16, v0 + 32 * index))
    asm.word(A.ldr_w(9, 16, L["UML_GADGET_CPU_NZCV"]))
    asm.word(A.msr("nzcv", 9))
    for guest, slot in VSLOT.items():
        asm.word(A.ldr_x(9, 16, cpu_regs + 8 * guest))
        asm.word(A.str_x(9, 31, slot))
    asm.word(A.ldr_x(9, 16, L["UML_GADGET_CPU_SP"]))
    asm.word(A.str_x(9, 31, SLOT_SP))
    skip = {16, 18, 24, 25, 26, 27, 28}
    index = 0
    while index <= 30:
        if index in skip:
            index += 1
            continue
        if index + 1 <= 30 and (index + 1) not in skip and index % 1 == 0:
            asm.word(A.ldp_x(index, index + 1, 16, cpu_regs + 8 * index))
            index += 2
        else:
            asm.word(A.ldr_x(index, 16, cpu_regs + 8 * index))
            index += 1
    asm.word(A.ldr_x(16, 16, cpu_regs + 8 * 16))
    asm.word(A.br(T0))

    for name, reason in (("EXIT_BUDGET", EXIT_BUDGET),
                         ("EXIT_BRANCH", EXIT_BRANCH),
                         ("EXIT_STEP", EXIT_STEP),
                         ("EXIT_BRANCH_ABS", EXIT_BRANCH | ABSOLUTE)):
        asm.label(name)
        asm.word(A.movz(T1, reason))
        asm.b("EXIT_COMMON")
    asm.label("EXIT_COMMON")
    asm.word(A.ldr_x(T2, 31, SLOT_CPU))
    index = 0
    while index <= 30:
        if index in (18, 24, 25, 26, 27, 28):
            index += 1
            continue
        if index + 1 <= 30 and (index + 1) not in (18, 24, 25, 26, 27, 28):
            asm.word(A.stp_x(index, index + 1, T2, cpu_regs + 8 * index))
            index += 2
        else:
            asm.word(A.str_x(index, T2, cpu_regs + 8 * index))
            index += 1
    for guest, slot in VSLOT.items():
        asm.word(A.ldr_x(TLB, 31, slot))
        asm.word(A.str_x(TLB, T2, cpu_regs + 8 * guest))
    asm.word(A.ldr_x(TLB, 31, SLOT_SP))
    asm.word(A.str_x(TLB, T2, L["UML_GADGET_CPU_SP"]))
    asm.word(A.mrs(TLB, "nzcv"))
    asm.word(A.str_w(TLB, T2, L["UML_GADGET_CPU_NZCV"]))
    asm.word(A.ldr_x(TLB, 31, SLOT_BIAS))
    asm.tbz(T1, 8, "EXIT_REL", nz=False)
    asm.b("EXIT_PC")
    asm.label("EXIT_REL")
    asm.word(A.add_reg(T0, T0, TLB))
    asm.label("EXIT_PC")
    asm.word(A.str_x(T0, T2, L["UML_GADGET_CPU_PC"]))
    for index in range(16):
        asm.word(A.ldp_q(2 * index, 2 * index + 1, T2, v0 + 32 * index,
                         load=False))
    asm.word(A.ubfx(0, T1, 0, 8))
    asm.word(A.ldr_x(TLB, 31, SLOT_BUD0))
    asm.word(A.add_reg(1, TLB, BUD, sub=True))
    for index in range(4):
        asm.word(A.ldp_d(8 + 2 * index, 9 + 2 * index, 31, 0x60 + 16 * index))
    asm.word(A.ldp_x(29, 30, 31, 0))
    for index, pair in enumerate(((19, 20), (21, 22), (23, 24), (25, 26),
                                  (27, 28))):
        asm.word(A.ldp_x(pair[0], pair[1], 31, 16 + 16 * index))
    asm.word(A.add_imm(31, 31, FRAME))
    asm.word(A.ret_())

    # The refill thunk. In: T0 = the address without its tag, T1 = the
    # landing for a failed refill (the word after it is the landing for a
    # successful one). It saves every register a C++ call may change, asks
    # the host to fill the TLB entry for T0, restores, and branches.
    for kind, access in (("LOAD", 0), ("STORE", 1)):
        asm.label("REFILL_" + kind)
        asm.word(A.movz(T2, access))
        if kind == "LOAD":
            asm.b("REFILL")
    asm.label("REFILL")
    rf = REFILL_FRAME
    asm.word(A.add_imm(31, 31, rf, sub=True))
    for index in range(0, 18, 2):
        asm.word(A.stp_x(index, index + 1, 31, 8 * index))
    asm.word(A.str_x(30, 31, 0x90))
    asm.word(A.mrs(9, "nzcv"))
    asm.word(A.str_x(9, 31, 0x98))
    for index in range(16):
        asm.word(A.ldp_q(2 * index, 2 * index + 1, 31, 0xa0 + 32 * index,
                         load=False))
    asm.word(A.ldr_x(9, 31, rf + SLOT_REFILL))
    asm.word(A.mov_reg(0, TLB))
    asm.word(A.mov_reg(1, T0))
    asm.word(A.mov_reg(2, T2))
    asm.word(A.blr(9))
    asm.word(A.mov_reg(T2, 0))
    for index in range(16):
        asm.word(A.ldp_q(2 * index, 2 * index + 1, 31, 0xa0 + 32 * index))
    asm.word(A.ldr_x(9, 31, 0x98))
    asm.word(A.msr("nzcv", 9))
    asm.word(A.ldr_x(30, 31, 0x90))
    for index in range(0, 18, 2):
        asm.word(A.ldp_x(index, index + 1, 31, 8 * index))
    asm.word(A.add_imm(31, 31, rf))
    asm.cbz(T2, "REFILL_FAIL", sf=1, nz=False)
    asm.word(A.add_imm(T1, T1, 4))
    asm.label("REFILL_FAIL")
    asm.word(A.br(T1))


# ---- the blob ----------------------------------------------------------------

def build_blob(libraries: list[Library], layout: dict[str, int],
               release_id: str) -> tuple[bytes, dict]:
    asm = A.Assembler()
    asm.label("BLOB")
    asm.data(bytes(HEADER_SIZE + LIB_RECORD_SIZE * len(libraries)))
    asm.align(64)
    emit_runtime(asm, layout)
    for library in libraries:
        library.emit(asm)
    asm.align(64)
    # tables are filled after layout; reserve their space now
    for library in libraries:
        asm.label(f"L{library.index}TAB0")
        asm.data(bytes(4 * library.table_words))
        asm.align(64)
    # per-library digest arrays
    digests_by_library: list[list[int]] = []
    for library in libraries:
        segment = library.segment
        first = library.table_base
        pages = []
        for page_vaddr in range(first, library.table_end, PAGE):
            file_offset = segment.offset + (page_vaddr - segment.vaddr)
            page = bytes(library.elf.data[file_offset:file_offset + PAGE])
            page += bytes(PAGE - len(page))
            pages.append(page_digest(page))
        digests_by_library.append(pages)
        asm.label(f"L{library.index}DIG")
        asm.data(bytes(8 * len(pages)))
        asm.align(64)
    slots = 16
    total_pages = sum(len(p) for p in digests_by_library)
    while slots < 2 * total_pages:
        slots *= 2
    asm.label("HASH")
    asm.data(bytes(16 * slots))
    asm.align(PAGE)
    asm.label("END")
    blob = bytearray(asm.resolve())
    # fill the tables
    names = {}
    for library in libraries:
        table_at = asm.label_address(f"L{library.index}TAB0")
        for offset in library.entry_labels:
            native = asm.label_address(library.lab("E", offset))
            index = (offset - library.table_base) // 4
            struct.pack_into("<I", blob, table_at + 4 * index, native)
        digest_at = asm.label_address(f"L{library.index}DIG")
        for page_index, digest in enumerate(digests_by_library[library.index]):
            struct.pack_into("<Q", blob, digest_at + 8 * page_index, digest)
    # the global page-digest hash
    hash_at = asm.label_address("HASH")
    occupancy: dict[int, int] = collections.Counter()
    for pages in digests_by_library:
        occupancy.update(pages)
    for library in libraries:
        for page_index, digest in enumerate(digests_by_library[library.index]):
            if occupancy[digest] != 1:
                continue
            slot = digest & (slots - 1)
            while struct.unpack_from("<Q", blob, hash_at + 16 * slot)[0]:
                slot = (slot + 1) & (slots - 1)
            struct.pack_into("<QII", blob, hash_at + 16 * slot, digest,
                             library.index, page_index)
    # the header and the library records
    record_at = HEADER_SIZE
    struct.pack_into("<8sIIIIIII", blob, 0, ENTRY_MAGIC, 1, HEADER_SIZE,
                     len(libraries), record_at,
                     asm.label_address("ENTER"), hash_at, slots)
    release = release_id.encode()[:96]
    struct.pack_into("96s", blob, 0x30, release)
    for library in libraries:
        elf = library.elf
        name = library.name.encode()
        if len(name) > 95:
            raise ValueError("library path too long")
        base = record_at + LIB_RECORD_SIZE * library.index
        struct.pack_into("96s", blob, base, name)
        struct.pack_into("32s", blob, base + 96, bytes.fromhex(elf.sha256))
        struct.pack_into("24s", blob, base + 128, elf.build_id[:24])
        struct.pack_into(
            "<QQIIIIQII", blob, base + 152, library.table_base,
            library.table_end, asm.label_address(f"L{library.index}TAB0"),
            library.table_words, asm.label_address(f"L{library.index}DIG"),
            len(digests_by_library[library.index]),
            library.segment.vaddr + library.segment.filesz,
            library.stats.instructions, library.stats.steps)
    manifest = {
        "release_id": release_id,
        "blob_bytes": len(blob),
        "libraries": [{
            "path": library.name,
            "elf_sha256": library.elf.sha256,
            "build_id": library.elf.build_id.hex(),
            "executable_segment": {
                "vaddr": library.segment.vaddr,
                "filesz": library.segment.filesz},
            "functions": library.stats.functions,
            "instructions": library.stats.instructions,
            "inlined": library.stats.inlined,
            "steps": library.stats.steps,
            "steps_by_cause": dict(library.stats.steps_by_cause),
            "inlined_by_kind": dict(library.stats.kinds),
            "log_span": library.log_span,
        } for library in libraries],
    }
    return bytes(blob), manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, required=True,
                        help="the unpacked partition holding the libraries")
    parser.add_argument("--library", action="append", required=True,
                        help="a library path inside --root, e.g. "
                        "/system/lib64/libz.so; repeatable")
    parser.add_argument("--release-id", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True,
                        help="output directory (blob.bin, manifest.json)")
    parser.add_argument("--decode-table", type=pathlib.Path,
                        default=a64decode.DEFAULT_TABLE)
    arguments = parser.parse_args(argv)
    decoder = a64decode.Decoder(arguments.decode_table)
    layout = read_layout(LAYOUT_HEADER)
    libraries = []
    for index, path in enumerate(arguments.library):
        data = (arguments.root / path.lstrip("/")).read_bytes()
        libraries.append(Library(index, path, elf64.Elf(data), decoder,
                                 layout))
    blob, manifest = build_blob(libraries, layout, arguments.release_id)
    arguments.out.mkdir(parents=True, exist_ok=True)
    (arguments.out / "blob.bin").write_bytes(blob)
    (arguments.out / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    # The functions the blob covers, for the tests that walk them.
    with open(arguments.out / "functions.txt", "w") as handle:
        for library in libraries:
            for function in library.functions:
                handle.write(f"{library.name} {function.start:#x} "
                             f"{function.end:#x}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
