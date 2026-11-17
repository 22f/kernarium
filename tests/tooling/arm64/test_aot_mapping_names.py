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
        # Three unnamed 1 MiB read-write mappings, told apart by their place.
        place = dict(zip(values, names))
        self.assertNotEqual(place[base + 0xc0000 + 0x10], place[base + 0x200000 + 0x10])
        self.assertNotEqual(place[base + 0x200000 + 0x10], place[base + 0x440000 + 0x10])

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
        self.assertNotEqual(first_last, second_last)
        self.assertEqual(len(set(names)), len(names))

    def test_a_name_that_does_not_fit_is_refused(self):
        base = 0x7ff100000000
        mappings: list[Mapping] = [(base, base + PAGE, 0, "[anon:" + "x" * 400 + "]", "rw-"),
                                   (base + PAGE, base + 2 * PAGE, 0, "[anon:short]", "rw-")]
        self.assertEqual(self.native(mappings, [base + 0x10, base + PAGE + 0x10]),
                         ["overflow", "[anon:short]+0x10"])


if __name__ == "__main__":
    unittest.main()
