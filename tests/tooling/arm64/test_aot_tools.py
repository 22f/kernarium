#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Known answers for the AOT rewriter's encoders, decoder and assembler.

The words below were produced by the system assembler for the text beside
each; the tool must produce the same bits. The rewriter's whole output is
checked against the engine by the arm64-aot-libz differential test.
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


if __name__ == "__main__":
    unittest.main()
