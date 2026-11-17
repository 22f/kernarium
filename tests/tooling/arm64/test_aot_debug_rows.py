#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The debugger and crash-report rows of the AOT guest runner, on scripted
records.

tests/integration/aot/debug_rows.py digests tombstones, checks that each mode
showed what it exists to show, and compares the runs of a mode with the
component off and on. These rows give it a good record and every kind of bad
one: a frame that moved, a register that changed only with the component, a
record that is incomplete, a crash that left no tombstone.
"""

__import__("sys").dont_write_bytecode = True

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "integration" / "aot"))
import debug_rows as rows  # noqa: E402


def quoted(address: int) -> str:
    return f"{address >> 32:016x}"[8:] + "'" + f"{address & 0xffffffff:08x}"


def tombstone(address: int = 0x7ff1b1f19000, frame: str = "__memmove_aarch64_mops+16", x13: int = 1,
              pid: int = 2038, extra_threads: str = "", load: int = 0x7ff1ae500000,
              pointer: int = 0x2854d, stack_name: str = "[anon:stack_and_tls:2040]",
              unnamed: int = 0x1234, unnamed_size: int = 0x100000, second_unnamed: bool = False,
              unnamed_target: int = 0, lower_unnamed: bool = False, lower_size: int = 0) -> str:
    """A tombstone of a crash in libc (loaded at `load`): x4 points `pointer`
    bytes into libc's file, x5 into its .bss, x6 into an unnamed mapping, and
    x7 into a thread stack. With `second_unnamed` a second unnamed mapping of
    the same size and protection lies above the stack, and with
    `lower_unnamed` a third lies below the first (of `lower_size` bytes if
    that is given, which makes its shape another); `unnamed_target` says
    which x6 points into: 0 the first, 1 the one above the stack, 2 the one
    below the first."""
    base = load + 0x70d50
    unnamed_base = load + {0: 0x200000, 1: 0x500000, 2: 0xc0000}[unnamed_target]
    below = lower_size or unnamed_size
    lower_map = (f"""    {quoted(load + 0xc0000)}-{quoted(load + 0xc0000 + below - 1)} rw-         0    {below:x}
                               VmFlags: rd wr mr mw me ac
""" if lower_unnamed else "")
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
    x4  {load + pointer:016x}  x5  {load + 0xb0000 + 0x2070:016x}  x6  {unnamed_base + unnamed:016x}  x7  {load + 0x300000 + 0xf8c00:016x}
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
{lower_map}    {quoted(load + 0x200000)}-{quoted(load + 0x200000 + unnamed_size - 1)} rw-         0    {unnamed_size:x}
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


class DigestTest(unittest.TestCase):
    def test_the_digest_keeps_the_crash_and_drops_the_rest(self):
        record = rows.digest_tombstone(tombstone())
        text = "\n".join(record)
        self.assertIn("SEGV_ACCERR", text)
        self.assertIn("libc.so (__memmove_aarch64_mops+16)", text)
        self.assertIn("= memory near pc", text)
        self.assertNotIn("Timestamp", text)
        self.assertNotIn("memory map", text)
        self.assertNotIn("memory near x3", text)
        self.assertEqual(sum(1 for line in record if line.startswith("=       #")), 4)

    def test_another_thread_keeps_its_library_and_not_its_position(self):
        one = rows.digest_tombstone(tombstone(extra_threads=sibling_block(360, "crc32_z")))
        two = rows.digest_tombstone(tombstone(extra_threads=sibling_block(352, "crc32")))
        self.assertEqual(one[-3:], two[-3:])
        self.assertIn("= thread frame 00 /system/lib64/libz.so", one)

    def test_registers_and_ids_vary_and_frames_do_not(self):
        record = rows.digest_tombstone(tombstone())
        varying = [line for line in record if line.startswith("~ ")]
        self.assertTrue(any("x13" in line for line in varying))
        self.assertTrue(any(line.startswith("~ pid:") for line in varying))
        self.assertFalse(any("#00" in line for line in varying))


class VerifyTest(unittest.TestCase):
    def test_a_good_memcpy_tombstone(self):
        rows.verify_tombstone("crash-memcpy", rows.digest_tombstone(tombstone()))

    def test_the_fault_must_be_a_read(self):
        text = tombstone().replace("(read)", "(write)")
        with self.assertRaises(rows.DebugFailure):
            rows.verify_tombstone("crash-memcpy", rows.digest_tombstone(text))

    def test_the_place_in_the_page_of_a_fault_is_compared(self):
        def signal_line(address):
            return next(line for line in rows.digest_tombstone(tombstone(address=address)) if "signal 11" in line)

        self.assertEqual(signal_line(0x7ff1b1f19000), signal_line(0x7ff1b1f2a000))
        self.assertNotEqual(signal_line(0x7ff1b1f19000), signal_line(0x7ff1b1f2a008))
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


PTRACE = """attached  pc=libc.so+0x97798
breakpoint crc32  hit=libz.so+0x12190 at_breakpoint=yes
  regs crc32 x00=0 x01=D+0 x02=0xc81 x03=S+0xff40 x04=linker64+0x1d5058 x05=0x7262006563617375 sp=S+0x0 pc=libz.so+0x12190 nzcv=0x0
  returns_to=libaotdebug.so+0x5eb4
""" + "".join(f"step {n}  pc=libz.so+{0x12194 + 4 * n:#x}\n" for n in range(40)) + """returned  pc=libaotdebug.so+0x5eb4 at_return=yes
breakpoint crc32 on the data  hit=libz.so+0x12190 at_breakpoint=yes
changed length: result is the library's answer
breakpoint crc32 on the data again  hit=libz.so+0x12190 at_breakpoint=yes
changed pc: result is adler32's answer
breakpoint deflate  hit=libz.so+0x12d20 at_breakpoint=yes
breakpoint deflate  hit=libz.so+0x12d20 at_breakpoint=yes
breakpoint deflate  hit=libz.so+0x12d20 at_breakpoint=yes
step 110: libz.so -> libc.so  pc=libc.so+0x95610
step 124: libc.so -> libz.so  pc=libz.so+0xd5f0
step 153: libz.so -> libc.so  pc=libc.so+0x70d40
step 159: libc.so -> libz.so  pc=libz.so+0x1385c
after 6000 steps: 8 library changes, 32 instructions in libc
tracee done sum=0x13b56ea3f8e
detached tracee status=0
done mode=ptrace
"""

MARKER = "randomization: off\n"

SIBLING = """entry: breakpoint hit by the running thread: yes by_sibling=yes pc_at_breakpoint=yes in_libz=yes code=1  pc=libz.so+0x12190
entry: the thread runs on after the restore: yes
in loop: breakpoint hit by the running thread: yes by_sibling=yes pc_at_breakpoint=yes in_libz=yes code=1
in loop: the thread runs on after the restore: yes
done mode=sibling
"""


class GuestProgramTest(unittest.TestCase):
    """The program names a register that points into a mapping with the
    header it shares with the host (mapping_names.hpp, which
    test_aot_mapping_names.py holds to the digest's naming): the guest runs
    only in a guest, so what it uses is read from its source."""

    SOURCE = (pathlib.Path(__file__).resolve().parents[2] / "integration" / "aot" / "aot_guest_debug.cpp")

    def test_a_mapping_is_named_by_the_shared_formatter_and_not_by_a_table_of_its_own(self):
        text = self.SOURCE.read_text()
        self.assertIn('#include "mapping_names.hpp"', text)
        self.assertIn("aot_mapping_names::name(maps_text", text)
        self.assertNotIn("shapes[", text)
        self.assertNotIn("value & 0xfff", text)

    def test_a_maps_text_that_was_cut_short_fails_the_row(self):
        text = self.SOURCE.read_text()
        self.assertIn("FAIL the maps of process %d do not fit", text)
        self.assertIn("does not fit %zu bytes", text)


class RecordTest(unittest.TestCase):
    def test_good_records(self):
        rows.verify_record("ptrace", PTRACE.splitlines())
        rows.verify_record("sibling", SIBLING.splitlines())

    def test_ptrace_record_missing_a_part(self):
        for needle in ("changed pc: result is adler32's answer\n", "detached tracee status=0\n",
                       "done mode=ptrace\n", "tracee done sum=0x13b56ea3f8e\n"):
            with self.subTest(needle=needle):
                with self.assertRaises(rows.DebugFailure):
                    rows.verify_record("ptrace", PTRACE.replace(needle, "").splitlines())

    def test_ptrace_record_that_never_reached_libc(self):
        text = PTRACE.replace("8 library changes, 32 instructions", "1 library changes, 0 instructions")
        with self.assertRaises(rows.DebugFailure):
            rows.verify_record("ptrace", text.splitlines())

    def test_sibling_that_hit_once(self):
        text = SIBLING.replace("in loop: breakpoint hit by the running thread: yes by_sibling=yes",
                               "in loop: breakpoint hit by the running thread: NO by_sibling=yes")
        with self.assertRaises(rows.DebugFailure):
            rows.verify_record("sibling", text.splitlines())


class CompareTest(unittest.TestCase):
    def runs(self):
        return [rows.digest_tombstone(tombstone(x13=value, pid=2038 + 4 * n))
                for n, value in enumerate((1, 0, 1))]

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
        text = "\n".join(rows.digest_tombstone(tombstone()))
        self.assertIn("@libc.so+0x2854d", text)
        self.assertIn("@[anon:.bss]+0x2070", text)
        # An unnamed mapping is named by its size and protection, its place
        # among the mappings of that shape and how many there are, and the
        # offset in the whole mapping is kept.
        self.assertIn("@anon[0x100000,rw-]#0/1+0x1234", text)
        self.assertIn("@[anon:stack_and_tls:*]+0xf8c00", text)
        self.assertIn("pc  @libc.so+0x70d50", text)

    def test_a_library_loaded_elsewhere_is_the_same_run(self):
        off = [rows.digest_tombstone(tombstone(load=load, pid=2038 + n, stack_name=f"[anon:stack_and_tls:{2040 + n}]"))
               for n, load in enumerate((0x7ff1ae500000, 0x7ff1a1a00000, 0x7ff1b2c00000))]
        on = rows.digest_tombstone(tombstone(load=0x7ff1a8100000, pid=3000, stack_name="[anon:stack_and_tls:3002]"))
        compared, masked = rows.compare("crash-memcpy", off, on)
        self.assertLessEqual(masked, 3)
        self.assertGreater(compared, 10)

    def test_a_pointer_that_moves_only_with_the_component_fails(self):
        off = [rows.digest_tombstone(tombstone(load=load, pid=2038 + n))
               for n, load in enumerate((0x7ff1ae500000, 0x7ff1a1a00000, 0x7ff1b2c00000))]
        on = rows.digest_tombstone(tombstone(load=0x7ff1a8100000, pointer=0x28555, pid=3000))
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("crash-memcpy", off, on)
        self.assertIn("with it", str(caught.exception))

    def test_a_pointer_moved_inside_an_unnamed_mapping_fails(self):
        off = [rows.digest_tombstone(tombstone(load=load, pid=2038 + n))
               for n, load in enumerate((0x7ff1ae500000, 0x7ff1a1a00000, 0x7ff1b2c00000))]
        # The same page offset, a page further in: the old naming kept only
        # the offset in the page and called these the same pointer.
        for moved in (0x1234 + 0x1000, 0x1234 + 0x10000):
            on = rows.digest_tombstone(tombstone(load=0x7ff1a8100000, unnamed=moved, pid=3000))
            with self.assertRaises(rows.DebugFailure) as caught:
                rows.compare("crash-memcpy", off, on)
            self.assertIn("with it", str(caught.exception))
            self.assertIn("x6", str(caught.exception))

    def test_a_pointer_into_another_unnamed_mapping_fails(self):
        off = [rows.digest_tombstone(tombstone(load=load, pid=2038 + n))
               for n, load in enumerate((0x7ff1ae500000, 0x7ff1a1a00000, 0x7ff1b2c00000))]
        on = rows.digest_tombstone(tombstone(load=0x7ff1a8100000, unnamed_size=0x80000, pid=3000))
        with self.assertRaises(rows.DebugFailure):
            rows.compare("crash-memcpy", off, on)

    def test_a_pointer_moved_to_another_mapping_of_the_same_shape_fails(self):
        """Two unnamed mappings of one size and protection are two places: x6
        moved from one to the other, at the same offset, is a moved pointer."""
        off = [rows.digest_tombstone(tombstone(load=load, pid=2038 + n, second_unnamed=True))
               for n, load in enumerate((0x7ff1ae500000, 0x7ff1a1a00000, 0x7ff1b2c00000))]
        on = rows.digest_tombstone(tombstone(load=0x7ff1a8100000, pid=3000, second_unnamed=True,
                                             unnamed_target=1))
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("crash-memcpy", off, on)
        self.assertIn("with it", str(caught.exception))
        self.assertIn("x6", str(caught.exception))
        # The same pointer in the same one is the same pointer, at any load address.
        same = rows.digest_tombstone(tombstone(load=0x7ff1a8100000, pid=3000, second_unnamed=True))
        compared, _ = rows.compare("crash-memcpy", off, same)
        self.assertGreater(compared, 10)

    LOADS = (0x7ff1ae500000, 0x7ff1a1a00000, 0x7ff1b2c00000)

    def baselines(self, *layouts):
        """One run per layout (keywords of tombstone), at three load addresses."""
        return [rows.digest_tombstone(tombstone(load=load, pid=2038 + n, **layout))
                for n, (load, layout) in enumerate(zip(self.LOADS, layouts))]

    def ambiguity(self, off, **layout):
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("crash-memcpy", off, rows.digest_tombstone(
                tombstone(load=0x7ff1a8100000, pid=3000, **layout)))
        return str(caught.exception)

    def assert_ambiguous(self, off, target):
        message = self.ambiguity(off, lower_unnamed=True, unnamed_target=target)
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
        self.assert_ambiguous(self.baselines({}, {}, {}), 0)

    def test_a_wrong_pointer_into_a_mapping_that_appeared_is_not_accepted(self):
        self.assert_ambiguous(self.baselines({}, {}, {}), 2)

    # The runs without the component differ among themselves in the set: no
    # run with it can be compared by a place in it.
    def test_baselines_that_disagree_on_the_set_do_not_judge_a_correct_pointer(self):
        self.assert_ambiguous(self.baselines({}, {"lower_unnamed": True}, {}), 0)

    def test_baselines_that_disagree_on_the_set_do_not_let_a_wrong_pointer_pass(self):
        self.assert_ambiguous(self.baselines({}, {"lower_unnamed": True}, {}), 2)

    def test_runs_that_agree_on_the_set_are_compared_by_their_place_in_it(self):
        both = {"lower_unnamed": True}
        off = self.baselines(both, both, both)
        same = rows.digest_tombstone(tombstone(load=0x7ff1a8100000, pid=3000, lower_unnamed=True))
        compared, _ = rows.compare("crash-memcpy", off, same)
        self.assertGreater(compared, 10)
        message = self.ambiguity(off, lower_unnamed=True, unnamed_target=2)
        self.assertIn("with it", message)
        self.assertNotIn("ambiguous", message)

    def test_a_mapping_of_another_shape_leaves_the_place_alone(self):
        off = self.baselines({}, {}, {})
        on = rows.digest_tombstone(tombstone(load=0x7ff1a8100000, pid=3000, lower_unnamed=True,
                                             lower_size=0x80000))
        compared, _ = rows.compare("crash-memcpy", off, on)
        self.assertGreater(compared, 10)

    def test_same_shaped_unnamed_mappings_are_numbered_in_address_order(self):
        names = lambda target: next(line for line in rows.digest_tombstone(
            tombstone(second_unnamed=True, unnamed_target=target)) if " x6 " in line)
        self.assertIn("#0/2+0x1234", names(0))
        self.assertIn("#1/2+0x1234", names(1))
        # A mapping that is alone of its shape is the first of one.
        alone = next(line for line in rows.digest_tombstone(tombstone()) if " x6 " in line)
        self.assertIn("#0/1+0x1234", alone)

    def test_an_unnamed_mapping_at_another_address_is_the_same_pointer(self):
        off = [rows.digest_tombstone(tombstone(load=load, pid=2038 + n))
               for n, load in enumerate((0x7ff1ae500000, 0x7ff1a1a00000, 0x7ff1b2c00000))]
        on = rows.digest_tombstone(tombstone(load=0x7ff1a8100000, pid=3000))
        compared, _ = rows.compare("crash-memcpy", off, on)
        self.assertGreater(compared, 10)

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

    def test_only_a_few_fields_may_be_masked(self):
        runs = [[f"  regs crc32 " + " ".join(f"x{n:02d}=0x{n + 100 * value:x}" for n in range(rows.MAX_MASKED + 4))]
                for value in (1, 2, 3)]
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("ptrace", runs, runs[0])
        self.assertIn("process ids and random values", str(caught.exception))

    def test_the_code_around_the_pc_is_compared_whatever_the_dump_starts_at(self):
        one = rows.digest_tombstone(tombstone(load=0x7ff1ae500000))
        two = rows.digest_tombstone(tombstone(load=0x7ff1a1a00000))
        dump = lambda record: [line for line in record if line.startswith("=     +0x")]
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
        lines = ["  regs crc32 x00=0 x05=0x7262 sp=S+0x0 pc=libz.so+0x12190 nzcv=0x0"]
        off = [["  regs crc32 x00=0 x05=0x%x sp=S+0x0 pc=libz.so+0x12190 nzcv=0x0" % value] for value in (1, 2, 3)]
        compared, masked = rows.compare("ptrace", off, lines[0:1])
        self.assertEqual(masked, 1)
        self.assertGreaterEqual(compared, 5)
        moved = [["  regs crc32 x00=0 x05=0x1 sp=S+0x0 pc=libz.so+0x12194 nzcv=0x0"]]
        with self.assertRaises(rows.DebugFailure):
            rows.compare("ptrace", [["  regs crc32 x00=0 x05=0x1 sp=S+0x0 pc=libz.so+0x12190 nzcv=0x0"]] * 3, moved[0])
        changed = ["  regs crc32 x00=5 x05=0x1 sp=S+0x0 pc=libz.so+0x12190 nzcv=0x0"]
        with self.assertRaises(rows.DebugFailure):
            rows.compare("ptrace", [["  regs crc32 x00=0 x05=0x1 sp=S+0x0 pc=libz.so+0x12190 nzcv=0x0"]] * 3, changed)

    def test_a_symbolic_field_that_differs_between_the_engines_own_runs_is_not_masked(self):
        off = [["  regs crc32 x00=0 pc=libz.so+0x12190"], ["  regs crc32 x00=0 pc=libz.so+0x12194"],
               ["  regs crc32 x00=0 pc=libz.so+0x12190"]]
        with self.assertRaises(rows.DebugFailure):
            rows.compare("ptrace", off, ["  regs crc32 x00=0 pc=libz.so+0x12190"])

    def test_a_plain_line_must_be_equal_everywhere(self):
        lines = ["step 3  pc=libz.so+0x1219c"]
        rows.compare("ptrace", [lines] * 3, lines)
        with self.assertRaises(rows.DebugFailure):
            rows.compare("ptrace", [lines] * 3, ["step 3  pc=libz.so+0x121a0"])


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
        shell = self.shell([
            ("AOT_DEBUG_MODE=crash-memcpy", "randomization: off\nwarm sum=0x1\nrc=139\n"),
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
        self.assertTrue(any("SEGV_ACCERR" in line for line in record))
        self.assertTrue(any("logcat -b crash -c" in call for call in shell.calls))

    def test_a_crash_with_the_wrong_status_fails(self):
        shell = self.shell([("AOT_DEBUG_MODE=crash-memcpy", "rc=0\n")])
        with self.assertRaises(rows.DebugFailure):
            rows.run_mode(shell, "crash-memcpy")

    def test_a_mode_that_reports_a_failure_fails(self):
        shell = self.shell([("AOT_DEBUG_MODE=sibling", MARKER + SIBLING.replace("in_libz=yes code=1\n", "in_libz=NO code=1\n", 1) + "rc=0\n")])
        with self.assertRaises(rows.DebugFailure):
            rows.run_mode(shell, "sibling")

    def test_a_good_debug_record_is_returned_without_its_status(self):
        shell = self.shell([("AOT_DEBUG_MODE=sibling", MARKER + SIBLING + "rc=0\n")])
        record = rows.run_mode(shell, "sibling")
        self.assertEqual(record[0], "randomization: off")
        self.assertEqual(record[-1], "done mode=sibling")

    def test_a_run_whose_randomization_was_not_turned_off_is_no_run(self):
        for output in (SIBLING + "rc=0\n", MARKER * 2 + SIBLING + "rc=0\n",
                       MARKER + "FAIL randomization is still on after the program ran again\n" + SIBLING + "rc=0\n"):
            with self.subTest(output=output[:60]):
                with self.assertRaises(rows.DebugFailure):
                    rows.run_mode(self.shell([("AOT_DEBUG_MODE=sibling", output)]), "sibling")


if __name__ == "__main__":
    unittest.main()
