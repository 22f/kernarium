#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Known answers for the AOT rewriter's encoders, decoder and assembler.

The words below were produced by the system assembler for the text beside
each; the tool must produce the same bits. The rewriter's whole output is
checked against the engine by the arm64-aot-differential differential test.
"""

__import__("sys").dont_write_bytecode = True

import hashlib
import pathlib
import struct
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3] / "tools" /
                       "aot"))

import a64decode  # noqa: E402
import asm as A  # noqa: E402
import elf64  # noqa: E402
import fixture  # noqa: E402
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


def library_of(words: list[int], functions: list[tuple[int, int]],
               memory_call: str = "inline") -> "rewrite.Library":
    layout = rewrite.read_layout(rewrite.LAYOUT_HEADER)
    return rewrite.Library(0, "/test/lib.so", FakeElf(words, functions),
                           a64decode.Decoder(), layout,
                           memory_call=memory_call)


def unit_words(library: "rewrite.Library") -> list[int]:
    digests = [1] * ((library.table_end - library.table_base) // 4096)
    blob = library.build(digests)["unit"]
    return list(struct.unpack(f"<{len(blob) // 4}I", blob))


NOP = 0xd503201f
RET = 0xd65f03c0


class AssemblerFarBranchTests(unittest.TestCase):
    def test_far_never_branch_is_a_branch_too(self) -> None:
        # B.NV (condition 15) is taken like B.AL: AArch64 treats both as
        # always, so neither may be relaxed to a no-op.
        assembler = A.Assembler()
        assembler.bcond(15, "far")
        assembler.data(bytes(4 * 300000))
        assembler.label("far")
        assembler.word(A.nop())
        blob = assembler.resolve()
        first, second = struct.unpack_from("<2I", blob, 0)
        self.assertEqual(first >> 26, 0b000101)
        self.assertEqual(4 * (first & 0x03ffffff), assembler.label_address("far"))

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
        with self.assertRaises(A.AsmError) as raised:
            A.b_word(1 << 25)
        self.assertIn("128 MiB", str(raised.exception))
        A.b_word((1 << 25) - 1)
        A.b_word(-(1 << 25))


class MemoryCallTests(unittest.TestCase):
    """A plain load or store may reach its probe by a call to a routine of
    its unit (an experiment; the inline probe stays the default)."""

    LDR = 0xf9400020      # ldr x0, [x1]
    STR = 0xf9000022      # str x2, [x1]
    RET = RET

    def build(self, mode: str, words=None, functions=None):
        words = words or [self.LDR, self.STR, self.RET]
        functions = functions or [(0, 4 * len(words))]
        library = library_of(words, functions, mode)
        unit = unit_words(library)
        return library, unit

    def calls(self, library, unit):
        """The word index of every bl in the unit and its target address."""
        found = []
        for index, word in enumerate(unit):
            if word >> 26 == 0b100101:
                delta = word & 0x3ffffff
                delta -= (delta & 0x2000000) << 1
                found.append((index, 4 * (index + delta)))
        return found

    def test_the_default_is_the_inline_probe(self) -> None:
        library, _ = self.build("inline")
        census = library.stats.census
        self.assertEqual(census["mem.call"], 0)
        self.assertEqual(census["mem.routine"], 0)
        self.assertEqual(census["mem.probe"], 2 * 8 * 4)

    def test_an_unknown_mode_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            library_of([self.RET], [(0, 4)], "everywhere")

    def test_probe_call_keeps_the_stub_and_moves_the_compare(self) -> None:
        library, unit = self.build("probe")
        census = library.stats.census
        self.assertEqual(sum(census.values()), 4 * len(unit))
        self.assertEqual(census["other"], 0)
        self.assertEqual(census["mem.probe"], 0)
        # untag, save x30, call, restore x30, branch to the stub: five words
        # a site, and the two-word stub is still there.
        self.assertEqual(census["mem.call"], 2 * 5 * 4)
        self.assertEqual(census["stub.mm"], 2 * 2 * 4)
        # the probe word for word, without the branch, and a return
        self.assertEqual(census["mem.routine"], 2 * 7 * 4)
        self.assertEqual(census["stub.refill"], 2 * 2 * 4)
        calls = self.calls(library, unit)
        self.assertEqual(len(calls), 2)
        asm = library.asm
        self.assertEqual({target for _, target in calls},
                         {asm.label_address("MEMP_LOAD_8"),
                          asm.label_address("MEMP_STORE_8")})
        for index, _ in calls:
            self.assertEqual(unit[index - 1],
                             A.str_x(30, 31, rewrite.SLOT_LR))
            self.assertEqual(unit[index + 1],
                             A.ldr_x(30, 31, rewrite.SLOT_LR))

    def test_probe_miss_call_has_no_stub_and_names_its_slot(self) -> None:
        library, unit = self.build("probe-miss")
        census = library.stats.census
        self.assertEqual(sum(census.values()), 4 * len(unit))
        self.assertEqual(census["other"], 0)
        self.assertEqual(census["mem.probe"], 0)
        self.assertEqual(census["stub.mm"], 0)
        # untag, save x30, marker, call, restore x30
        self.assertEqual(census["mem.call"], 2 * 5 * 4)
        # six probe words, the branch to the miss tail, a return, and the
        # five-word tail
        self.assertEqual(census["mem.routine"], 2 * 13 * 4)
        self.assertEqual(census["stub.refill"], 2 * 2 * 4)
        for index, target in self.calls(library, unit):
            marker = unit[index - 1]
            # the routine reads the marker at the return address less 8
            self.assertEqual((4 * (index + 1)) - 8, 4 * (index - 1))
            self.assertEqual(marker & 0x1f, 31)           # into XZR
            self.assertEqual(marker >> 23, 0b110100101)   # MOVZ, 64-bit
            self.assertEqual(unit[index - 2],
                             A.str_x(30, 31, rewrite.SLOT_LR))
        slots = sorted(((unit[i - 1] >> 5) & 0x3ffff) * 4
                       for i, _ in self.calls(library, unit))
        self.assertEqual(slots, [0, 4])

    def test_a_slot_beyond_a_mebibyte_has_its_own_routine(self) -> None:
        count = (3 << 20) // 4 + 8     # the load's slot is 3 MiB + 32
        words = [NOP] * count + [self.LDR, self.RET]
        library, unit = self.build(
            "probe-miss", words, [(4 * count, 4 * count + 8)])
        asm = library.asm
        self.assertIn("MEMC_LOAD_8_3", asm._labels)
        self.assertNotIn("MEMC_LOAD_8_0", asm._labels)
        # the tail of a later chunk forms the higher bits of the slot (a
        # movz and an orr more)
        self.assertEqual(library.stats.census["mem.routine"], 15 * 4)
        index = self.calls(library, unit)[0][0]
        slot = 4 * count
        self.assertEqual(((unit[index - 1] >> 5) & 0x3ffff) * 4,
                         slot & ((1 << 20) - 1))

    def test_a_pair_and_a_byte_load_use_their_own_routines(self) -> None:
        ldrb = 0x39400020   # ldrb w0, [x1]
        ldp = 0xa9400820    # ldp x0, x2, [x1]
        library, unit = self.build("probe", [ldrb, ldp, self.RET])
        asm = library.asm
        self.assertIn("MEMP_LOAD_1", asm._labels)
        self.assertIn("MEMP_LOAD_16", asm._labels)
        # a one-byte access has no last-byte arithmetic: six words
        # (untag at the site) and five in the routine
        self.assertEqual(library.stats.census["mem.routine"],
                         (5 + 7) * 4)


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

    def test_reviewed_ins_words_keep_their_vector_registers(self) -> None:
        # INS V0.B[0], V27.B[1] and INS V18.B[0], V0.B[5]: imm4 selects the
        # source lane, so no field names a general register.
        for word in (0x6e010f60, 0x6e012c12):
            self.assertEqual(self.fields(word)[1], [], hex(word))

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


class FixtureTests(unittest.TestCase):
    """The generated test library has the control flow the harness needs."""

    def setUp(self) -> None:
        self.elf = elf64.Elf(fixture.build())
        self.library = rewrite.Library(
            0, "/aot-test/libaotfixture.so", self.elf,
            a64decode.Decoder(a64decode.DEFAULT_TABLE),
            rewrite.read_layout(rewrite.LAYOUT_HEADER))

    def test_ranges_and_plt(self) -> None:
        self.assertEqual(
            [(f.start, f.end) for f in self.elf.functions()][:3],
            [(0x1000, 0x1010), (0x1020, 0x1030), (0x1040, 0x1050)])

    def test_loop_branch_lands_inside_another_range(self) -> None:
        # b.lo at 0x1028 (range B) targets 0x1004 (inside range A), and no
        # function entry lies on the cycle.
        kind, target = self.library._flow(fixture.CODE[0x1028], 0x1028)
        self.assertEqual((kind, target), ("cond", 0x1004))
        self.assertIn(0x1004, self.library.charge)
        self.assertNotIn(0x1008, self.library.charge)


class HintAndFloatingPointTests(unittest.TestCase):
    """What the rewriter inlines must be what the engine runs natively."""

    @staticmethod
    def steps(word: int) -> int:
        library = library_of([word, RET], [(0, 8)])
        unit_words(library)
        return library.stats.steps

    def test_hints_the_engine_runs_as_no_ops_are_inlined(self) -> None:
        # paciasp, autiasp, bti c, nop, wfe: no effect for this guest.
        for word in (0xd503233f, 0xd50323bf, 0xd503245f, 0xd503201f,
                     0xd503205f):
            self.assertEqual(self.steps(word), 0, hex(word))

    def test_hints_with_an_effect_step(self) -> None:
        # yield, dgh: gadgets with an effect in the engine.
        for word in (0xd503203f, 0xd50320df):
            self.assertEqual(self.steps(word), 1, hex(word))

    def test_single_and_double_precision_are_inlined(self) -> None:
        for word in (0x1e622820,   # fadd d0, d1, d2
                     0x1e612000,   # fcmp d0, d1
                     0x4e22d420):  # fadd v0.4s, v1.4s, v2.4s
            self.assertEqual(self.steps(word), 0, hex(word))

    def test_general_register_operands_are_substituted(self) -> None:
        decoder = a64decode.Decoder()
        for word, expected in ((0x9e620240, [(5, False)]),   # scvtf d0, x18
                               (0x9e780038, [(0, False)]),   # fcvtzs x24, d1
                               (0x9e660079, [(0, False)]),   # fmov x25, d3
                               (0x9e670343, [(5, False)])):  # fmov d3, x26
            encoding = decoder.decode(word)
            self.assertEqual(
                rewrite.Library._copy_fields(None, word, encoding),
                expected, hex(word))

    def test_what_the_engine_may_run_in_software_steps(self) -> None:
        for word in (0x1ee22820,   # fadd h0, h1, h2 (FEAT_FP16)
                     0x1e284020,   # frint32z s0, s1 (FEAT_FRINTTS)
                     0x1ef80020,   # fcvtzs w0, h1 (FEAT_FP16)
                     0x1e634020,   # bfcvt h0, s1 (FEAT_BF16)
                     0x1e7e0020):  # fjcvtzs w0, d1 (FEAT_JSCVT)
            self.assertEqual(self.steps(word), 1, hex(word))


class BudgetTests(unittest.TestCase):
    def test_every_backward_branch_target_is_charged(self) -> None:
        # Function B (0x1010..0x1020) branches back into the interior of
        # function A (0x1000..0x1010): a cycle through two unwind ranges.
        # b #-0xc from 0x1018 is the branch to 0x100c.
        b_back = 0x14000000 | ((-3) & 0x3ffffff)
        words = [NOP, NOP, NOP, NOP,          # A: 0x1000-0x100c
                 NOP, NOP, b_back, RET]       # B: 0x1010-0x101c
        library = library_of(words, [(0, 0x10), (0x10, 0x20)])
        self.assertIn(0x100c, library.charge)
        self.assertIn(0x1000, library.charge)
        self.assertIn(0x1010, library.charge)

    def test_a_self_loop_is_charged(self) -> None:
        words = [NOP, 0x14000000, RET]  # b . at 0x1004
        library = library_of(words, [(0, 0xc)])
        self.assertIn(0x1004, library.charge)


class RunBoundTests(unittest.TestCase):
    """No run of code is longer than the budget pays for (RUN_LIMIT)."""

    def test_long_straight_code_is_split(self) -> None:
        words = [NOP] * 200 + [RET]
        library = library_of(words, [(0, 4 * len(words))])
        points = sorted(library.charge)
        self.assertGreater(len(points), 200 // rewrite.RUN_LIMIT)
        for offset, cost in library.charge.items():
            self.assertLessEqual(cost, rewrite.RUN_LIMIT + 1)
        # The gaps between charge points are at most RUN_LIMIT words.
        edges = points + [0x1000 + 4 * len(words)]
        for before, after in zip(edges, edges[1:]):
            self.assertLessEqual((after - before) // 4, rewrite.RUN_LIMIT + 1)

    def test_a_return_loop_pays_for_what_it_executes(self) -> None:
        # Forty instructions and a RET that returns to the start: the
        # reviewed case that retired 42,000 instructions between polls
        # while charging four a lap. Every lap pays for the run it landed
        # on, plus the RET.
        words = [NOP] * 40 + [RET]
        library = library_of(words, [(0, 4 * len(words))])
        unit = unit_words(library)
        self.assertIn(A.add_imm(rewrite.BUD, rewrite.BUD,
                                rewrite.RUN_LIMIT + 1, sub=True), unit)
        # The lap itself crosses charge points that cover its 41 words.
        paid = sum(library.charge[offset] for offset in library.charge)
        self.assertGreaterEqual(paid, len(words))


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


class SimdStructureTests(unittest.TestCase):
    """Advanced SIMD structure loads and stores are rewritten unless the
    base is SP or the word is reserved."""

    def test_only_sp_and_reserved_rows_step(self) -> None:
        elf = elf64.Elf(fixture.build())
        library = rewrite.Library(
            0, "/aot-test/libaotfixture.so", elf,
            a64decode.Decoder(a64decode.DEFAULT_TABLE),
            rewrite.read_layout(rewrite.LAYOUT_HEADER))
        library.build([1] * ((library.table_end - library.table_base) // 4096))
        causes = dict(library.step_list)
        for index, (name, _) in enumerate(fixture.simd_rows()):
            cause = causes.get(fixture.SIMD_AT + index * fixture.SIMD_STEP)
            if "_sp" in name:
                self.assertEqual(cause, "simd-struct-sp", name)
            elif "reserved" in name:
                self.assertIsNotNone(cause, name)
            else:
                self.assertIsNone(cause, name)

    def test_the_warm_rows_do_not_step(self) -> None:
        elf = elf64.Elf(fixture.build())
        library = rewrite.Library(
            0, "/aot-test/libaotfixture.so", elf,
            a64decode.Decoder(a64decode.DEFAULT_TABLE),
            rewrite.read_layout(rewrite.LAYOUT_HEADER))
        library.build([1] * ((library.table_end - library.table_base) // 4096))
        causes = dict(library.step_list)
        for index, (name, _) in enumerate(fixture.simd_warm_rows()):
            base = fixture.SIMD_WARM_AT + index * fixture.SIMD_WARM_STEP
            self.assertIsNone(causes.get(base), name)
            self.assertIsNone(causes.get(base + 4), name)

    def test_the_rows_fit_between_the_mops_rows_and_the_edges(self) -> None:
        end = fixture.SIMD_WARM_AT + fixture.SIMD_WARM_STEP * len(
            fixture.simd_warm_rows())
        self.assertLessEqual(end, min(fixture.EDGES.values()))
        self.assertGreaterEqual(fixture.SIMD_AT,
                                fixture.MOPS_AT + fixture.MOPS_STEP *
                                len(fixture.MOPS_ROWS))


class LargeLibraryTests(unittest.TestCase):
    """A library's text is not capped at 16 MiB: a page state beyond the 12
    bits of a load's offset costs the gates that name it one more word."""

    NOP_WORD = NOP

    def gate_words(self, page: int) -> list[int]:
        return [A.ldr_x(rewrite.T0, 31, rewrite.SLOT_PSTATE)] + (
            [A.add_imm(rewrite.T0, rewrite.T0, page >> 12, shift12=True)]
            if page >= 4096 else []) + [
            A.ldrb_w(rewrite.T0, rewrite.T0, page & 0xfff)]

    def contains(self, unit: list[int], words: list[int]) -> bool:
        return any(unit[i:i + len(words)] == words
                   for i in range(len(unit) - len(words) + 1))

    def test_a_gate_to_a_page_beyond_the_load_offset_adds_a_word(self) -> None:
        far = 17 * 1024 * 1024
        words = [NOP] * (far // 4 + 4)
        # A function at the start branches to one in the 4097th page.
        target = 4097 * 4096
        words[0] = 0x14000000 | (target >> 2)
        words[target // 4] = NOP
        words[target // 4 + 1] = RET
        library = library_of(words, [(0, 4), (target, target + 8)])
        unit = unit_words(library)
        self.assertTrue(self.contains(unit, self.gate_words(4097)))
        self.assertFalse(self.contains(unit, [
            A.ldr_x(rewrite.T0, 31, rewrite.SLOT_PSTATE),
            A.ldrb_w(rewrite.T0, rewrite.T0, 4097 & 0xfff)]))

    def test_a_gate_below_the_offset_keeps_its_three_words(self) -> None:
        target = 4095 * 4096
        words = [NOP] * (target // 4 + 4)
        words[0] = 0x14000000 | (target >> 2)
        words[target // 4 + 1] = RET
        library = library_of(words, [(0, 4), (target, target + 8)])
        unit = unit_words(library)
        self.assertTrue(self.contains(unit, self.gate_words(4095)))

    def test_text_above_16_mib_in_the_address_space_builds(self) -> None:
        library = rewrite.Library(
            0, "/test/lib.so", FakeElf([RET, NOP], [(0, 4)], base=0x2000000),
            a64decode.Decoder(), rewrite.read_layout(rewrite.LAYOUT_HEADER))
        unit = unit_words(library)
        # REFILL_FAIL and IND form the base of the code in a register.
        self.assertTrue(self.contains(unit, [
            A.movz(rewrite.T0, 0), A.movk(rewrite.T0, 0x200, 1)]))
        self.assertTrue(self.contains(unit, [
            A.movz(rewrite.T2, 0), A.movk(rewrite.T2, 0x200, 1),
            A.add_reg(rewrite.T1, rewrite.T1, rewrite.T2, sub=True)]))


class CensusTests(unittest.TestCase):
    """The byte census names the kind of every word of a unit."""

    LDR = 0xf9400020      # ldr x0, [x1]
    STR = 0xf9000022      # str x2, [x1]
    ADD = 0x8b020020      # add x0, x1, x2

    def census(self) -> tuple[dict, int]:
        library = library_of([self.LDR, self.STR, self.ADD, RET],
                             [(0, 16)])
        unit = unit_words(library)
        return library.stats.census, 4 * len(unit)

    def test_the_kinds_add_up_to_the_unit(self) -> None:
        census, size = self.census()
        self.assertEqual(sum(census.values()), size)
        self.assertEqual(census["other"], 0)

    def test_a_load_and_a_store_are_a_probe_an_access_and_a_stub(self) -> None:
        census, _ = self.census()
        # A probe of a wide access is eight words: the untag, the index, the
        # entry address, the entry load, the host address, the last byte,
        # the compare and the branch to its cold stub of two words.
        self.assertEqual(census["mem.probe"], 2 * 8 * 4)
        self.assertEqual(census["mem.access"], 2 * 4)
        self.assertEqual(census["stub.mm"], 2 * 2 * 4)
        self.assertEqual(census["insn"], 4)
        self.assertEqual(census["table"], 4096)  # one text page of entries

    def test_only_the_refill_routines_a_site_enters_exist(self) -> None:
        census, _ = self.census()
        # A load and a store of eight bytes: a movz and a branch each.
        self.assertEqual(census["stub.refill"], 2 * 2 * 4)

    def test_a_function_that_falls_off_its_end_tags_the_branch(self) -> None:
        library = library_of([self.ADD, self.ADD], [(0, 8)])
        unit = unit_words(library)
        self.assertEqual(sum(library.stats.census.values()), 4 * len(unit))
        self.assertEqual(library.stats.census["other"], 0)

    def test_nothing_is_untagged_in_a_whole_library(self) -> None:
        elf = elf64.Elf(fixture.build())
        library = rewrite.Library(
            0, "/aot-test/libaotfixture.so", elf,
            a64decode.Decoder(a64decode.DEFAULT_TABLE),
            rewrite.read_layout(rewrite.LAYOUT_HEADER))
        unit = library.build(
            [1] * ((library.table_end - library.table_base) // 4096))["unit"]
        census = library.stats.census
        self.assertEqual(sum(census.values()), len(unit))
        self.assertEqual(census["other"], 0)

    def test_a_build_with_an_untagged_word_is_refused(self) -> None:
        # Every framework build holds the census invariant, not only this
        # test: an emitter that stops tagging its words fails the build.
        elf = elf64.Elf(fixture.build())
        library = rewrite.Library(
            0, "/aot-test/libaotfixture.so", elf,
            a64decode.Decoder(a64decode.DEFAULT_TABLE),
            rewrite.read_layout(rewrite.LAYOUT_HEADER))
        original = rewrite.Library.tag

        def skip_exit_stubs(self, kind, start):
            if kind != "stub.xu":
                original(self, kind, start)

        with mock.patch.object(rewrite.Library, "tag", skip_exit_stubs):
            with self.assertRaises(RuntimeError) as raised:
                library.build([1] * ((library.table_end -
                                      library.table_base) // 4096))
        self.assertIn("no census kind", str(raised.exception))

    def test_functions_that_leave_for_one_target_share_its_stub(self) -> None:
        # Three functions, each ending in a branch to the same uncovered
        # address (and a fourth to another one): one stub per target.
        def b(from_word: int, to_offset: int) -> int:
            return 0x14000000 | (((to_offset - 4 * from_word) >> 2) & 0x3ffffff)
        words = [b(0, 0x100), b(1, 0x100), b(2, 0x100), b(3, 0x104)]
        library = library_of(words, [(0, 4), (4, 4), (8, 4), (12, 4)])
        unit_words(library)
        # movz + b, per distinct target.
        self.assertEqual(library.stats.census["stub.xb"], 2 * 2 * 4)

    def test_a_second_build_starts_with_no_shared_stubs(self) -> None:
        def b(from_word: int, to_offset: int) -> int:
            return 0x14000000 | (((to_offset - 4 * from_word) >> 2) & 0x3ffffff)
        words = [b(0, 0x100), b(1, 0x100)]
        library = library_of(words, [(0, 4), (4, 4)])
        first = unit_words(library)
        second = unit_words(library)
        self.assertEqual(first, second)

    def test_a_text_page_is_stored_as_its_sha256(self) -> None:
        library = library_of([self.ADD, RET], [(0, 8)])
        pages = (library.table_end - library.table_base) // 4096
        self.assertEqual(pages, 1)
        source = bytes(range(256)) * 16
        unit = library.build([1] * pages, source)
        self.assertEqual(library.stats.census["src"], 32)
        stored = unit["unit"][unit["hashes_at"]:unit["hashes_at"] + 32]
        self.assertEqual(stored, hashlib.sha256(source).digest())
        self.assertEqual(len(unit["unit"]) - unit["hashes_at"], 32)

    def test_every_page_has_its_own_hash_in_page_order(self) -> None:
        source = b"".join(bytes([n]) * 4096 for n in range(5))
        hashes = rewrite.page_hashes(source)
        self.assertEqual(len(hashes), 5 * rewrite.PAGE_HASH_BYTES)
        for n in range(5):
            self.assertEqual(
                hashes[32 * n:32 * n + 32],
                hashlib.sha256(bytes([n]) * 4096).digest())

    def test_one_bit_in_a_page_changes_its_hash(self) -> None:
        page = bytearray(range(256)) * 16
        before = rewrite.page_hashes(bytes(page))
        for offset in (0, 1, 2047, 2048, 4094, 4095):
            changed = bytearray(page)
            changed[offset] ^= 0x10
            self.assertNotEqual(rewrite.page_hashes(bytes(changed)), before)

    def test_a_partial_page_of_source_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            rewrite.page_hashes(bytes(100))


class WideningTests(unittest.TestCase):
    """Ordered accesses, atomics and the MOPS stages that rewritten code
    runs itself, and what is left to the engine."""

    # Words an assembler produced for the encoders added with them.
    ENCODINGS = {
        "and x2, x1, #0x00ffffffffffffff": (A.and_ones(2, 1, 56), 0x9240dc22),
        "orr x26, xzr, #0x7fffffffffffffff":
            (A.orr_ones(26, 31, 63), 0xb240fbfa),
        "orr x26, xzr, #0x007fffffffffffff":
            (A.orr_ones(26, 31, 55), 0xb240dbfa),
        "cmp x1, x2": (A.cmp_reg(1, 2), 0xeb02003f),
        "csel x1, x1, x26, ls": (A.csel(1, 1, 26, 9), 0x9a9a9021),
        "ccmp x28, #0, #4, lo": (A.ccmp_imm(28, 0, 4, 3), 0xfa403b84),
        "ldar w3, [x28]": (fixture.ordered(True, 2, 28, 3), 0x88dfff83),
        "stlrh w4, [x28]": (fixture.ordered(False, 1, 28, 4), 0x489fff84),
        "ldaddal x26, x27, [x28]":
            (fixture.amo(3, 1, 1, 26, 0, False, 28, 27), 0xf8fa039b),
        "swplh w26, w27, [x28]":
            (fixture.amo(1, 0, 1, 26, 0, True, 28, 27), 0x787a839b),
        "casa x26, x27, [x28]": (fixture.cas(3, 1, 0, 26, 28, 27), 0xc8fa7f9b),
        "casah w26, w27, [x28]":
            (fixture.cas(1, 1, 0, 26, 28, 27), 0x48fa7f9b),
        "ld1 {v0.16b}, [x1]": (fixture.simd_multi(True, 1, 7, 0, 1, 0),
                               0x4c407020),
        "ld1 {v0.16b}, [x1], #16": (fixture.simd_multi(True, 1, 7, 0, 1, 0,
                                                       31), 0x4cdf7020),
        "ld1 {v0.16b, v1.16b}, [x1], x2":
            (fixture.simd_multi(True, 1, 10, 0, 1, 0, 2), 0x4cc2a020),
        "ld4 {v0.4s-v3.4s}, [x1], #64":
            (fixture.simd_multi(True, 1, 0, 2, 1, 0, 31), 0x4cdf0820),
        "st3 {v0.8h-v2.8h}, [x1]": (fixture.simd_multi(False, 1, 4, 1, 1, 0),
                                    0x4c004420),
        "ld1 {v0.s}[1], [x1], #4":
            (fixture.simd_single(True, 0, 0, 4, 1, 0, 1, 0, 31),
             0x0ddf9020),
        "ld1r {v0.16b}, [x1], #1":
            (fixture.simd_single(True, 1, 0, 6, 0, 0, 1, 0, 31), 0x4ddfc020),
        "ld1r {v0.2d}, [x1], x2":
            (fixture.simd_single(True, 1, 0, 6, 0, 3, 1, 0, 2), 0x4dc2cc20),
        "ld2 {v0.h, v1.h}[3], [x1]":
            (fixture.simd_single(True, 0, 1, 2, 1, 2, 1, 0), 0x0d605820),
        "cpyfp [x0]!, [x1]!, x2!": (fixture.mops("cpyf", 0, 0, 0, 2, 1),
                                    0x19010440),
        "cpym [x0]!, [x1]!, x2!": (fixture.mops("cpy", 1, 0, 0, 2, 1),
                                   0x1d410440),
        "cpye [x0]!, [x1]!, x2!": (fixture.mops("cpy", 2, 0, 0, 2, 1),
                                   0x1d810440),
        "cpypwn [x0]!, [x1]!, x2!": (fixture.mops("cpy", 0, 4, 0, 2, 1),
                                     0x1d014440),
        "setp [x0]!, x1!, x2": (fixture.mops("set", 0, 0, 0, 1, 2),
                                0x19c20420),
        "setm [x0]!, x1!, x2": (fixture.mops("set", 1, 0, 0, 1, 2),
                                0x19c24420),
        "setptn [x0]!, x1!, x2": (fixture.mops("set", 0, 3, 0, 1, 2),
                                  0x19c23420),
        # The page-edge functions of the fixture, from TARGET = 0x4100.
        "b target": (fixture.b(0x3004, 0x4100), 0x1400043f),
        "bl target": (fixture.bl(0x3024, 0x4100), 0x94000437),
        "b.eq target": (fixture.b_cond(0, 0x3048, 0x4100), 0x540085c0),
        "cbz x2, target": (fixture.cbz(2, 0x3068, 0x4100), 0xb40084c2),
        "tbz x2, #0, target": (fixture.tbz(2, 0, 0x3088, 0x4100),
                               0x360083c2),
        "adr x16, target": (fixture.adr(16, 0x30a4, 0x4100), 0x100082f0),
        "adr x30, target": (fixture.adr(30, 0x30e4, 0x4100), 0x100080fe),
    }

    def test_encoders_match_an_assembler(self) -> None:
        for text, (made, assembled) in self.ENCODINGS.items():
            self.assertEqual(made, assembled, f"{text}: {made:#x}")

    def test_shared_memory_loops_match_an_assembler(self) -> None:
        # Words from the system assembler for the loops the differential test
        # runs on one shared cell (fixture.hammer_code).
        code = fixture.hammer_code()
        add, cas, llsc = (fixture.HAMMER_ADD, fixture.HAMMER_CAS,
                          fixture.HAMMER_LLSC)
        expected = {
            add: 0xd2800023, add + 4: 0xf8230044, add + 8: 0xf1000400,
            add + 12: 0x54ffffc1, add + 16: 0xd65f03c0,
            cas: 0xf9400045, cas + 4: 0x910004a6, cas + 8: 0xaa0503e7,
            cas + 12: 0xc8a77c46, cas + 16: 0xeb0500ff,
            cas + 20: 0x54ffff61, cas + 24: 0xf1000400,
            cas + 28: 0x54ffff21, cas + 32: 0xd65f03c0,
            llsc: 0xc85f7c45, llsc + 4: 0x910004a5, llsc + 8: 0xc8067c45,
            llsc + 12: 0x35ffffa6, llsc + 16: 0xf1000400,
            llsc + 20: 0x54ffff61, llsc + 24: 0xd65f03c0}
        self.assertEqual(code, expected)

    def test_memory_copy_and_set_rows_match_an_assembler(self) -> None:
        # The rows the differential test starts memory copies and sets from
        # (fixture.mops_code), as the system assembler wrote the stages of
        # CPYF, CPY and SET with registers 0, 2, 1, then 17, 16, 15, and a
        # SET whose fill is the zero register.
        expected = {}
        for row, words in enumerate((
                (0x19010440, 0x19410440, 0x19810440),
                (0x1d010440, 0x1d410440, 0x1d810440),
                (0x19c10440, 0x19c14440, 0x19c18440),
                (0x190f0611, 0x194f0611, 0x198f0611),
                (0x1d0f0611, 0x1d4f0611, 0x1d8f0611),
                (0x19cf0611, 0x19cf4611, 0x19cf8611),
                (0x19df0440, 0x19df4440, 0x19df8440))):
            base = fixture.MOPS_AT + row * fixture.MOPS_STEP
            for stage, word in enumerate(words):
                expected[base + 4 * stage] = word
            expected[base + 12] = 0xd65f03c0
        self.assertEqual(fixture.mops_code(), expected)
        # Past the shared-memory loops, before the page edges.
        self.assertGreater(fixture.MOPS_AT, fixture.HAMMER_AT + 0x80)
        self.assertLess(fixture.MOPS_AT + len(fixture.MOPS_ROWS) *
                        fixture.MOPS_STEP, fixture.SRC_PAGE)

    def test_the_differential_test_finds_the_forms_it_names(self) -> None:
        # The test addresses cas_<size>_<ordering> by arithmetic.
        names = [name for name, _ in fixture.forms()]
        for size in range(4):
            for order in range(4):
                self.assertEqual(
                    names[177 + 2 * (size * 4 + order)],
                    f"cas_{size}_{order}")
        self.assertLessEqual(
            fixture.FORMS_AT + len(names) * fixture.FORM_SIZE,
            fixture.HAMMER_AT)

    def test_only_the_exclusive_loop_leaves_rewritten_code(self) -> None:
        elf = elf64.Elf(fixture.build())
        library = rewrite.Library(
            0, "/aot-test/libaotfixture.so", elf,
            a64decode.Decoder(a64decode.DEFAULT_TABLE),
            rewrite.read_layout(rewrite.LAYOUT_HEADER))
        library.build([1] * ((library.table_end - library.table_base) // 4096))
        stepped = sorted(offset for offset, _ in library.step_list
                         if fixture.HAMMER_AT <= offset < fixture.HAMMER_AT
                         + 0x100)
        self.assertEqual(stepped, [fixture.HAMMER_LLSC,
                                   fixture.HAMMER_LLSC + 8])

    def test_what_steps_is_what_the_engine_must_run(self) -> None:
        elf = elf64.Elf(fixture.build())
        library = rewrite.Library(
            0, "/aot-test/libaotfixture.so", elf,
            a64decode.Decoder(a64decode.DEFAULT_TABLE),
            rewrite.read_layout(rewrite.LAYOUT_HEADER))
        library.build([1] * ((library.table_end - library.table_base) // 4096))
        causes = dict(library.step_list)
        stepped: dict[str, set[str]] = {}
        for index, (name, words) in enumerate(fixture.forms()):
            base = fixture.FORMS_AT + index * fixture.FORM_SIZE
            for position in range(len(words)):
                cause = causes.get(base + 4 * position)
                if cause is not None:
                    stepped.setdefault(name, set()).add(cause)
        expected = {}
        for name, _ in fixture.forms():
            if name.endswith("_sp"):
                expected[name] = {
                    "ordered-sp" if name[:4] in ("ldar", "stlr") else
                    "atomic-sp"}
        for name, cause in (("ldapr", "memop:LDAPR:FEAT_LRCPC"),
                            ("ldapr_b", "memop:LDAPRB:FEAT_LRCPC"),
                            ("ldlar", "ldstord:LDLAR:FEAT_LOR"),
                            ("stllr", "ldstord:STLLR:FEAT_LOR")):
            expected[name] = {cause}
        for kind in ("cpyf", "cpy", "set"):
            for name in ("same", "xzr_d"):
                expected[f"{kind}_{name}"] = {"mops-invalid"}
        expected["cpyf_zs"] = {"mops-invalid"}
        expected["cpy_zs"] = {"mops-invalid"}
        # The epilogue is carried out by a thunk unless an operand lives in a
        # frame slot; the prologue of a copy with two of them steps.
        for kind in ("cpyf", "cpy", "set"):
            for name in ("vn", "vd", "vs", "vdvn", "vnvs"):
                expected[f"{kind}_{name}"] = {"mops-epilogue-virtual"}
        expected["isb_mov"] = {"barriers:ISB:baseline"}
        expected["cpy_vdvn"] = {"mops-virtual", "mops-epilogue-virtual"}
        expected["cpy_vnvs"] = {"mops-virtual", "mops-epilogue-virtual"}
        # "xzr_d" is parsed as ("xzr", "d").
        for kind in ("cpyf", "cpy", "set"):
            expected[f"{kind}_xzr_d"] = {"mops-invalid"}
        self.assertEqual(stepped, expected)


if __name__ == "__main__":
    unittest.main()
