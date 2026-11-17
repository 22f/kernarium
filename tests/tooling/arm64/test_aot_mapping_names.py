#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The guest debug program's naming of a mapping, against the tombstone
digest's.

aot_guest_debug.cpp prints a register that points into a mapping by the name
tests/integration/aot/mapping_names.hpp gives it; debug_rows.py:name_value
gives the same register the same name in a tombstone. The runner compares the
two kinds of record only against runs of their own kind, but both must say the
same of a layout. The guest program runs only in a guest, so this row builds
the header into a host program (aot_mapping_names_harness.cpp) and gives both
formatters the same layouts, however many different shapes they hold.
"""

__import__("sys").dont_write_bytecode = True

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
AOT = HERE.parents[1] / "integration" / "aot"
sys.path.insert(0, str(AOT))
import debug_rows as rows  # noqa: E402

PAGE = 0x1000
Mapping = tuple[int, int, int, str, str]  # low, high, file offset, name, protection


def maps_text(mappings: list[Mapping]) -> str:
    """A layout as the kernel writes /proc/<pid>/maps: a name is separated
    from the inode by padding, an unnamed mapping's line ends at the inode."""
    lines = []
    for low, high, offset, name, protection in mappings:
        line = f"{low:012x}-{high:012x} {protection}p {offset:08x} 00:00 0"
        lines.append(line + (" " * 26 + name if name else ""))
    return "\n".join(lines) + "\n"


class MappingNamesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        compiler = os.environ.get("UML_CXX") or shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
        if compiler is None:
            raise unittest.SkipTest("no C++ compiler")
        cls.directory = tempfile.mkdtemp(prefix="aot-mapping-names-")
        cls.harness = pathlib.Path(cls.directory) / "harness"
        subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(AOT),
                        str(HERE / "aot_mapping_names_harness.cpp"), "-o", str(cls.harness)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.directory, ignore_errors=True)

    def native(self, mappings: list[Mapping], values: list[int]) -> list[str]:
        path = pathlib.Path(self.directory) / "maps"
        path.write_text(maps_text(mappings))
        done = subprocess.run([str(self.harness), str(path)] + [f"{value:x}" for value in values],
                              check=True, capture_output=True, text=True, timeout=60)
        return done.stdout.splitlines()

    def digest(self, mappings: list[Mapping], values: list[int]) -> list[str]:
        """The tombstone digest's names; it writes `@` before a name and
        leaves a value that points into nothing as it is."""
        named = [rows.name_value(value, mappings) for value in values]
        return [name[1:] if name else "none" for name in named]

    def agree(self, mappings: list[Mapping], values: list[int]) -> list[str]:
        native = self.native(mappings, values)
        self.assertEqual(native, self.digest(mappings, values))
        return native

    @staticmethod
    def inside(mappings: list[Mapping]) -> list[int]:
        """The first byte, a byte in, and the last byte of every mapping."""
        return [value for low, high, *_ in mappings for value in (low, low + 0x10, high - 1)]

    def test_both_formatters_name_a_mixed_layout_alike(self):
        base = 0x7ff1ae500000
        mappings: list[Mapping] = [
            (0x400000, 0x401000, 0, "/system/bin/toybox", "r-x"),
            (base, base + 0xb0000, 0x6000, "/apex/com.android.runtime/lib64/bionic/libc.so", "r-x"),
            (base + 0xb0000, base + 0xb3000, 0, "[anon:.bss]", "rw-"),
            (base + 0xc0000, base + 0x1c0000, 0, "", "rw-"),
            (base + 0x200000, base + 0x300000, 0, "", "rw-"),
            (base + 0x300000, base + 0x340000, 0, "", "rw-"),
            (base + 0x340000, base + 0x440000, 0, "", "r--"),
            (base + 0x440000, base + 0x540000, 0, "", "rw-"),
        ]
        values = self.inside(mappings) + [0x1000, base + 0x1c0000 + 0x10, base + 0x540000]
        names = self.agree(mappings, values)
        self.assertEqual(names[0], "toybox+0x0")
        self.assertEqual(names[3], "libc.so+0x6000")
        self.assertEqual(names[-2:], ["none", "none"])
        # Three unnamed 1 MiB read-write mappings, told apart by their place
        # among the three.
        place = dict(zip(values, names))
        self.assertEqual([place[base + low + 0x10] for low in (0xc0000, 0x200000, 0x440000)],
                         [f"anon[0x100000,rw-]#{ordinal}/3+0x10" for ordinal in range(3)])

    def test_both_formatters_name_more_than_128_shapes_alike(self):
        """A layout of 129 distinct shapes, and then two mappings of the last:
        the formatter has no table of shapes to fill, so the pointers into
        the two mappings of the last shape are told apart."""
        base = 0x7ff100000000
        mappings: list[Mapping] = []
        at = base
        for pages in range(1, 130):
            mappings.append((at, at + pages * PAGE, 0, "", "rw-"))
            at += pages * PAGE + PAGE
        low, high = at, at + 129 * PAGE
        mappings.append((low, high, 0, "", "rw-"))
        names = self.agree(mappings, self.inside(mappings))
        first_last, second_last = (self.agree(mappings, [value])[0] for value in (mappings[128][0] + 0x10, low + 0x10))
        self.assertEqual(first_last, "anon[0x81000,rw-]#0/2+0x10")
        self.assertEqual(second_last, "anon[0x81000,rw-]#1/2+0x10")
        self.assertEqual(len(set(names)), len(names))

    def test_a_name_that_does_not_fit_is_refused(self):
        base = 0x7ff100000000
        mappings: list[Mapping] = [(base, base + PAGE, 0, "[anon:" + "x" * 400 + "]", "rw-"),
                                   (base + PAGE, base + 2 * PAGE, 0, "[anon:short]", "rw-")]
        self.assertEqual(self.native(mappings, [base + 0x10, base + PAGE + 0x10]),
                         ["overflow", "[anon:short]+0x10"])


GUEST_PROBE = r"""
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "mapping_names.hpp"
static std::string incoming;
static size_t cursor;
static int fake_open(const char *, int) { cursor = 0; return 1; }
static int fake_close(int) { return 0; }
static long fake_read(int, void *out, size_t capacity) {
    const size_t amount = std::min(capacity, incoming.size() - cursor);
    std::memcpy(out, incoming.data() + cursor, amount);
    cursor += amount;
    return static_cast<long>(amount);
}
#define open fake_open
#define close fake_close
#define read fake_read
%(functions)s
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const std::string which = argv[1];
    uint64_t value = 0x100010;
    if (which == "full") {
        incoming.assign((1 << 17) - 1, 'x');
    } else {
        const std::string name = which == "short" ? "[anon:short]" : "[anon:" + std::string(600, 'x') + "]";
        value = which == "second" ? 0x100020 : 0x100010;
        incoming = "00100000-00101000 rw-p 00000000 00:00 0 " + name + "\n";
    }
    // What print_regs does with one register that points into a mapping.
    load_maps(123);
    dprintf(1, "  regs crc32");
    for (int i = 0; i < 31; ++i) {
        char label[8];
        std::snprintf(label, sizeof(label), "x%%02d", i);
        if (i != 6 || !print_mapped(label, value)) {
            dprintf(1, " %%s=0", label);
        }
    }
    dprintf(1, " sp=S+0x0 pc=libz.so+0x12190 nzcv=0x0\n");
    return 0;
}
"""


class GuestOutputTest(unittest.TestCase):
    """What the guest debug program prints when a name or the maps text does
    not fit its buffers, through the runner's own judgement of a record
    (debug_rows.run_mode): the functions are the program's own, taken from its
    source and built on the host with the file reads replaced."""

    @classmethod
    def setUpClass(cls) -> None:
        compiler = os.environ.get("UML_CXX") or shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
        if compiler is None:
            raise unittest.SkipTest("no C++ compiler")
        source = (AOT / "aot_guest_debug.cpp").read_text()
        functions = source[source.index("char maps_text[1 << 17];"):source.index("void print_value(")]
        cls.directory = tempfile.mkdtemp(prefix="aot-guest-output-")
        program = pathlib.Path(cls.directory) / "probe.cpp"
        program.write_text(GUEST_PROBE % {"functions": functions})
        cls.probe = pathlib.Path(cls.directory) / "probe"
        subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-I", str(AOT), str(program), "-o", str(cls.probe)],
                       check=True, capture_output=True, text=True)
        sys.path.insert(0, str(HERE))
        import test_aot_debug_rows as fixture  # noqa: E402
        cls.fixture = fixture

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.directory, ignore_errors=True)

    def output(self, which: str) -> str:
        return subprocess.run([str(self.probe), which], check=True, capture_output=True, text=True,
                              timeout=30).stdout

    def judged(self, which: str) -> list[str]:
        fixture = self.fixture
        text = fixture.MARKER + fixture.PTRACE.replace(fixture.regs_line(), self.output(which).rstrip("\n")) + "rc=0\n"
        return rows.run_mode(lambda *_args: text, "ptrace")

    def test_a_name_that_fits_is_a_record(self):
        self.assertEqual(self.judged("short")[-1], "done mode=ptrace")

    def test_a_name_that_does_not_fit_fails_the_row(self):
        for which in ("first", "second"):
            with self.subTest(which=which):
                with self.assertRaises(rows.DebugFailure) as caught:
                    self.judged(which)
                self.assertIn("FAIL", str(caught.exception))

    def test_a_failure_is_a_line_of_its_own(self):
        for which in ("first", "second", "full"):
            with self.subTest(which=which):
                output = self.output(which)
                failures = [line for line in output.splitlines() if "FAIL" in line]
                self.assertTrue(failures, output)
                self.assertTrue(all(line.startswith("FAIL") for line in failures), output)

    def test_a_maps_text_that_does_not_fit_fails_the_row(self):
        with self.assertRaises(rows.DebugFailure) as caught:
            self.judged("full")
        self.assertIn("FAIL", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
