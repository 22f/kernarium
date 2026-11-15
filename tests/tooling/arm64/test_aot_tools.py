#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Known answers for the AOT rewriter's encoders, decoder and assembler.

The words below were produced by the system assembler for the text beside
each; the tool must produce the same bits. The rewriter's whole output is
checked against the engine by the arm64-aot-differential differential test.
"""

__import__("sys").dont_write_bytecode = True

import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3] / "tools" /
                       "aot"))

import a64decode  # noqa: E402
import asm as A  # noqa: E402
import elf64  # noqa: E402
import rewrite  # noqa: E402

ENCODERS = [
    ("add x0, x1, #1", 0x91000420, lambda: A.add_imm(0, 1, 1)),
    ("sub sp, sp, #240", 0xd103c3ff,
     lambda: A.add_imm(31, 31, 240, sub=True)),
    ("movz x9, #0x1234", 0xd2824689, lambda: A.movz(9, 0x1234)),
    ("movk x9, #0x5678, lsl #16", 0xf2aacf09,
     lambda: A.movk(9, 0x5678, 1)),
    ("ubfx x26, x3, #12, #14", 0xd34c647a, lambda: A.ubfx(26, 3, 12, 14)),
    ("lsr x1, x2, #7", 0xd347fc41, lambda: A.lsr_imm(1, 2, 7)),
    ("add x28, x25, x28, lsl #5", 0x8b1c173c,
     lambda: A.add_reg(28, 25, 28, shift=0, amount=5)),
    ("eor x27, x27, x3, lsr #12", 0xca43337b,
     lambda: A.eor_reg(27, 27, 3, shift=1, amount=12)),
    ("mov x24, x4", 0xaa0403f8, lambda: A.mov_reg(24, 4)),
    ("ldr x9, [sp, #168]", 0xf94057e9, lambda: A.ldr_x(9, 31, 168)),
    ("str x2, [sp, #176]", 0xf9005be2, lambda: A.str_x(2, 31, 176)),
    ("ldr w9, [x16, #8]", 0xb9400a09, lambda: A.ldr_w(9, 16, 8)),
    ("ldrb w9, [x16, #904]", 0x394e2209, lambda: A.ldrb_w(9, 16, 904)),
    ("ldp x27, x28, [x28]", 0xa940739b, lambda: A.ldp_x(27, 28, 28, 0)),
    ("stp x29, x30, [sp]", 0xa9007bfd, lambda: A.stp_x(29, 30, 31, 0)),
    ("ldp q0, q1, [x16, #32]", 0xad410600,
     lambda: A.ldp_q(0, 1, 16, 32)),
    ("stp q2, q3, [x28, #64]", 0xad020f82,
     lambda: A.stp_q(2, 3, 28, 64)),
    ("mrs x9, nzcv", 0xd53b4209, lambda: A.mrs(9, "nzcv")),
    ("msr nzcv, x9", 0xd51b4209, lambda: A.msr("nzcv", 9)),
    ("mrs x10, fpcr", 0xd53b440a, lambda: A.mrs(10, "fpcr")),
    ("ret", 0xd65f03c0, lambda: A.ret_()),
    ("br x26", 0xd61f0340, lambda: A.br(26)),
    ("nop", 0xd503201f, lambda: A.nop()),
]

DECODED = [
    (0xd65f03c0, "RET", "branch_reg"),
    (0x91000420, "ADD", "addsub_imm"),
    (0xa940739b, "LDP", "ldstpair_off"),
    (0xd53b4209, "MRS", "systemmove"),
    (0xd4000001, "SVC", "exception"),
    (0xc85f7c20, "LDXR", "ldstexclr"),
    (0x1e202800, "FADD", "floatdp2"),
    (0x4e20842a, "ADD", "asimdsame"),
    (0x00000000, "UDF", "perm_undef"),
]


class EncoderTests(unittest.TestCase):
    def test_known_words(self) -> None:
        for text, word, build in ENCODERS:
            with self.subTest(text):
                self.assertEqual(build(), word, f"{build():#010x} for {text}")


class DecoderTests(unittest.TestCase):
    def test_identification(self) -> None:
        decoder = a64decode.Decoder()
        for word, mnemonic, decode_class in DECODED:
            with self.subTest(f"{word:#010x}"):
                encoding = decoder.decode(word)
                self.assertIsNotNone(encoding)
                self.assertEqual(encoding.mnemonic, mnemonic)
                self.assertEqual(encoding.decode_class, decode_class)

    def test_unallocated_words_are_unknown_to_the_table(self) -> None:
        decoder = a64decode.Decoder()
        # Reserved encoding space: not an instruction of any feature.
        self.assertIsNone(decoder.decode(0x00ff00ff))


class AssemblerTests(unittest.TestCase):
    def test_far_test_and_branch_is_relaxed(self) -> None:
        assembler = A.Assembler()
        assembler.tbz(1, 3, "far", nz=True)
        assembler.data(bytes(40000))
        assembler.label("far")
        assembler.word(A.nop())
        blob = assembler.resolve()
        first, second = struct.unpack_from("<2I", blob, 0)
        # tbnz is out of range (32 KiB): tbz over a following b.
        self.assertEqual(first, 0x36180041)
        self.assertEqual(second >> 26, 0b000101)
        target = assembler.label_address("far")
        delta = (second & 0x03ffffff) * 4
        self.assertEqual(4 + delta, target)

    def test_near_branch_is_not_relaxed(self) -> None:
        assembler = A.Assembler()
        assembler.cbz(5, "next", nz=True)
        assembler.label("next")
        assembler.word(A.nop())
        blob = assembler.resolve()
        self.assertEqual(len(blob), 8)
        (word,) = struct.unpack_from("<I", blob, 0)
        self.assertEqual(word, 0xb5000025)


class FakeElf:
    """The parts of an ELF library the rewriter reads: one code segment cut
    into the functions an unwind table would name."""

    def __init__(self, words: list[int], functions: list[tuple[int, int]],
                 base: int = 0x1000) -> None:
        self.base = base
        self.code = struct.pack(f"<{len(words)}I", *words)
        self.data = self.code
        self._functions = [elf64.Function(base + a, base + b)
                           for a, b in functions]
        self.sha256 = "00" * 32
        self.build_id = b"\0" * 16

    def executable_segment(self) -> elf64.Segment:
        return elf64.Segment(self.base, 0, len(self.code), len(self.code), 5)

    def functions(self) -> list[elf64.Function]:
        return self._functions

    def read_vaddr(self, vaddr: int, size: int) -> bytes:
        return self.code[vaddr - self.base:vaddr - self.base + size]


def library_of(words: list[int], functions: list[tuple[int, int]]
               ) -> "rewrite.Library":
    layout = rewrite.read_layout(rewrite.LAYOUT_HEADER)
    return rewrite.Library(0, "/test/lib.so", FakeElf(words, functions),
                           a64decode.Decoder(), layout)


def unit_words(library: "rewrite.Library") -> list[int]:
    digests = [1] * ((library.table_end - library.table_base) // 4096)
    blob = library.build(digests)["unit"]
    return list(struct.unpack(f"<{len(blob) // 4}I", blob))


NOP = 0xd503201f
RET = 0xd65f03c0


class AssemblerFarBranchTests(unittest.TestCase):
    def test_far_always_branch_is_a_branch(self) -> None:
        # b.al has no inverse condition: the relaxed form is a branch.
        assembler = A.Assembler()
        assembler.bcond(14, "far")
        assembler.data(bytes(4 * 300000))
        assembler.label("far")
        assembler.word(A.nop())
        blob = assembler.resolve()
        first, second = struct.unpack_from("<2I", blob, 0)
        self.assertEqual(first >> 26, 0b000101)
        self.assertEqual(second, A.nop())
        self.assertEqual(4 * (first & 0x03ffffff), assembler.label_address("far"))

    def test_far_conditional_branch_is_inverted_over_a_branch(self) -> None:
        assembler = A.Assembler()
        assembler.bcond(0, "far")  # b.eq
        assembler.data(bytes(4 * 300000))
        assembler.label("far")
        assembler.word(A.nop())
        blob = assembler.resolve()
        first, second = struct.unpack_from("<2I", blob, 0)
        self.assertEqual(first & 15, 1)  # b.ne over the branch
        self.assertEqual(second >> 26, 0b000101)

    def test_an_unconditional_branch_out_of_reach_is_refused(self) -> None:
        # +-128 MiB: a unit larger than that cannot be assembled, and says so.
        with self.assertRaises(A.AsmError):
            A.b_word(1 << 25)


class SimdRegisterFieldTests(unittest.TestCase):
    """A vector field must never be treated as a general register (the
    register substitution applies only to x18, x24-x28 and sp)."""

    REGISTERS = (0, 5, 18, 24, 25, 26, 27, 28, 31)
    decoder = a64decode.Decoder()

    def fields(self, word: int):
        encoding = self.decoder.decode(word)
        self.assertIsNotNone(encoding, hex(word))
        return encoding, rewrite.Library._copy_fields(None, word, encoding)

    def test_ins_element_names_no_general_register(self) -> None:
        for imm5 in (1, 2, 4, 8, 16, 3, 6, 12, 24):
            for imm4 in range(16):
                for rn in self.REGISTERS:
                    for rd in self.REGISTERS:
                        word = (0x6e000400 | imm5 << 16 | imm4 << 11 |
                                rn << 5 | rd)
                        encoding = self.decoder.decode(word)
                        if encoding is None or \
                                encoding.decode_class != "asimdins":
                            continue
                        fields = rewrite.Library._copy_fields(
                            None, word, encoding)
                        self.assertEqual(fields, [], hex(word))

    def test_general_register_forms_still_substitute(self) -> None:
        # INS (general), DUP (general): Rn is a general register.
        for rn in (18, 24, 31):
            ins = 0x4e001c00 | 8 << 16 | rn << 5 | 3
            self.assertEqual(self.fields(ins)[1], [(5, False)])
            dup = 0x4e000c00 | 8 << 16 | rn << 5 | 3
            self.assertEqual(self.fields(dup)[1], [(5, False)])
        # UMOV: Rd is a general register.
        for rd in (18, 28):
            umov = 0x4e003c00 | 8 << 16 | 4 << 5 | rd
            self.assertEqual(self.fields(umov)[1], [(0, False)])


class BudgetTests(unittest.TestCase):
    def test_every_backward_branch_target_is_charged(self) -> None:
        # Function B (0x1010..0x1020) branches back into the interior of
        # function A (0x1000..0x1010): a cycle through two unwind ranges.
        # b #-0xc from 0x1018 is the branch to 0x100c.
        b_back = 0x14000000 | ((-3) & 0x3ffffff)
        words = [NOP, NOP, NOP, NOP,          # A: 0x1000-0x100c
                 NOP, NOP, b_back, RET]       # B: 0x1010-0x101c
        library = library_of(words, [(0, 0x10), (0x10, 0x20)])
        self.assertIn(0x100c, library.budget_extent)
        self.assertIn(0x1000, library.budget_extent)
        self.assertIn(0x1010, library.budget_extent)

    def test_a_self_loop_is_charged(self) -> None:
        words = [NOP, 0x14000000, RET]  # b . at 0x1004
        library = library_of(words, [(0, 0xc)])
        self.assertIn(0x1004, library.budget_extent)


class StackAlignmentTests(unittest.TestCase):
    """AOT code runs with a 16-byte aligned SP, as the engine checks every
    SP-based access: an instruction that can leave SP misaligned is followed
    by a test that ends the entry."""

    GUARD = A.ubfx(27, 26, 0, 4)  # ubfx x27, x26, #0, #4 (T1 = T0 & 15)

    def test_mov_to_sp_is_checked(self) -> None:
        mov_sp = 0x9100001f | 1 << 5  # mov sp, x1 (add sp, x1, #0)
        unit = unit_words(library_of([mov_sp, RET], [(0, 8)]))
        self.assertIn(self.GUARD, unit)

    def test_aligned_adjustment_of_sp_is_not_checked(self) -> None:
        sub_sp = 0xd10043ff  # sub sp, sp, #16
        unit = unit_words(library_of([sub_sp, RET], [(0, 8)]))
        self.assertNotIn(self.GUARD, unit)

    def test_odd_adjustment_of_sp_is_checked(self) -> None:
        sub_sp = 0xd10023ff  # sub sp, sp, #8
        unit = unit_words(library_of([sub_sp, RET], [(0, 8)]))
        self.assertIn(self.GUARD, unit)

    def test_sp_stays_unchecked_for_a_plain_load(self) -> None:
        ldr = 0xf94003e0  # ldr x0, [sp]
        unit = unit_words(library_of([ldr, RET], [(0, 8)]))
        self.assertNotIn(self.GUARD, unit)


if __name__ == "__main__":
    unittest.main()
