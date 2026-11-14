#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The thermal runner's reading of the guest.

run_android_thermal.py plays the host application against a booted release and
reads `dumpsys thermalservice`, `dumpsys display`, `dumpsys SurfaceFlinger`,
`cmd thermalservice headroom` and the launcher's frame statistics. The texts
below have the layout those commands print (framework classes
ThermalManagerService, SkinThermalStatusObserver and the display manager's vote
storage), so the runner's parsers and its records run without a guest. The
records are the golden frames of tests/unit/andarium/fixtures.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
import math
from pathlib import Path
import sys
import unittest

TESTS = Path(__file__).resolve().parents[2]
SCRIPT = TESTS / "integration/android/run_android_thermal.py"
SPEC = importlib.util.spec_from_file_location("run_android_thermal", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
# Dataclasses resolve their annotations through sys.modules.
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)
FIXTURE = TESTS / "unit/andarium/fixtures/thermal-golden-frames.txt"

THERMAL_DUMP = """\
IsStatusOverride: false
ThermalEventListeners:
	callbacks: 4
ThermalStatusListeners:
	callbacks: 2
Thermal Status: 3
Cached temperatures:
	Temperature{mValue=41.5, mType=3, mName=skin, mStatus=3}
HAL Ready: true
HAL connection:
	ThermalHAL AIDL 3  connected: yes
Current temperatures from HAL:
	Temperature{mValue=41.5, mType=3, mName=skin, mStatus=3}
Current cooling devices from HAL:
Temperature static thresholds from HAL:
	TemperatureThreshold{mType=3, mName=skin, mHotThrottlingThresholds=[NaN, 34.0, 37.0, 40.0, 43.0, 46.0, 49.0], mColdThrottlingThresholds=[NaN, NaN, NaN, NaN, NaN, NaN, NaN]}
Temperature headroom thresholds:
[NaN, 0.8, 0.9, 1.0, 1.1, 1.2, 1.3]
"""

DISPLAY_DUMP_CAPPED = """\
  SkinThermalStatusObserver:
    mStatus: 3
    mThermalThrottlingByDisplay: {0={3=(0.0 30.0)}}
    mThermalWorkDurationsByDisplay: {}
  mVotesByDisplay:
    0:
      PRIORITY_SKIN_TEMPERATURE -> RenderVote{ RefreshRateVote{  mMinRefreshRate=0.0, mMaxRefreshRate=30.0 } }
      PRIORITY_USER_SETTING_MIN_RENDER_FRAME_RATE -> RenderVote{ RefreshRateVote{  mMinRefreshRate=0.0, mMaxRefreshRate=Infinity } }
"""

DISPLAY_DUMP_IDLE = """\
  SkinThermalStatusObserver:
    mStatus: 0
    mThermalThrottlingByDisplay: {0={3=(0.0 30.0)}}
  mVotesByDisplay:
    0:
      PRIORITY_USER_SETTING_MIN_RENDER_FRAME_RATE -> RenderVote{ RefreshRateVote{  mMinRefreshRate=0.0, mMaxRefreshRate=Infinity } }
"""

DISPLAY_DUMP_NO_MAP = """\
  SkinThermalStatusObserver:
    mStatus: 3
    mThermalThrottlingByDisplay: {0={}}
"""


class ThermalDumpTests(unittest.TestCase):
    def test_the_state_and_the_one_sensor(self) -> None:
        dump = runner.parse_thermal_dump(THERMAL_DUMP)
        self.assertEqual(dump.status, 3)
        self.assertEqual(dump.cached, [("skin", 3, 3, 41.5)])
        self.assertEqual(dump.hal, [("skin", 3, 3, 41.5)])
        self.assertTrue(dump.hal_ready)
        self.assertIn("AIDL 3", dump.hal_line)
        self.assertEqual(dump.thresholds[0][:2], ("skin", 3))
        self.assertTrue(runner.same_numbers(dump.thresholds[0][2], runner.HOT_THRESHOLDS))
        self.assertTrue(runner.same_numbers(dump.headroom_thresholds, runner.HEADROOM_THRESHOLDS))

    def test_a_second_sensor_is_seen(self) -> None:
        text = THERMAL_DUMP.replace(
            "Current cooling devices from HAL:",
            "\tTemperature{mValue=30.2, mType=2, mName=battery, mStatus=0}\nCurrent cooling devices from HAL:")
        self.assertEqual([entry[:2] for entry in runner.parse_thermal_dump(text).hal],
                         [("skin", 3), ("battery", 2)])

    def test_thresholds_that_differ_are_not_the_expected_ones(self) -> None:
        wrong = THERMAL_DUMP.replace("34.0, 37.0", "33.0, 37.0")
        dump = runner.parse_thermal_dump(wrong)
        self.assertFalse(runner.same_numbers(dump.thresholds[0][2], runner.HOT_THRESHOLDS))

    def test_no_dump_reads_as_nothing(self) -> None:
        dump = runner.parse_thermal_dump("")
        self.assertIsNone(dump.status)
        self.assertEqual((dump.cached, dump.hal, dump.thresholds), ([], [], []))
        self.assertFalse(dump.hal_ready)

    def test_headroom(self) -> None:
        self.assertAlmostEqual(runner.parse_headroom("Headroom in 0 seconds: 0.85\n"), 0.85)
        self.assertIsNone(runner.parse_headroom("Headroom in 0 seconds: NaN\n"))
        self.assertIsNone(runner.parse_headroom("Error: thermal HAL is not ready\n"))


class DisplayDumpTests(unittest.TestCase):
    def test_the_cap(self) -> None:
        display = runner.parse_display_dump(DISPLAY_DUMP_CAPPED)
        self.assertEqual(display.status, 3)
        self.assertTrue(display.map_has_severe_cap)
        self.assertTrue(display.skin_vote)
        self.assertEqual(display.skin_vote_maximum, 30.0)

    def test_no_vote_at_none(self) -> None:
        display = runner.parse_display_dump(DISPLAY_DUMP_IDLE)
        self.assertEqual(display.status, 0)
        self.assertTrue(display.map_has_severe_cap)
        self.assertFalse(display.skin_vote)
        self.assertIsNone(display.skin_vote_maximum)

    def test_an_empty_map_is_no_cap(self) -> None:
        display = runner.parse_display_dump(DISPLAY_DUMP_NO_MAP)
        self.assertFalse(display.map_has_severe_cap)
        self.assertFalse(display.skin_vote)

    def test_a_map_without_the_severe_point_is_no_cap(self) -> None:
        text = DISPLAY_DUMP_IDLE.replace("{3=(0.0 30.0)}",
                                         "{3=(0.0 60.0)}")
        self.assertFalse(runner.parse_display_dump(text).map_has_severe_cap)

    def test_render_rates(self) -> None:
        text = "  x\n    renderRate=30.00 Hz\n    activeMode={vsyncRate=59.99 Hz}\n    renderRate=59.99 Hz\n"
        self.assertEqual(runner.parse_render_rates(text), [30.0, 59.99])
        self.assertEqual(runner.parse_render_rates("no rate"), [])


class FrameStatisticsTests(unittest.TestCase):
    def test_the_highest_of_the_busy_windows(self) -> None:
        log = "\n".join(
            f"Native display frames: guest {guest}/s presented {presented}/s dropped 0.0/s (0.0%) cpu-fallback 0.0/s"
            for guest, presented in ((1.1, 1.1), (30.2, 30.0), (29.9, 29.9), (30.0, 31.0), (0.0, 0.0)))
        self.assertEqual(runner.presented_rate(log), 31.0)

    def test_the_windows_at_the_edges_of_a_run_do_not_lower_its_rate(self) -> None:
        log = "\n".join(
            f"Native display frames: guest {rate}/s presented {rate}/s dropped 0.0/s (0.0%) cpu-fallback 0.0/s"
            for rate in (0.0, 18.8, 30.6))
        self.assertEqual(runner.presented_rate(log), 30.6)

    def test_an_idle_log_has_no_rate(self) -> None:
        self.assertIsNone(runner.presented_rate("Native display frames: guest 0.0/s presented 0.0/s dropped 0.0/s"))
        self.assertIsNone(runner.presented_rate(""))


class RecordTests(unittest.TestCase):
    def fixture(self) -> dict[str, bytes]:
        frames = {}
        for line in FIXTURE.read_text().splitlines():
            if line and not line.startswith("#"):
                name, hexbytes = line.split(":")
                frames[name] = bytes.fromhex(hexbytes.replace(" ", ""))
        return frames

    def test_the_records_are_the_golden_frames(self) -> None:
        frames = self.fixture()
        self.assertEqual(runner.record(0), frames["none"])
        self.assertEqual(runner.record(1), frames["light"])
        self.assertEqual(runner.record(3), frames["severe"])
        self.assertEqual(runner.record(4), frames["critical"])

    def test_the_refused_records_are_the_golden_rejects(self) -> None:
        frames = self.fixture()
        self.assertEqual(runner.REFUSED["emergency"], frames["reject-emergency"])
        self.assertEqual(runner.REFUSED["shutdown"], frames["reject-shutdown"])
        self.assertEqual(runner.REFUSED["magic"], frames["reject-magic"])
        self.assertEqual(runner.REFUSED["reserved"], frames["reject-reserved"])

    def test_the_levels_agree_with_the_headroom_rule(self) -> None:
        # ThermalManagerService normalizes a temperature against the SEVERE threshold (40): 1.0 there, 30
        # degrees for one unit.
        severe = runner.HOT_THRESHOLDS[3]
        for severity, (temperature, headroom) in runner.LEVELS.items():
            self.assertAlmostEqual((temperature - (severe - 30.0)) / 30.0, headroom, places=5)
            self.assertTrue(math.isnan(runner.HOT_THRESHOLDS[0]))
            low = runner.HOT_THRESHOLDS[severity] if severity else -math.inf
            self.assertLessEqual(low, temperature)
            self.assertLess(temperature, runner.HOT_THRESHOLDS[severity + 1])


if __name__ == "__main__":
    unittest.main()
