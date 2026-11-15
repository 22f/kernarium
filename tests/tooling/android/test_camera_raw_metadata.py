#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A RAW capable V4L2 camera states the static metadata the camera device info requires of one.

The camera HAL (route p0030) builds each V4L2 camera's characteristics from a template, emu_camera_v4l2.json,
and V4l2Characteristics.cpp's Specialize: RAW is declared only for a camera whose description has colour data
(a lens whose still has taught the host). The stock device info, EmulatedCameraDeviceInfo, then checks a RAW
capable camera at boot and refuses to create the ones that fail; cameraserver never lists a camera the HAL
refused, so a lens with colour data was gone from the guest after its first restart. The checks of the stock
device info that depend on RAW (EmulatedCameraDeviceInfo.cpp, InitializeStatisticsDefaults, InitializeSensorDefaults
and InitializeLensDefaults, and utils/HWLUtils.cpp for the sensor characteristics) are modelled below against the two
files as the series leaves them. The stock sources are not in this repository, so each assertion names the message of
the check it stands for; a guest still has the last word (the camera runner's restart mode).

The two files are rebuilt from the series' patches (git apply), and nothing here boots anything.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

REPOSITORY = Path(__file__).resolve().parents[3]
SERIES = REPOSITORY / "android/lineage-24/patches/projects/p0030/series"
HWL = "devices/EmulatedCamera/hwl"
SPECIALIZER = f"{HWL}/V4l2Characteristics.cpp"
TEMPLATE = f"{HWL}/configs/emu_camera_v4l2.json"

# The tags of system/media/camera/include/system/camera_metadata_tags.h that the checks name. The template lists them
# by number, and the numbers below are the ones its lists hold for these tags (a template that lists the result key
# of the lens shading map under another number would not be asserting this test's tag).
LENS_SHADING_MAP = 0x11000B
LENS_SHADING_MAP_MODE = 0x110010
HOT_PIXEL_MAP_MODE = 0x110003
SENSOR_NEUTRAL_COLOR_POINT = 0xE0012
SENSOR_NOISE_PROFILE = 0xE0013
SENSOR_GREEN_SPLIT = 0xE0016
INFO_AVAILABLE_HOT_PIXEL_MAP_MODES = 0x120006
INFO_AVAILABLE_LENS_SHADING_MAP_MODES = 0x120007

OFF, ON = "ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF", "ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_ON"


def rebuilt_files() -> dict[str, str]:
    """The specializer and the template as the whole series leaves them: every patch that touches either is applied,
    in the order of series.tsv, to an empty repository, limited to those two paths."""
    names = [line.split("\t", 1)[1] for line in (SERIES / "series.tsv").read_text(encoding="utf-8").splitlines()
             if line.startswith("patch\t")]
    with tempfile.TemporaryDirectory(prefix="camera-raw-") as directory:
        subprocess.run(["git", "init", "-q", directory], check=True)
        for name in names:
            text = (SERIES / name).read_text(encoding="utf-8")
            if not any(f"diff --git a/{path} " in text for path in (SPECIALIZER, TEMPLATE)):
                continue
            subprocess.run(["git", "-C", directory, "apply", f"--include={SPECIALIZER}", f"--include={TEMPLATE}",
                            str(SERIES / name)], check=True)
        return {path: (Path(directory) / path).read_text(encoding="utf-8") for path in (SPECIALIZER, TEMPLATE)}


def block(source: str, opening: str) -> str:
    """The text between the braces of the first `opening` (a line ending in `{`), by brace depth."""
    start = source.index(opening) + len(opening)
    depth = 1
    for position in range(start, len(source)):
        depth += {"{": 1, "}": -1}.get(source[position], 0)
        if depth == 0:
            return source[start:position]
    raise AssertionError(f"{opening!r} is never closed")


class RawMetadataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        files = rebuilt_files()
        cls.source = files[SPECIALIZER]
        cls.template = json.loads(files[TEMPLATE])
        # Specialize's two sides of the RAW decision: `raw` is the camera description's colour data.
        marker = "  // RAW: the sensor's own numbers, or none of its keys.\n  if (raw) {\n"
        cls.raw_side = block(cls.source, marker)
        after = cls.source[cls.source.index(marker) + len(marker) + len(cls.raw_side) + 1:]
        assert after.startswith(" else {\n"), after[:40]
        cls.plain_side = block(after, " else {\n")

    def numbers(self, key: str) -> set[int]:
        return {int(value) for value in self.template[key]}

    def tag_names(self, text: str) -> set[str]:
        return set(re.findall(r"\bANDROID_[A-Z0-9_]+\b", text))

    # ---- what the template gives every V4L2 camera, and what RAW must not lose

    def test_the_template_is_the_one_of_a_camera_without_raw(self) -> None:
        # "No available lens shading modes" is the template's own state for a camera without RAW: OFF only, so a camera
        # without RAW is not asked for a map.
        self.assertEqual(self.template["android.statistics.info.availableLensShadingMapModes"], ["0"])
        self.assertNotIn(ON, self.tag_names(self.plain_side))
        self.assertNotIn("AVAILABLE_LENS_SHADING_MAP_MODES", self.plain_side,
                         "the side without RAW leaves the template's lens shading map modes alone")

    def test_a_camera_with_raw_states_the_lens_shading_map_modes_off_and_on(self) -> None:
        # InitializeStatisticsDefaults: "RAW capable device must support lens shading map reporting!" (-22, and the
        # camera is gone from the list).
        found = re.search(r"Put<uint8_t>\(m, ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES,\s*\{([^}]*)\}\)",
                          self.raw_side)
        self.assertIsNotNone(found, "the RAW side states no lens shading map modes")
        self.assertEqual(sorted(name.strip() for name in found[1].split(",")), sorted([OFF, ON]))

    def test_raw_is_declared_only_with_the_raw_side(self) -> None:
        self.assertEqual(self.source.count("ANDROID_REQUEST_AVAILABLE_CAPABILITIES_RAW"), 1)
        self.assertIn("const bool raw = caps.raw && desc.white_level > desc.black_level && desc.white_level > 0;", self.source)

    def test_the_lens_shading_map_can_be_reported(self) -> None:
        # InitializeStatisticsDefaults: with ON supported, the result keys must hold the lens shading map ("Lens shading
        # map reporting available but corresponding result key is absent!"), and the map needs a size ("... without valid
        # shading map size!"; InitializeLensDefaults reads android.lens.info.shadingMapSize before it, and
        # HWLUtils' sensor characteristics refuse a RAW camera without one: "No available shading map size!").
        results = self.numbers("android.request.availableResultKeys")
        self.assertIn(LENS_SHADING_MAP, results)
        self.assertIn(LENS_SHADING_MAP_MODE, results)
        self.assertIn(LENS_SHADING_MAP_MODE, self.numbers("android.request.availableRequestKeys"))
        size = [int(value) for value in self.template["android.lens.info.shadingMapSize"]]
        self.assertEqual(len(size), 2)
        self.assertTrue(all(0 < value <= 64 for value in size), size)  # EmulatedSensor::kMaxLensShadingMapSize is 64 x 64
        self.assertIn(INFO_AVAILABLE_LENS_SHADING_MAP_MODES, self.numbers("android.request.availableCharacteristicsKeys"))
        # Nothing in the specializer takes a lens shading key out of a list.
        for call in re.findall(r"(?:EditKeyList|RemoveCharacteristic)\([^;]*;", self.source):
            self.assertNotIn("SHADING_MAP", call)

    def test_the_raw_results_and_hot_pixel_modes_are_there_for_a_raw_camera_only_by_the_plain_side_removing_them(self) -> None:
        # InitializeSensorDefaults: "RAW capable devices must be able to report the noise profile / neutral color point /
        # green split!"; InitializeStatisticsDefaults: "RAW capable device must support hot pixel map modes!".
        results = self.numbers("android.request.availableResultKeys")
        for tag in (SENSOR_NOISE_PROFILE, SENSOR_NEUTRAL_COLOR_POINT, SENSOR_GREEN_SPLIT, HOT_PIXEL_MAP_MODE):
            self.assertIn(tag, results, hex(tag))
        self.assertIn("android.statistics.info.availableHotPixelMapModes", self.template)
        self.assertIn(INFO_AVAILABLE_HOT_PIXEL_MAP_MODES, self.numbers("android.request.availableCharacteristicsKeys"))
        for name in ("SENSOR_NOISE_PROFILE", "SENSOR_GREEN_SPLIT", "SENSOR_NEUTRAL_COLOR_POINT",
                     "STATISTICS_INFO_AVAILABLE_HOT_PIXEL_MAP_MODES"):
            self.assertIn(f"ANDROID_{name}", self.plain_side, f"the side without RAW removes {name}")
            self.assertNotIn(f"ANDROID_{name}", self.raw_side, f"the RAW side must keep {name}")

    def test_the_raw_side_gives_the_sensor_the_numbers_the_sensor_characteristics_read(self) -> None:
        # HWLUtils, for a RAW camera: "Invalid ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT / WHITE_LEVEL /
        # BLACK_LEVEL_PATTERN / COLOR_TRANSFORM1 / FORWARD_MATRIX1!". The colour and forward matrices come from the
        # description or, when it has none, from the template (a 3 x 3 matrix of rationals).
        for tag in ("SENSOR_INFO_COLOR_FILTER_ARRANGEMENT", "SENSOR_INFO_WHITE_LEVEL", "SENSOR_BLACK_LEVEL_PATTERN"):
            self.assertIn(f"ANDROID_{tag}", self.raw_side, tag)
        for key in ("android.sensor.colorTransform1", "android.sensor.forwardMatrix1", "android.sensor.calibrationTransform1"):
            self.assertEqual(len(self.template[key]), 18, key)  # nine rationals, numerator and denominator each
        self.assertEqual(len(self.template["android.sensor.blackLevelPattern"]), 4)

    def test_the_result_of_a_request_with_the_mode_on_can_carry_the_map(self) -> None:
        # InitializeStatisticsDefaults gives the still capture template the mode ON for a RAW camera, and EmulatedRequestState
        # accepts a request's mode only when the camera states it. EmulatedSensor then makes a map of the stated size
        # (a perfect lens: ones) and the result, a copy of the request's settings, holds the mode and the map: the result keys
        # above list both, and the map is 4 values for each cell of the stated size.
        self.assertEqual(len(self.template["android.lens.info.shadingMapSize"]), 2)


if __name__ == "__main__":
    unittest.main()
