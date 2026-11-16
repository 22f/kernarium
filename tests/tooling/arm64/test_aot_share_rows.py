#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The shared-verification rows of the AOT guest runner, on scripted records.

tests/integration/aot/share_rows.py runs a mode of the share program, checks
that every child reported, compares the record without the component and with
it, and judges the framework's counters around a mode. These rows give it a
good run and every kind of bad one.
"""

__import__("sys").dont_write_bytecode = True

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "integration" / "aot"))
import share_rows as rows  # noqa: E402


def record(mode: str, children: int, sums: dict[int, int] | None = None, tail: str = "") -> str:
    lines = [f"sequential child {i} sum={(sums or {}).get(i, 0x1000 + i):#x}" for i in range(children)]
    lines.append(f"done mode={mode}")
    return "\n".join(lines) + tail + "\nrc=0\n"


def shell_of(output: str):
    return lambda text, seconds=60: output


class RunTest(unittest.TestCase):
    def test_a_complete_run_gives_its_record(self):
        for mode, count in rows.EXPECTED_CHILDREN.items():
            lines = rows.run_mode(shell_of(record(mode, count)), mode)
            self.assertEqual(lines[-1], f"done mode={mode}")
            self.assertEqual(len(lines), count + 1)

    def test_a_missing_child_fails(self):
        with self.assertRaises(rows.ShareFailure):
            rows.run_mode(shell_of(record("fork", 15)), "fork")

    def test_a_failing_exit_status_fails(self):
        with self.assertRaises(rows.ShareFailure):
            rows.run_mode(shell_of(record("fork", 16).replace("rc=0", "rc=139")), "fork")

    def test_a_fail_line_fails(self):
        with self.assertRaises(rows.ShareFailure):
            rows.run_mode(shell_of(record("fork", 16, tail="\nFAIL fork")), "fork")

    def test_a_row_that_answers_no_fails(self):
        with self.assertRaises(rows.ShareFailure):
            rows.run_mode(shell_of(record("evict", 18, tail="\nround 0: the page cache dropped some of libz.so: NO")),
                          "evict")

    def test_no_closing_record_fails(self):
        with self.assertRaises(rows.ShareFailure):
            rows.run_mode(shell_of(record("fork", 16).replace("done mode=fork", "")), "fork")

    def test_no_exit_status_fails(self):
        with self.assertRaises(rows.ShareFailure):
            rows.run_mode(shell_of(record("fork", 16).replace("rc=0\n", "")), "fork")


class CompareTest(unittest.TestCase):
    def lines(self, **kwargs):
        return rows.run_mode(shell_of(record("fork", 16, **kwargs)), "fork")

    def test_equal_records_compare(self):
        self.assertEqual(rows.compare("fork", self.lines(), self.lines()), 17)

    def test_one_sum_that_differs_fails(self):
        with self.assertRaises(rows.ShareFailure) as caught:
            rows.compare("fork", self.lines(), self.lines(sums={7: 0x99}))
        self.assertIn("line 8", str(caught.exception))

    def test_a_shorter_record_fails(self):
        with self.assertRaises(rows.ShareFailure):
            rows.compare("fork", self.lines(), self.lines()[:-1])


class CountsTest(unittest.TestCase):
    def test_the_fork_mode_needs_reused_verdicts(self):
        rows.check_counts("fork", {"shared_hits": rows.FORK_MIN_SHARED, "verifications": 3}, None)
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("fork", {"shared_hits": rows.FORK_MIN_SHARED - 1}, None)

    def test_the_copy_mode_shares_a_fraction_of_the_fork_mode_and_hashes(self):
        rows.check_counts("copy", {"shared_hits": 5, "verifications": 20}, 100)
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("copy", {"shared_hits": 60, "verifications": 20}, 100)
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("copy", {"shared_hits": 0, "verifications": 11}, 100)
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("copy", {"shared_hits": 0, "verifications": 20}, None)

    def test_the_other_modes_only_report(self):
        for mode in ("cow", "mprotect", "evict"):
            self.assertIn(mode, rows.check_counts(mode, {}, 100))


if __name__ == "__main__":
    unittest.main()
