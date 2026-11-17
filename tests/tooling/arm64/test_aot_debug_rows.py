#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The debugger and crash-report rows of the AOT guest runner, on scripted
records.

tests/integration/aot/debug_rows.py digests tombstones into records, checks
that each mode showed what it exists to show, and compares the records of a
mode with the component off and on, field by field. These rows give it a good
record and every kind of bad one: a frame that moved, a register that changed
only with the component, a record that is incomplete or does not decode, a
crash that left no tombstone, a name that looks like a field, a value that is
cut short.
"""

__import__("sys").dont_write_bytecode = True

import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "integration" / "aot"))
import debug_rows as rows  # noqa: E402
import records as rec  # noqa: E402
from records import Named, Record, Unnamed  # noqa: E402


def quoted(address: int) -> str:
    return f"{address >> 32:016x}"[8:] + "'" + f"{address & 0xffffffff:08x}"


def tombstone(address: int = 0x7ff1b1f19000, frame: str = "__memmove_aarch64_mops+16", x13: int = 1,
              pid: int = 2038, extra_threads: str = "", load: int = 0x7ff1ae500000,
              pointer: int = 0x2854d, stack_name: str = "[anon:stack_and_tls:2040]",
              unnamed: int = 0x1234, unnamed_size: int = 0x100000, second_unnamed: bool = False,
              unnamed_target: int = 0, lower_unnamed: bool = False, lower_size: int = 0,
              x5_unnamed: bool = False) -> str:
    """A tombstone of a crash in libc (loaded at `load`): x4 points `pointer`
    bytes into libc's file, x5 into its .bss, x6 into an unnamed mapping, and
    x7 into a thread stack. With `second_unnamed` a second unnamed mapping of
    the same size and protection lies above the stack, and with
    `lower_unnamed` a third lies below the first (of `lower_size` bytes if
    that is given, which makes its shape another); `unnamed_target` says
    which x6 points into: 0 the first, 1 the one above the stack, 2 the one
    below the first. With `x5_unnamed` x5 points into a fourth unnamed mapping
    of 4 KiB instead of the .bss."""
    base = load + 0x70d50
    unnamed_base = load + {0: 0x200000, 1: 0x500000, 2: 0xc0000}[unnamed_target]
    below = lower_size or unnamed_size
    lower_map = (f"""    {quoted(load + 0xc0000)}-{quoted(load + 0xc0000 + below - 1)} rw-         0    {below:x}
                               VmFlags: rd wr mr mw me ac
""" if lower_unnamed else "")
    small_map = (f"""    {quoted(load + 0x1e0000)}-{quoted(load + 0x1e1000 - 1)} rw-         0      1000
                               VmFlags: rd wr mr mw me ac
""" if x5_unnamed else "")
    x5_value = load + 0x1e0010 if x5_unnamed else load + 0xb0000 + 0x2070
    extra_map = (f"""    {quoted(load + 0x500000)}-{quoted(load + 0x500000 + unnamed_size - 1)} rw-         0    {unnamed_size:x}
                               VmFlags: rd wr mr mw me ac
""" if second_unnamed else "")
    return f"""*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
Build fingerprint: 'Android/test:17/ID/1:user/release-keys'
Timestamp: 2026-11-16 05:48:50.054109184+0000
Process uptime: 1s
Executable: /system/bin/toybox
pid: {pid}, ppid: 2036, tid: {pid}, name: toybox  >>> /system/bin/toybox <<<
uid: 2000
signal 11 (SIGSEGV), code 2 (SEGV_ACCERR), fault addr {address:#018x} (read)
    x0  b4007ff19a825220  x1  {address:016x}  x2  0000000000000028  x3  b4007ff19a825238
    x4  {load + pointer:016x}  x5  {x5_value:016x}  x6  {unnamed_base + unnamed:016x}  x7  {load + 0x300000 + 0xf8c00:016x}
    x12 0000000000000001  x13 {x13:016x}  x14 0000000000000001  x15 0000000000001863
    lr  {load + 0x25d0:016x}  sp  00007ffff7299dd0  pc  {base:016x}  pst 0000000020000000

4 total frames
backtrace:
      #00 pc 0000000000070d50  /apex/com.android.runtime/lib64/bionic/libc.so ({frame}) (BuildId: f1804e2e)
      #01 pc 00000000000195cc  /system/lib64/libz.so (updatewindow(z_stream_s*, unsigned char const*, unsigned int)+76) (BuildId: 014ea0f1)
      #02 pc 00000000000197b4  /system/lib64/libz.so (inflateSetDictionary+148) (BuildId: 014ea0f1)
      #03 pc 000000000000500c  /data/local/tmp/libaotdebug.so

memory near x3 ([stack]):
    00007fffd9634e10 0000000000000001 0000000000000002  ................

memory near pc (/apex/com.android.runtime/lib64/bionic/libc.so):
    {base:016x} b90017e093407e68 93407e81aa0803e0  h~@..........~@.
    {base + 16:016x} 528000c2910003e3 d400000152801e08  .......R...R....

memory map (6 entries):
    {quoted(0x400000)}-{quoted(0x401000 - 1)} r-x         0      1000  /system/bin/toybox (BuildId: ef9a4dcc)
                               VmFlags: rd ex mr mw me
    {quoted(load)}-{quoted(load + 0xb0000 - 1)} r-x         0     b0000  /apex/com.android.runtime/lib64/bionic/libc.so (BuildId: f1804e2e)
                               VmFlags: rd ex mr mw me
    {quoted(load + 0xb0000)}-{quoted(load + 0xb3000 - 1)} rw-         0      3000  [anon:.bss]
                               VmFlags: rd wr mr mw me ac
{lower_map}{small_map}    {quoted(load + 0x200000)}-{quoted(load + 0x200000 + unnamed_size - 1)} rw-         0    {unnamed_size:x}
                               VmFlags: rd wr mr mw me ac
    {quoted(load + 0x300000)}-{quoted(load + 0x400000 - 1)} rw-         0    100000  {stack_name}
                               VmFlags: rd wr mr mw me ac
{extra_map}{extra_threads}"""


def sibling_block(offset: int = 360, name: str = "crc32_z") -> str:
    return f"""--- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---
pid: 2111, ppid: 2108, tid: 2112, name: toybox  >>> /system/bin/toybox <<<
uid: 2000
    x0  0000000068e70125  x1  43736f6c69757273
backtrace:
      #00 pc 00000000000123d8  /system/lib64/libz.so ({name}+{offset}) (BuildId: 014ea0f1)
      #01 pc 0000000000005fec  /data/local/tmp/libaotdebug.so
"""




class Wire:
    """What the guest program prints: the lines of its records."""

    @staticmethod
    def lines(recs):
        return [line for each in recs for line in rec.encode_record(each)]

    @staticmethod
    def text(recs, extra=""):
        return "\n".join(Wire.lines(recs)) + "\n" + extra


def regs_record(tag: str = "crc32", **fields) -> Record:
    """A register record of the guest program: x00 to x30, sp, pc and nzcv,
    with the values of `fields` in place of the defaults."""
    values = {"x00": 0, "x01": Named("D", 0), "x02": 0xc81, "x03": Named("S", 0xff40),
              "x04": Named("linker64", 0x1d5058), "x05": 0x7262006563617375,
              "sp": Named("S", 0), "pc": Named("libz.so", 0x12190), "nzcv": 0}
    values.update(fields)
    return Record("regs", (("tag", tag),) + tuple((name, values.get(name, 0)) for name in rec.REGISTERS))


def step(n: int) -> Record:
    return Record("step", (("n", n), ("pc", Named("libz.so", 0x12194 + 4 * n))))


def breakpoint(what: str, offset: int = 0x12190, at: str = "yes") -> Record:
    return Record("breakpoint", (("what", what), ("hit", Named("libz.so", offset)), ("at_breakpoint", at)))


def randomization() -> Record:
    return Record("randomization", (("state", "off"),))


def done(mode: str) -> Record:
    return Record("done", (("mode", mode),))


def ptrace_records(with_marker: bool = False) -> list[Record]:
    out = [randomization()] if with_marker else []
    out += [Record("attached", (("pc", Named("libc.so", 0x97798)),)), breakpoint("crc32"),
            regs_record(), Record("returns_to", (("location", Named("libaotdebug.so", 0x5eb4)),))]
    out += [step(n) for n in range(40)]
    out += [Record("returned", (("pc", Named("libaotdebug.so", 0x5eb4)), ("at_return", "yes"))),
            breakpoint("crc32 on the data"),
            Record("changed", (("what", "length"), ("verdict", "is"))),
            breakpoint("crc32 on the data again"),
            Record("changed", (("what", "pc"), ("verdict", "is")))]
    out += [breakpoint("deflate", 0x12d20)] * 3
    for at, source, target, offset, place in ((110, "libz.so", "libc.so", 0x95610, "libc.so"),
                                             (124, "libc.so", "libz.so", 0xd5f0, "libz.so"),
                                             (153, "libz.so", "libc.so", 0x70d40, "libc.so"),
                                             (159, "libc.so", "libz.so", 0x1385c, "libz.so")):
        out.append(Record("transition", (("step", at), ("from", source), ("to", target),
                                         ("pc", Named(place, offset)))))
    out += [Record("steps", (("steps", 6000), ("changes", 8), ("libc", 32))),
            Record("tracee_done", (("sum", 0x13b56ea3f8e),)),
            Record("detached", (("status", 0),)), done("ptrace")]
    return out


def sibling_hit(kind: str, round_name: str, **changes) -> Record:
    values = {"round": round_name, "hit": "yes", "by_sibling": "yes", "pc_at_breakpoint": "yes",
              "in_libz": "yes", "code": 1}
    if kind == "sibling_entry":
        values["pc"] = Named("libz.so", 0x12190)
    values.update(changes)
    return Record(kind, tuple(values.items()))


def sibling_records(with_marker: bool = False) -> list[Record]:
    out = [randomization()] if with_marker else []
    return out + [sibling_hit("sibling_entry", "entry"),
                  Record("sibling_runs_on", (("round", "entry"), ("yes", "yes"))),
                  sibling_hit("sibling_loop", "in loop"),
                  Record("sibling_runs_on", (("round", "in loop"), ("yes", "yes"))), done("sibling")]


def tombstone_field(records_of_run, kind, name):
    return next(each for each in records_of_run if each.kind == kind).get(name)


class DigestTest(unittest.TestCase):
    def test_the_digest_keeps_the_crash_and_drops_the_rest(self):
        recs = rows.digest_tombstone(tombstone())
        shown = "\n".join(str(each) for each in recs)
        self.assertEqual(tombstone_field(recs, "tomb.signal", "code_name"), "SEGV_ACCERR")
        frames = [each for each in recs if each.kind == "tomb.frame"]
        self.assertEqual(len(frames), 4)
        self.assertIn("__memmove_aarch64_mops+16", frames[0].get("detail"))
        self.assertTrue(frames[0].get("path").endswith("libc.so"))
        self.assertEqual({each.get("region") for each in recs if each.kind == "tomb.code"}, {"pc"})
        self.assertEqual(sum(1 for each in recs if each.kind == "tomb.code"), 2)
        for dropped in ("Timestamp", "memory map", "memory near x3"):
            self.assertNotIn(dropped, shown)

    def test_another_thread_keeps_its_library_and_not_its_position(self):
        one = rows.digest_tombstone(tombstone(extra_threads=sibling_block(360, "crc32_z")))
        two = rows.digest_tombstone(tombstone(extra_threads=sibling_block(352, "crc32")))
        self.assertEqual(one[-3:], two[-3:])
        self.assertEqual([each.kind for each in one[-3:]], ["tomb.thread", "tomb.thread_frame", "tomb.thread_frame"])
        self.assertEqual(one[-2].fields, (("index", 0), ("path", "/system/lib64/libz.so"), ("symbol", "")))

    def test_registers_and_ids_vary_and_frames_do_not(self):
        recs = rows.digest_tombstone(tombstone())
        self.assertIn("x13", recs[[each.kind for each in recs].index("tomb.regs")].names())
        self.assertIn("tomb.regs", rows.MASKABLE)
        self.assertIn("tomb.process", rows.MASKABLE)
        self.assertTrue({each.kind for each in recs if each.kind.startswith("tomb.frame")}.isdisjoint(rows.MASKABLE))

    def test_a_register_seen_twice_is_a_failure(self):
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.digest_tombstone(tombstone().replace("  x7  ", "  x4  ", 1))
        self.assertIn("x4", str(caught.exception))

    def test_a_register_line_it_cannot_read_is_a_failure(self):
        base = tombstone()
        for broken in (base.replace("  x5  ", "  x5  zzzz  x9  ", 1), base.replace("x5  0000", "x5  000", 1),
                       base.replace("  x6  ", "  x6   ", 1).replace("x6   0", "x6   g", 1)):
            with self.subTest(broken=broken[broken.index("x4 "):][:90]):
                with self.assertRaises(rows.DebugFailure):
                    rows.digest_tombstone(broken)

    def test_lines_it_cannot_read_are_failures_and_never_guesses(self):
        base = tombstone()
        for broken in (base.replace("ppid: 2036,", "ppid: ,", 1), base.replace("(SEGV_ACCERR)", "SEGV_ACCERR", 1),
                       base.replace("#00 pc 0000000000070d50", "#00 pc zz", 1),
                       base.replace("uid: 2000", "uid: 2000\nuid: 2001", 1)):
            with self.subTest(broken=broken[:20]):
                with self.assertRaises(rows.DebugFailure):
                    rows.digest_tombstone(broken)

    def test_text_in_a_mapping_name_is_never_read_as_a_field(self):
        """A name that looks like a register does not overwrite the register
        it looks like: the digest never reads the names of the map as fields."""
        recs = rows.digest_tombstone(tombstone(stack_name="[anon:space x6  0 x4  0]"))
        regs = next(each for each in recs if each.kind == "tomb.regs")
        self.assertEqual(regs.get("x7"), Named("[anon:space x6  0 x4  0]", 0xf8c00))
        self.assertEqual(regs.get("x6"), Unnamed(0x100000, "rw-", 0, 1, 0x1234))
        self.assertEqual(regs.get("x4"), Named("libc.so", 0x2854d))


class VerifyTest(unittest.TestCase):
    def test_a_good_memcpy_tombstone(self):
        rows.verify_tombstone("crash-memcpy", rows.digest_tombstone(tombstone()))

    def test_the_fault_must_be_a_read(self):
        text = tombstone().replace("(read)", "(write)")
        with self.assertRaises(rows.DebugFailure):
            rows.verify_tombstone("crash-memcpy", rows.digest_tombstone(text))

    def test_the_place_in_the_page_of_a_fault_is_compared(self):
        def offset(address):
            return tombstone_field(rows.digest_tombstone(tombstone(address=address)), "tomb.signal", "page_offset")

        self.assertEqual(offset(0x7ff1b1f19000), offset(0x7ff1b1f2a000))
        self.assertNotEqual(offset(0x7ff1b1f19000), offset(0x7ff1b1f2a008))
        off = [rows.digest_tombstone(tombstone(address=0x7ff1b1f19000, pid=2038 + n)) for n in range(3)]
        with self.assertRaises(rows.DebugFailure):
            rows.compare("crash-memcpy", off, rows.digest_tombstone(tombstone(address=0x7ff1b1f2a008, pid=2100)))

    def test_the_fault_must_be_in_a_memory_routine(self):
        with self.assertRaises(rows.DebugFailure):
            rows.verify_tombstone("crash-memcpy", rows.digest_tombstone(tombstone(frame="strlen+4")))

    def test_a_sibling_tombstone_needs_the_running_thread(self):
        text = tombstone().replace("SIGSEGV", "SIGABRT").replace("(__memmove_aarch64_mops+16)", "(abort+160)")
        with self.assertRaises(rows.DebugFailure):
            rows.verify_tombstone("crash-sibling", rows.digest_tombstone(text))
        rows.verify_tombstone("crash-sibling", rows.digest_tombstone(text + sibling_block()))


class GuestProgramTest(unittest.TestCase):
    """The program prints records with the header it shares with the host
    (guest_records.hpp, which test_aot_records.py builds and reads back): the
    guest runs only in a guest, so what it uses is read from its source."""

    SOURCE = (pathlib.Path(__file__).resolve().parents[2] / "integration" / "aot" / "aot_guest_debug.cpp")

    def test_everything_it_prints_is_a_record_or_a_failure_line(self):
        text = self.SOURCE.read_text()
        self.assertIn('#include "guest_records.hpp"', text)
        printed = re.findall(r'dprintf\(1, "([^"]*)"', text)
        self.assertTrue(printed)
        self.assertTrue(all(literal.startswith("\\nFAIL ") for literal in printed), printed)
        for gone in ("print_location", "print_mapped", "print_value", "shapes[", "value & 0xfff"):
            self.assertNotIn(gone, text)

    def test_every_error_the_program_prints_is_a_line_of_its_own(self):
        text = self.SOURCE.read_text()
        self.assertNotRegex(text, r'"FAIL ')
        self.assertGreaterEqual(text.count('"\\nFAIL '), 8)

    def test_a_maps_text_that_was_cut_short_fails_the_row(self):
        text = self.SOURCE.read_text()
        self.assertIn("FAIL the maps of process %d do not fit", text)


class RecordTest(unittest.TestCase):
    def test_good_records(self):
        rows.verify_record("ptrace", ptrace_records())
        rows.verify_record("sibling", sibling_records())

    def without(self, recs, kind, **fields):
        return [each for each in recs if not (each.kind == kind and all(each.get(n) == v for n, v in fields.items()))]

    def test_ptrace_record_missing_a_part(self):
        for kind, fields in (("changed", {"what": "pc"}), ("detached", {}), ("done", {}), ("tracee_done", {})):
            with self.subTest(kind=kind):
                with self.assertRaises(rows.DebugFailure):
                    rows.verify_record("ptrace", self.without(ptrace_records(), kind, **fields))

    def test_ptrace_record_that_never_reached_libc(self):
        recs = [Record("steps", (("steps", 6000), ("changes", 1), ("libc", 0))) if each.kind == "steps" else each
                for each in ptrace_records()]
        with self.assertRaises(rows.DebugFailure):
            rows.verify_record("ptrace", recs)

    def test_sibling_that_hit_once(self):
        recs = [sibling_hit("sibling_loop", "in loop", hit="NO") if each.kind == "sibling_loop" else each
                for each in sibling_records()]
        with self.assertRaises(rows.DebugFailure):
            rows.verify_record("sibling", recs)


class CompareTest(unittest.TestCase):
    LOADS = (0x7ff1ae500000, 0x7ff1a1a00000, 0x7ff1b2c00000)

    def runs(self):
        return [rows.digest_tombstone(tombstone(x13=value, pid=2038 + 4 * n))
                for n, value in enumerate((1, 0, 1))]

    def baselines(self, *layouts):
        """One run per layout (keywords of tombstone), at three load addresses."""
        return [rows.digest_tombstone(tombstone(load=load, pid=2038 + n, **layout))
                for n, (load, layout) in enumerate(zip(self.LOADS, layouts))]

    def three(self, **layout):
        return self.baselines(layout, layout, layout)

    def with_component(self, **layout):
        return rows.digest_tombstone(tombstone(load=0x7ff1a8100000, pid=3000, **layout))

    def test_identical_apart_from_what_the_engine_varies(self):
        off = self.runs()
        on = rows.digest_tombstone(tombstone(x13=0, pid=3000))
        compared, masked = rows.compare("crash-memcpy", off, on)
        self.assertGreater(compared, 10)
        self.assertGreater(masked, 0)

    def test_a_register_that_changes_only_with_the_component(self):
        off = [rows.digest_tombstone(tombstone(pid=2038 + n)) for n in range(3)]
        on = rows.digest_tombstone(tombstone(x13=7, pid=2100))
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("crash-memcpy", off, on)
        self.assertIn("with it", str(caught.exception))

    def test_pointers_are_named_by_what_they_point_into(self):
        regs = next(each for each in rows.digest_tombstone(tombstone()) if each.kind == "tomb.regs")
        self.assertEqual(regs.get("x4"), Named("libc.so", 0x2854d))
        self.assertEqual(regs.get("x5"), Named("[anon:.bss]", 0x2070))
        # An unnamed mapping is its shape, its place among the mappings of that
        # shape and how many there are, and the offset in the whole mapping.
        self.assertEqual(regs.get("x6"), Unnamed(0x100000, "rw-", 0, 1, 0x1234))
        self.assertEqual(regs.get("x7"), Named("[anon:stack_and_tls:*]", 0xf8c00))
        self.assertEqual(regs.get("pc"), Named("libc.so", 0x70d50))

    def test_a_library_loaded_elsewhere_is_the_same_run(self):
        off = [rows.digest_tombstone(tombstone(load=load, pid=2038 + n, stack_name=f"[anon:stack_and_tls:{2040 + n}]"))
               for n, load in enumerate(self.LOADS)]
        on = rows.digest_tombstone(tombstone(load=0x7ff1a8100000, pid=3000, stack_name="[anon:stack_and_tls:3002]"))
        compared, masked = rows.compare("crash-memcpy", off, on)
        self.assertLessEqual(masked, 3)
        self.assertGreater(compared, 10)

    def test_a_pointer_that_moves_only_with_the_component_fails(self):
        on = self.with_component(pointer=0x28555)
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("crash-memcpy", self.three(), on)
        self.assertIn("with it", str(caught.exception))

    def test_a_pointer_moved_inside_an_unnamed_mapping_fails(self):
        # The same page offset, a page further in: naming only the offset in
        # the page called these the same pointer.
        for moved in (0x1234 + 0x1000, 0x1234 + 0x10000):
            with self.assertRaises(rows.DebugFailure) as caught:
                rows.compare("crash-memcpy", self.three(), self.with_component(unnamed=moved))
            self.assertIn("with it", str(caught.exception))
            self.assertIn("x6", str(caught.exception))

    def test_a_pointer_into_another_unnamed_mapping_fails(self):
        with self.assertRaises(rows.DebugFailure):
            rows.compare("crash-memcpy", self.three(), self.with_component(unnamed_size=0x80000))

    def test_a_pointer_moved_to_another_mapping_of_the_same_shape_fails(self):
        """Two unnamed mappings of one size and protection are two places: x6
        moved from one to the other, at the same offset, is a moved pointer."""
        off = self.three(second_unnamed=True)
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("crash-memcpy", off, self.with_component(second_unnamed=True, unnamed_target=1))
        self.assertIn("with it", str(caught.exception))
        self.assertIn("x6", str(caught.exception))
        # The same pointer in the same one is the same pointer, at any load address.
        compared, _ = rows.compare("crash-memcpy", off, self.with_component(second_unnamed=True))
        self.assertGreater(compared, 10)

    def assert_ambiguous(self, off, target):
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("crash-memcpy", off, self.with_component(lower_unnamed=True, unnamed_target=target))
        message = str(caught.exception)
        self.assertIn("ambiguous identity", message)
        self.assertIn("anon[0x100000,rw-]", message)
        self.assertIn("1 in one run and 2 in another", message)
        self.assertIn("x6", message)

    # x6 points into the first of two mappings of one shape in the run that
    # has two and into the only one in the runs that have one: the place among
    # them is not one place in both, whether the pointer is the right one (the
    # first mapping, `unnamed_target=0`) or a wrong one (the mapping that
    # appeared, `unnamed_target=2`), and the row says so instead of judging it.
    def test_a_correct_pointer_is_not_judged_by_a_place_among_a_changed_set(self):
        self.assert_ambiguous(self.three(), 0)

    def test_a_wrong_pointer_into_a_mapping_that_appeared_is_not_accepted(self):
        self.assert_ambiguous(self.three(), 2)

    # The runs without the component differ among themselves in the set: no
    # run with it can be compared by a place in it.
    def test_baselines_that_disagree_on_the_set_do_not_judge_a_correct_pointer(self):
        self.assert_ambiguous(self.baselines({}, {"lower_unnamed": True}, {}), 0)

    def test_baselines_that_disagree_on_the_set_do_not_let_a_wrong_pointer_pass(self):
        self.assert_ambiguous(self.baselines({}, {"lower_unnamed": True}, {}), 2)

    def test_runs_that_agree_on_the_set_are_compared_by_their_place_in_it(self):
        both = {"lower_unnamed": True}
        off = self.baselines(both, both, both)
        compared, _ = rows.compare("crash-memcpy", off, self.with_component(lower_unnamed=True))
        self.assertGreater(compared, 10)
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("crash-memcpy", off, self.with_component(lower_unnamed=True, unnamed_target=2))
        self.assertIn("with it", str(caught.exception))
        self.assertNotIn("ambiguous", str(caught.exception))

    def test_a_mapping_of_another_shape_leaves_the_place_alone(self):
        compared, _ = rows.compare("crash-memcpy", self.three(),
                                   self.with_component(lower_unnamed=True, lower_size=0x80000))
        self.assertGreater(compared, 10)

    # An unnamed pointer's identity is proved over the whole record, field by
    # name, or it has none: a field that is an unnamed pointer in one run and
    # something else (another mapping, no mapping, nothing) in another fails,
    # whatever the other fields hold and however many baselines there are.
    def test_an_unnamed_pointer_is_judged_by_its_field_and_not_by_its_place_in_a_list_of_matches(self):
        """x5 is an unnamed pointer in one baseline and the .bss in the other;
        x6 points into the only mapping of its shape in the first and into one
        of two in the second; the run with the component has a mapping below
        and x6 into it."""
        load = 0x700000000000
        first = rows.digest_tombstone(tombstone(load=load, x5_unnamed=True))
        second = rows.digest_tombstone(tombstone(load=load, second_unnamed=True))
        wrong = rows.digest_tombstone(tombstone(load=load, lower_unnamed=True, unnamed_target=2))
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("crash-memcpy", [first, second, first], wrong)
        self.assertIn("ambiguous identity", str(caught.exception))
        self.assertIn("x5", str(caught.exception))

    def test_a_baseline_with_no_unnamed_pointer_on_the_line_is_no_proof(self):
        """x6 is zero in one baseline: the other runs' unnamed pointers have
        nothing to be proved equal to."""
        load = 0x700000000000
        plain = tombstone(load=load)
        zero = plain.replace(f"{load + 0x200000 + 0x1234:016x}", "0" * 16)
        off = [rows.digest_tombstone(plain), rows.digest_tombstone(tombstone(load=load, second_unnamed=True)),
               rows.digest_tombstone(zero)]
        wrong = rows.digest_tombstone(tombstone(load=load, lower_unnamed=True, unnamed_target=2))
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("crash-memcpy", off, wrong)
        self.assertIn("ambiguous identity", str(caught.exception))
        self.assertIn("x6", str(caught.exception))

    def test_an_unnamed_pointer_that_varies_in_the_engine_is_never_masked(self):
        """Two baselines place x6 at different offsets of the same mapping and
        the run with the component at one of them: a named location would be
        judged by the engine's own values, an unnamed mapping's place is not."""
        off = self.baselines({"unnamed": 0x1234}, {"unnamed": 0x1238}, {"unnamed": 0x1234})
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("crash-memcpy", off, self.with_component())
        self.assertIn("x6", str(caught.exception))
        self.assertIn("never masked", str(caught.exception))

    def raw_shift(self, left, right, offset):
        """x4 points into a mapping named `left` and x7 into one named
        `right`; x5 and x6 point into the same unnamed mapping, x6 at
        `offset`. A name with spaces in it shifts every field after it by as
        many whitespace-separated tokens."""
        load = 0x700000000000
        text = tombstone(load=load, stack_name=right, unnamed=offset)
        text = text.replace(f"{load + 0x2854d:016x}", f"{load + 0x700020:016x}")
        text = text.replace(f"{load + 0xb2070:016x}", f"{load + 0x200020:016x}")
        text += (f"    {quoted(load + 0x700000)}-{quoted(load + 0x701000 - 1)} rw-         0    1000  {left}\n"
                 "                               VmFlags: rd wr mr mw me ac\n")
        return rows.digest_tombstone(text)

    def test_names_that_shift_the_whitespace_do_not_make_one_field_another(self):
        """The baselines' x4 and x7 names hold two spaces more or less, so that
        the whitespace-separated tokens of the two lines are as many and x6 of
        one lines up with x5 of the other; the run with the component moves x6
        to x5's place. Fields are compared by name: x6 is x6."""
        first = self.raw_shift("[anon:left]", "[anon:right one two]", 0x10)
        second = self.raw_shift("[anon:left one two]", "[anon:right]", 0x10)
        wrong = self.raw_shift("[anon:left]", "[anon:right one two]", 0x20)
        for each in (first, second, wrong):
            rows.verify_tombstone("crash-memcpy", each)
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("crash-memcpy", [first, second, first], wrong)
        self.assertIn("x6", str(caught.exception))
        # the control: the same shift with x6 where it was passes
        compared, _ = rows.compare("crash-memcpy", [first, second, first],
                                   self.raw_shift("[anon:left]", "[anon:right one two]", 0x10))
        self.assertGreater(compared, 10)

    def test_names_that_look_like_fields_do_not_overwrite_the_field_they_look_like(self):
        """The mapping x7 points into is named like x6 and x4, with values:
        x6 stays the pointer it is, so the run with the component that moves
        it is judged by x6 and not by the name."""
        left, right = "[anon:left x4  0x1  x6  0x2]", "[anon:right x6  0x20 x4  0]"
        first = self.raw_shift(left, right, 0x10)
        second = self.raw_shift("[anon:left]", right, 0x10)
        wrong = self.raw_shift(left, right, 0x20)
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("crash-memcpy", [first, second, first], wrong)
        self.assertIn("x6", str(caught.exception))
        compared, _ = rows.compare("crash-memcpy", [first, second, first], self.raw_shift(left, right, 0x10))
        self.assertGreater(compared, 10)

    def test_a_register_record_is_compared_field_by_field_whatever_its_names_hold(self):
        """x07's name has other numbers of spaces from run to run and looks
        like x06: the records are compared by field, so x06 that moved is x06
        that moved, and x07 is judged by the names the baselines show."""
        place, moved = Unnamed(0x1000, "rw-", 0, 1, 0x10), Unnamed(0x1000, "rw-", 0, 1, 0x20)

        def run(x06, name):
            return [regs_record(x06=x06, x07=Named(name, 0x20))]

        for one, other in (("[anon:a b c]", "[anon:a]"), ("[anon:x06 0 x06 0]", "[anon:x06=0]"),
                           ("[anon:\nAOT\tregs\tx06\tn\t0]", "[anon:]")):
            with self.subTest(one=one):
                off = [run(place, one), run(place, other), run(place, one)]
                compared, masked = rows.compare("ptrace", off, run(place, one))
                self.assertGreaterEqual(compared, 30)
                self.assertEqual(masked, 1)
                with self.assertRaises(rows.DebugFailure) as caught:
                    rows.compare("ptrace", off, run(moved, one))
                self.assertIn("x06", str(caught.exception))
                with self.assertRaises(rows.DebugFailure):
                    rows.compare("ptrace", off, run(place, "[anon:a name no baseline showed]"))

    def test_a_named_location_that_varies_in_the_engine_is_judged_by_the_engines_values(self):
        off = [rows.digest_tombstone(tombstone(pointer=pointer, pid=2038 + n))
               for n, pointer in enumerate((0x2854d, 0x28555, 0x2854d))]
        # the run with the component shows a place the engine showed: no verdict on that field
        compared, masked = rows.compare("crash-memcpy", off, rows.digest_tombstone(tombstone(pid=3000)))
        self.assertGreater(compared, 10)
        self.assertGreater(masked, 0)
        # a place the engine never showed fails, naming the engine's own values
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("crash-memcpy", off, rows.digest_tombstone(tombstone(pointer=0x28600, pid=3000)))
        self.assertIn("the engine's own runs differ", str(caught.exception))
        self.assertIn("run with the component shows", str(caught.exception))

    def test_the_mask_is_the_set_of_kind_and_field_pairs_that_differ(self):
        result = rows.compare_fields("crash-memcpy", self.runs(), rows.digest_tombstone(tombstone(x13=0, pid=3000)))
        self.assertIn(("tomb.regs", "x13"), result.mask)
        self.assertIn(("tomb.process", "pid"), result.mask)
        self.assertIn(("tomb.process", "tid"), result.mask)
        self.assertEqual(result.masked, 3)
        self.assertEqual(result.compared + result.masked, sum(len(each.fields) for each in self.runs()[0]))

    def test_only_a_few_fields_may_be_masked(self):
        def run(value):
            return [Record("regs", (("tag", "crc32"),) + tuple((f"x{n:02d}", n + 100 * value)
                                                               for n in range(rows.MAX_MASKED + 4)))]

        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("ptrace", [run(1), run(2), run(3)], run(1))
        self.assertIn("process ids and random values", str(caught.exception))

    def test_the_code_around_the_pc_is_compared_whatever_the_dump_starts_at(self):
        one = rows.digest_tombstone(tombstone(load=0x7ff1ae500000))
        two = rows.digest_tombstone(tombstone(load=0x7ff1a1a00000))
        dump = lambda recs: [each for each in recs if each.kind == "tomb.code"]
        self.assertEqual(dump(one), dump(two))
        self.assertEqual(len(dump(one)), 2)

    def test_a_frame_that_moves(self):
        off = [rows.digest_tombstone(tombstone(pid=2038 + n)) for n in range(3)]
        on = rows.digest_tombstone(tombstone(frame="__memmove_aarch64_mops+20", pid=2100))
        with self.assertRaises(rows.DebugFailure):
            rows.compare("crash-memcpy", off, on)

    def test_a_frame_that_differs_between_the_engines_own_runs(self):
        off = [rows.digest_tombstone(tombstone(pid=2038)), rows.digest_tombstone(tombstone(frame="x+1", pid=2039)),
               rows.digest_tombstone(tombstone(pid=2040))]
        with self.assertRaises(rows.DebugFailure):
            rows.compare("crash-memcpy", off, rows.digest_tombstone(tombstone(pid=2100)))

    def test_the_runs_must_have_the_same_length(self):
        off = [rows.digest_tombstone(tombstone(pid=2038 + n)) for n in range(3)]
        with self.assertRaises(rows.DebugFailure):
            rows.compare("crash-memcpy", off, rows.digest_tombstone(tombstone() + sibling_block()))

    def test_register_records_mask_only_values(self):
        off = [[regs_record(x05=value)] for value in (1, 2, 3)]
        compared, masked = rows.compare("ptrace", off, [regs_record(x05=0x7262)])
        self.assertEqual(masked, 1)
        self.assertGreaterEqual(compared, 30)
        steady = [[regs_record(x05=1)]] * 3
        with self.assertRaises(rows.DebugFailure):
            rows.compare("ptrace", steady, [regs_record(x05=1, pc=Named("libz.so", 0x12194))])
        with self.assertRaises(rows.DebugFailure):
            rows.compare("ptrace", steady, [regs_record(x05=1, x00=5)])

    def test_a_symbolic_field_that_differs_between_the_engines_own_runs_is_not_masked(self):
        off = [[regs_record(pc=Named("libz.so", 0x12190))], [regs_record(pc=Named("libz.so", 0x12194))],
               [regs_record(pc=Named("libz.so", 0x12190))]]
        with self.assertRaises(rows.DebugFailure):
            rows.compare("ptrace", off, [regs_record(pc=Named("libz.so", 0x12190))])

    def test_a_record_that_is_not_one_of_the_varying_kinds_must_be_equal_everywhere(self):
        steady = [step(3)]
        rows.compare("ptrace", [steady] * 3, steady)
        with self.assertRaises(rows.DebugFailure):
            rows.compare("ptrace", [steady] * 3, [Record("step", (("n", 3), ("pc", Named("libz.so", 0x121a8))))])
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("ptrace", [[step(3)], [step(4)], [step(3)]], [step(3)])
        self.assertIn("the engine's own runs differ", str(caught.exception))

    def test_a_text_is_never_masked_even_in_a_kind_that_varies(self):
        def process(name):
            return [Record("tomb.process", (("pid", 1), ("ppid", 1), ("tid", 1), ("name", name), ("executable", "x")))]

        with self.assertRaises(rows.DebugFailure):
            rows.compare("crash-abort", [process("a"), process("b"), process("a")], process("a"))

    def test_the_programs_register_records_are_judged_by_the_same_rule(self):
        """The guest program's `x06`: a place counted among another number of
        mappings fails, and a place that differs between the baselines is never
        masked."""
        def run(place):
            return [regs_record(x06=place)]

        alike = run(Unnamed(0x1000, "rw-", 0, 1, 0x10))
        rows.compare("ptrace", [alike] * 3, alike)
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("ptrace", [alike] * 3, run(Unnamed(0x1000, "rw-", 0, 2, 0x10)))
        self.assertIn("ambiguous identity", str(caught.exception))
        self.assertIn("x06", str(caught.exception))
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("ptrace", [alike, run(Unnamed(0x1000, "rw-", 0, 1, 0x20)), alike], alike)
        self.assertIn("never masked", str(caught.exception))

    def test_a_field_that_is_an_unnamed_pointer_in_one_run_and_a_number_in_another_fails(self):
        alike = [regs_record(x06=Unnamed(0x1000, "rw-", 0, 1, 0x10))]
        number = [regs_record(x06=0x1010)]
        for off, on in (([alike, number, alike], alike), ([alike] * 3, number), ([number] * 3, alike)):
            with self.assertRaises(rows.DebugFailure) as caught:
                rows.compare("ptrace", off, on)
            self.assertIn("ambiguous identity", str(caught.exception))

    def test_records_of_other_kinds_or_fields_are_not_compared_as_one_record(self):
        with self.assertRaises(rows.DebugFailure):
            rows.compare("ptrace", [[step(3)]] * 3, [Record("returns_to", (("location", Named("libz.so", 1)),))])
        with self.assertRaises(rows.DebugFailure):
            rows.compare("ptrace", [[step(3)]] * 3, [Record("step", (("pc", Named("libz.so", 0x12194 + 12)), ("n", 3)))])


class RunTest(unittest.TestCase):
    def shell(self, outputs):
        calls = []

        def shell(command, timeout=60):
            calls.append(command)
            for needle, output in outputs:
                if needle in command:
                    return output
            return ""
        shell.calls = calls
        return shell

    def test_a_crash_is_judged_by_its_tombstone(self):
        log = "Fatal signal 11 (SIGSEGV), code 2 (SEGV_ACCERR), fault addr 0x7ff1b1f19000 in tid 2038 (toybox), pid 2038 (toybox)\npbtombstone x\n"
        printed = Wire.text([randomization(), Record("warm", (("sum", 1),))], "rc=139\n")
        shell = self.shell([
            ("AOT_DEBUG_MODE=crash-memcpy", printed),
            ("logcat -b crash -d", log),
            ("pidof", ""),
            ("grep -l", "/data/tombstones/tombstone_03\n"),
            ("cat /data/tombstones/tombstone_03", tombstone()),
        ])
        original = rows.time.sleep
        rows.time.sleep = lambda _seconds: None
        try:
            record = rows.run_mode(shell, "crash-memcpy")
        finally:
            rows.time.sleep = original
        self.assertTrue(any(each.kind == "tomb.signal" and each.get("code_name") == "SEGV_ACCERR" for each in record))
        self.assertTrue(any("logcat -b crash -c" in call for call in shell.calls))

    def test_a_crash_with_the_wrong_status_fails(self):
        shell = self.shell([("AOT_DEBUG_MODE=crash-memcpy", "rc=0\n")])
        with self.assertRaises(rows.DebugFailure):
            rows.run_mode(shell, "crash-memcpy")

    def sibling_output(self, recs=None, extra=""):
        return self.shell([("AOT_DEBUG_MODE=sibling", Wire.text(recs or sibling_records(True), extra + "rc=0\n"))])

    def test_a_mode_that_reports_a_failure_fails(self):
        recs = [sibling_hit("sibling_loop", "in loop", in_libz="NO") if each.kind == "sibling_loop" else each
                for each in sibling_records(True)]
        with self.assertRaises(rows.DebugFailure):
            rows.run_mode(self.sibling_output(recs), "sibling")

    def test_a_good_debug_record_is_returned_without_its_status(self):
        record = rows.run_mode(self.sibling_output(), "sibling")
        self.assertEqual(record[0].kind, "randomization")
        self.assertEqual(record[-1], done("sibling"))

    def test_a_run_whose_randomization_was_not_turned_off_is_no_run(self):
        good = Wire.lines(sibling_records(True))
        failure = "\nFAIL randomization is still on after the program ran again"
        for lines in (good[1:], good[:1] * 2 + good, [failure] + good):
            with self.subTest(lines=len(lines)):
                with self.assertRaises(rows.DebugFailure):
                    rows.run_mode(self.shell([("AOT_DEBUG_MODE=sibling", "\n".join(lines) + "\nrc=0\n")]), "sibling")

    def ptrace(self, lines):
        text = "\n".join(lines) + "\nrc=0\n"
        return rows.run_mode(self.shell([("AOT_DEBUG_MODE=ptrace", text)]), "ptrace")

    def ptrace_lines(self):
        return Wire.lines(ptrace_records(True))

    def test_a_good_ptrace_output_is_a_record(self):
        record = self.ptrace(self.ptrace_lines())
        self.assertEqual(record[-1], done("ptrace"))

    def test_a_register_record_with_fewer_fields_than_the_format_has_fails(self):
        lines = self.ptrace_lines()
        regs = [n for n, line in enumerate(lines) if line.split("\t")[1] == "regs"]
        self.assertEqual(len(regs), len(rec.SCHEMA["regs"]))
        for kept in (20, 30, len(regs) - 1):
            with self.subTest(kept=kept):
                with self.assertRaises(rows.DebugFailure) as caught:
                    self.ptrace(lines[:regs[0] + kept] + lines[regs[-1] + 1:])
                self.assertIn("regs", str(caught.exception))
        # cut short in the middle of a line, with nothing after the cut
        with self.assertRaises(rows.DebugFailure):
            self.ptrace(lines[:regs[0] + 3] + [lines[regs[0] + 3][:20]])

    def test_a_failure_marker_inside_a_record_line_fails(self):
        lines = self.ptrace_lines()
        broken = [line.replace("deflate", "FAIL") for line in lines]
        self.assertNotEqual(broken, lines)
        with self.assertRaises(rows.DebugFailure) as caught:
            self.ptrace(broken)
        self.assertIn("FAIL", str(caught.exception))

    def test_a_line_that_is_no_record_line_fails(self):
        lines = self.ptrace_lines()
        at = next(n for n, line in enumerate(lines) if line.split("\t")[1] == "step")
        for stray in ("x07=0 sp=S+0x0 pc=libz.so+0x12190 nzcv=0x0", "step 5  pc=libz.so+0x12198",
                      "AOT\tstep\tn\ti", "AOT step n i 5", "warning: linker: something"):
            with self.subTest(stray=stray):
                with self.assertRaises(rows.DebugFailure) as caught:
                    self.ptrace(lines[:at] + [stray] + lines[at:])
                self.assertIn("record", str(caught.exception))

    def test_malformed_values_are_failures(self):
        lines = self.ptrace_lines()
        regs = [n for n, line in enumerate(lines) if line.split("\t")[1] == "regs"]
        x06 = next(n for n in regs if lines[n].split("\t")[2] == "x06")
        pc = next(n for n in regs if lines[n].split("\t")[2] == "pc")
        nzcv = next(n for n in regs if lines[n].split("\t")[2] == "nzcv")
        cases = {
            "x06 twice": lines[:x06 + 1] + [lines[x06]] + lines[x06 + 1:],
            "pc without its offset": lines[:pc] + ["AOT\tregs\tpc\tm\tlibz.so"] + lines[pc + 1:],
            "nzcv without digits": lines[:nzcv] + ["AOT\tregs\tnzcv\tn\t"] + lines[nzcv + 1:],
            "nzcv 0x": lines[:nzcv] + ["AOT\tregs\tnzcv\tn\t0x"] + lines[nzcv + 1:],
            "x06 dropped": lines[:x06] + lines[x06 + 1:],
            "an unknown field": lines[:x06] + ["AOT\tregs\tx99\tn\t0"] + lines[x06 + 1:],
            "a number with a leading zero": lines[:x06] + ["AOT\tregs\tx06\tn\t07"] + lines[x06 + 1:],
            "text where a number is": lines[:nzcv] + ["AOT\tregs\tnzcv\tt\t0"] + lines[nzcv + 1:],
        }
        for label, broken in cases.items():
            with self.subTest(label):
                with self.assertRaises(rows.DebugFailure):
                    self.ptrace(broken)

    def test_a_name_that_looks_like_a_field_is_one_name_end_to_end(self):
        """x07 points into a mapping named like a field, with a space, `=` and
        a line end in it: the record keeps x06 as the pointer it is, and the
        name as the name."""
        hostile = "[anon:space x06=0\nAOT\tregs\tx06\tn\t0]"
        recs = ptrace_records(True)
        recs[[each.kind for each in recs].index("regs")] = regs_record(
            x06=Unnamed(0x100000, "rw-", 0, 1, 0x1234), x07=Named(hostile, 0x20))
        regs = next(each for each in self.ptrace(Wire.lines(recs)) if each.kind == "regs")
        self.assertEqual(regs.get("x07"), Named(hostile, 0x20))
        self.assertEqual(regs.get("x06"), Unnamed(0x100000, "rw-", 0, 1, 0x1234))
        self.assertTrue(all(len(line.split("\t")) >= 5 and line.startswith("AOT\t") for line in Wire.lines(recs)))


if __name__ == "__main__":
    unittest.main()
