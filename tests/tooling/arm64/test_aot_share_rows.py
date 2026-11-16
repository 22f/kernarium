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
            rows.run_mode(shell_of(record("fork", 16, tail="\nround 0: the pressure took effect: NO")), "fork")

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

    def test_informational_lines_are_not_compared(self):
        off = self.lines(tail="\n# round 0: libz.so pages cached 30 before, 12 after")
        on = self.lines(tail="\n# round 0: libz.so pages cached 30 before, 25 after")
        self.assertEqual(rows.compare("fork", off, on), 17)
        self.assertEqual(len(rows.informational(on)), 1)

    def test_a_shorter_record_fails(self):
        with self.assertRaises(rows.ShareFailure):
            rows.compare("fork", self.lines(), self.lines()[:-1])


class CountsTest(unittest.TestCase):
    def test_the_fork_mode_needs_reused_verdicts(self):
        rows.check_counts("fork", {"shared": rows.FORK_MIN_SHARED, "verified": 3}, None)
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("fork", {"shared": rows.FORK_MIN_SHARED - 1}, None)

    def test_the_copy_mode_is_refused_for_lack_of_the_fact_and_shares_little(self):
        rows.check_counts("copy", {"shared": 5, "verified": 0, "mutable": 20}, 100)
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("copy", {"shared": 60, "verified": 0, "mutable": 20}, 100)
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("copy", {"shared": 0, "verified": 20, "mutable": 11}, 100)
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("copy", {"shared": 0, "mutable": 20}, None)

    def test_the_fork_mode_refuses_nothing(self):
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("fork", {"shared": 176, "mutable": 1}, None)

    def test_a_copied_page_is_refused_in_every_child_that_ran_it(self):
        # The modes that copy a page: the children that ran the copy are
        # refused it, and the library's other pages share.
        for mode, writers in rows.COW_WRITERS.items():
            good = {"shared": 100, "verified": 2, "mutable": writers}
            rows.check_counts(mode, good, 176)
            with self.subTest(mode=mode, what="refused too few times"):
                with self.assertRaises(rows.ShareFailure):
                    rows.check_counts(mode, dict(good, mutable=writers - 1), 176)
            with self.subTest(mode=mode, what="a copied page shared"):
                with self.assertRaises(rows.ShareFailure):
                    rows.check_counts(mode, dict(good, mutable=0), 176)
            with self.subTest(mode=mode, what="nothing shared"):
                with self.assertRaises(rows.ShareFailure):
                    rows.check_counts(mode, dict(good, shared=0), 176)

    def test_a_protection_without_a_store_refuses_nothing(self):
        rows.check_counts("mprotect", {"shared": 40, "mutable": 0}, 176)
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("mprotect", {"shared": 40, "mutable": 1}, 176)
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("mprotect", {"shared": 2, "mutable": 0}, 176)

    def test_pressure_still_shares(self):
        rows.check_counts("pressure", {"shared": 200, "verified": 20}, 176)
        for bad in ({"shared": 3}, {"shared": 200, "mutable": 1}, {}):
            with self.assertRaises(rows.ShareFailure):
                rows.check_counts("pressure", bad, 176)


EVICT = ["# control: 256 of 256 pages cached before, 0 after",
         "# libz.so: 0 faults read pages back before the eviction, 0 after"]


class EvictTest(unittest.TestCase):
    def test_nothing_evicted_and_everything_shared(self):
        text = rows.check_counts("evict", {"shared": 30, "verified": 2}, 176, EVICT)
        self.assertIn("0 faults read libz.so's pages back", text)

    def test_pages_that_were_evicted_are_read_back_and_the_rest_shared(self):
        record = [EVICT[0], "# libz.so: 0 faults read pages back before the eviction, 3 after"]
        text = rows.check_counts("evict", {"shared": 30, "verified": 5}, 176, record)
        self.assertIn("3 faults read libz.so's pages back", text)

    def test_a_control_that_kept_its_pages_cannot_observe_eviction(self):
        for control in ("# control: 256 of 256 pages cached before, 3 after",
                        "# control: 20 of 256 pages cached before, 0 after"):
            with self.assertRaises(rows.ShareFailure):
                rows.check_counts("evict", {"shared": 30}, 176, [control, EVICT[1]])

    def test_a_record_without_its_counts_fails(self):
        for record in ([], EVICT[:1], EVICT[1:]):
            with self.assertRaises(rows.ShareFailure):
                rows.check_counts("evict", {"shared": 30}, 176, record)

    def test_nothing_shared_fails(self):
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("evict", {"shared": 1, "verified": 30}, 176, EVICT)

    def test_a_refused_page_of_the_read_only_library_fails(self):
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("evict", {"shared": 30, "mutable": 1}, 176, EVICT)


RECLAIM = ["# reclaim: libz.so 33 pages cached before the eviction, 2 after; 3 faults read them back"]


class ReclaimTest(unittest.TestCase):
    """The study's own row: the kernel pages out what a stock one spares."""

    def test_the_study_mode_is_run_only_on_request(self):
        self.assertEqual(rows.modes(False), rows.MODES)
        self.assertEqual(rows.modes(True), rows.MODES + ("reclaim",))

    def test_evicted_pages_are_hashed_again_and_then_shared(self):
        text = rows.check_counts("reclaim", {"shared": 8, "verified": 11}, 176, RECLAIM)
        self.assertIn("libz.so lost 31 of 33 pages, read back in 3 faults", text)

    def test_a_guest_that_spares_the_pages_fails(self):
        for after in (33, 20):
            record = [f"# reclaim: libz.so 33 pages cached before the eviction, {after} after; 1 faults read them back"]
            with self.assertRaises(rows.ShareFailure):
                rows.check_counts("reclaim", {"shared": 8, "verified": 11}, 176, record)

    def test_an_engine_that_answers_for_an_evicted_frame_fails(self):
        for delta in ({"shared": 19, "verified": 0}, {"shared": 19, "verified": 3}):
            with self.assertRaises(rows.ShareFailure):
                rows.check_counts("reclaim", delta, 176, RECLAIM)

    def test_a_library_the_guest_had_not_cached_cannot_observe_eviction(self):
        record = ["# reclaim: libz.so 4 pages cached before the eviction, 0 after; 1 faults read them back"]
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("reclaim", {"shared": 8, "verified": 11}, 176, record)

    def test_nothing_shared_after_the_first_verification_fails(self):
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("reclaim", {"shared": 0, "verified": 11}, 176, RECLAIM)

    def test_evicted_pages_that_take_no_fault_to_read_back_fail(self):
        record = ["# reclaim: libz.so 33 pages cached before the eviction, 2 after; 0 faults read them back"]
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("reclaim", {"shared": 8, "verified": 11}, 176, record)

    def test_a_refused_page_or_a_missing_record_fails(self):
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("reclaim", {"shared": 8, "verified": 11, "mutable": 1}, 176, RECLAIM)
        with self.assertRaises(rows.ShareFailure):
            rows.check_counts("reclaim", {"shared": 8, "verified": 11}, 176, [])

    def test_a_record_of_six_children_is_the_expected_shape(self):
        out = record("reclaim", 6, tail="\n" + RECLAIM[0])
        self.assertEqual(len(rows.run_mode(shell_of(out), "reclaim")), 8)


if __name__ == "__main__":
    unittest.main()
