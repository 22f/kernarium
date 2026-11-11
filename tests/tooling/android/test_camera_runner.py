#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The camera runner's expectations and its reading of the guest.

run_android_camera.py boots a release with the launcher's synthetic camera and a camera layout on the
vendor SKU, and reads `pm list features`, `dumpsys media.camera`, a screenshot and the still the camera
application takes. The texts below have the layout those commands print, and the images are built here,
so the runner's plan for every layout, its parsers and its image readers run without a guest. The
expectations are cross-checked against the generator's layouts, the camera ID plan of the HAL and the
launcher's own option parser.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import re
import struct
import sys
import unittest
import zlib

TESTS = Path(__file__).resolve().parents[2]
REPOSITORY = TESTS.parent
SCRIPT = TESTS / "integration/android/run_android_camera.py"
SPEC = importlib.util.spec_from_file_location("run_android_camera", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
# Dataclasses resolve their annotations through sys.modules.
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)
layouts = runner.camera_layouts

FEATURES = """\
feature:android.hardware.audio.output
feature:android.hardware.camera
feature:android.hardware.camera.any
feature:android.hardware.camera.autofocus
feature:android.hardware.camera.capability.manual_sensor
feature:android.hardware.camera.front
feature:android.hardware.location
feature:android.software.midi=0
"""

MEDIA_CAMERA = """\
== Service global info: ==

Number of camera devices: 3
Number of normal camera devices: 3
   Device 0 maps to "0"
   Device 1 maps to "1"
   Device 2 maps to "2"
Active Camera Clients:
[]
Allowed user IDs: 0

== Camera service events log (most recent at top): ==
01-01 00:01:02 : CONNECT device 0 client for package org.lineageos.aperture (PID 1301) for Camera API 2 with Camera API 2.0 and cameraId 0
01-01 00:00:55 : DISCONNECT device 1 client for package org.example.other (PID 900) (PRIORITY: 0)
"""

MEDIA_CAMERA_IDLE = MEDIA_CAMERA.split("== Camera service events log")[0]
MEDIA_CAMERA_ACTIVE = MEDIA_CAMERA_IDLE.replace(
    "[]", "[(Client 0: org.lineageos.aperture, priority 100, pid 1301)]")


def png(width: int, height: int, color: int, pixels: list[bytes], filters: list[int]) -> bytes:
    """A PNG whose rows use the given filters (cycled), from rows of raw pixel bytes."""
    channels = {0: 1, 2: 3, 6: 4}[color]
    stride = width * channels
    previous = bytearray(stride)
    raw = bytearray()
    for y in range(height):
        kind = filters[y % len(filters)]
        row = pixels[y]
        out = bytearray()
        for i in range(stride):
            left = row[i - channels] if i >= channels else 0
            up = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            predictor = {0: 0, 1: left, 2: up, 3: (left + up) >> 1, 4: runner.paeth(left, up, corner)}[kind]
            out.append((row[i] - predictor) & 255)
        raw.append(kind)
        raw += out
        previous = bytearray(row)

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    return (runner.PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, color, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def gradient(width: int, height: int, channels: int) -> list[bytes]:
    return [bytes((x * 7 + y * 13 + c * 31) & 255 for x in range(width) for c in range(channels))
            for y in range(height)]


def jpeg(width: int, height: int, exif: bool = True, end: bool = True) -> bytes:
    data = b"\xff\xd8"
    if exif:
        body = b"Exif\x00\x00" + b"MM\x00*\x00\x00\x00\x08\x00\x00"
        data += b"\xff\xe1" + struct.pack(">H", len(body) + 2) + body
    sof = struct.pack(">BHHB", 8, height, width, 1) + b"\x01\x11\x00"
    data += b"\xff\xc0" + struct.pack(">H", len(sof) + 2) + sof
    data += b"\xff\xda\x00\x08\x01\x01\x00\x00\x3f\x00" + b"\x12\x34\xff\x00\x56"
    return data + (b"\xff\xd9" if end else b"")


class PlanTests(unittest.TestCase):
    def test_every_layout_of_a_profile_has_a_plan_that_follows_the_generator(self) -> None:
        for profile, (cameras, ids) in runner.PROFILE_CAMERAS.items():
            matching = [layout for layout in layouts.all_layouts() if layout.cameras == cameras]
            self.assertTrue(matching, profile)
            for layout in matching:
                with self.subTest(profile=profile, layout=layout.name):
                    plan = runner.make_plan(layout.name, profile, "live")
                    self.assertEqual(plan.features, frozenset(layout.features))
                    self.assertEqual(plan.media_variant, layout.media_variant)
                    self.assertEqual(plan.camera_ids, ids)
                    self.assertTrue(plan.features)

    def test_a_layout_must_describe_the_profiles_cameras(self) -> None:
        for layout, profile in (("front", "phone"), ("bf-small", "laptop"), ("bf-large", "laptop"),
                                ("front-manual", "tablet"), ("none", "phone")):
            with self.subTest(layout=layout, profile=profile), self.assertRaises(ValueError):
                runner.make_plan(layout, profile, "live")
        for arguments in (("bf-huge", "phone", "live"), ("bf-small", "watch", "live"), ("bf-small", "phone", "x")):
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                runner.make_plan(*arguments)

    def test_a_guest_without_the_camera_device_has_no_camera(self) -> None:
        plan = runner.make_plan("none", "phone", "none")
        self.assertEqual((plan.camera_ids, plan.features, plan.media_variant), ((), frozenset(), ""))
        with self.assertRaises(ValueError):
            runner.make_plan("bf-small", "phone", "none")

    def test_the_default_layouts_exist_and_fit_their_profiles(self) -> None:
        self.assertEqual(set(runner.DEFAULT_LAYOUTS), set(runner.PROFILE_CAMERAS))
        for profile, name in runner.DEFAULT_LAYOUTS.items():
            with self.subTest(profile=profile):
                runner.make_plan(name, profile, "live")
                runner.make_plan(name, profile, "blocked")

    def test_the_camera_ids_follow_the_hals_plan(self) -> None:
        header = (REPOSITORY / "android/lineage-24/overlay/camera/hal/camera_ids.hpp").read_text(encoding="utf-8")
        found = {name: int(value) for name, value in re.findall(
            r"constexpr std::uint32_t (\w+_camera_id) = (\d+);", header)}
        self.assertEqual(found, {"wide_camera_id": 2, "ultra_wide_camera_id": 3, "telephoto_camera_id": 4})
        self.assertEqual(runner.PROFILE_CAMERAS["phone"][1], ("0", "1", *(str(value) for value in found.values())))
        self.assertEqual(runner.PROFILE_CAMERAS["tablet"][1], ("0", "1"))
        self.assertEqual(runner.PROFILE_CAMERAS["laptop"][1], ("0",))

    def test_the_still_caps_follow_the_tiers(self) -> None:
        small = runner.make_plan("bf-small", "tablet", "live")
        large = runner.make_plan("bf-large", "phone", "live")
        self.assertLess(small.still_maximum_pixels, large.still_maximum_pixels)
        self.assertGreaterEqual(small.still_maximum_pixels, 4000 * 3000)
        self.assertGreaterEqual(large.still_maximum_pixels, 8000 * 6000)


class LauncherTests(unittest.TestCase):
    def test_the_launcher_arguments(self) -> None:
        self.assertEqual(runner.launcher_arguments(runner.make_plan("bf-large-flash", "phone", "live")),
                         ["--camera=synthetic", "--camera-profile=phone",
                          "androidboot.product.vendor.sku=bf-large-flash"])
        self.assertEqual(runner.launcher_arguments(runner.make_plan("front", "laptop", "blocked"))[0],
                         "--camera=blocked")
        self.assertEqual(runner.launcher_arguments(runner.make_plan("none", "tablet", "none"))[0],
                         "--camera=none")

    def test_the_launcher_accepts_what_the_runner_passes(self) -> None:
        source = (REPOSITORY / "darwin/app/macos/main.cpp").read_text(encoding="utf-8")
        for value in set(runner.CAMERA_SOURCES.values()):
            self.assertRegex(source, rf'value == "{value}"', value)
        for profile in runner.PROFILE_CAMERAS:
            self.assertRegex(source, rf'value == "{profile}"', profile)
        self.assertIn('valued("--camera=")', source)
        self.assertIn('valued("--camera-profile=")', source)

    def test_the_sku_argument_becomes_the_property_the_trigger_reads(self) -> None:
        self.assertEqual(layouts.SKU_BOOT_ARGUMENT, "androidboot." + layouts.SKU_PROPERTY.removeprefix("ro.boot.") + "=")
        rc = (REPOSITORY / "android/lineage-24/overlay/camera/generated" / layouts.INIT_FILE).read_text(encoding="utf-8")
        self.assertIn(f"on property:{layouts.SKU_PROPERTY}=bf-large", rc)

    def test_the_parser_defaults(self) -> None:
        options = runner.build_parser().parse_args([])
        self.assertEqual((options.mode, options.camera_profile, options.layout), ("live", "phone", None))
        self.assertEqual(options.adb_port, runner.slot_ports.slot_port(runner.slot_ports.ADB_OFFSET, 5580))


class GuestReaderTests(unittest.TestCase):
    def test_camera_features(self) -> None:
        self.assertEqual(runner.parse_camera_features(FEATURES), {
            "android.hardware.camera", "android.hardware.camera.any", "android.hardware.camera.autofocus",
            "android.hardware.camera.capability.manual_sensor", "android.hardware.camera.front"})
        self.assertEqual(runner.parse_camera_features("feature:android.hardware.wifi\n"), set())

    def test_camera_ids(self) -> None:
        self.assertEqual(runner.parse_camera_ids(MEDIA_CAMERA), ["0", "1", "2"])
        self.assertEqual(runner.parse_camera_ids("Number of camera devices: 0\n"), [])

    def test_a_client_of_the_camera_service(self) -> None:
        package = runner.CAMERA_PACKAGE
        self.assertTrue(runner.camera_connected(MEDIA_CAMERA, package))
        self.assertTrue(runner.camera_connected(MEDIA_CAMERA_ACTIVE, package))
        self.assertFalse(runner.camera_connected(MEDIA_CAMERA_IDLE, package))
        self.assertFalse(runner.camera_connected(MEDIA_CAMERA, "org.example.absent"))

    def test_listings(self) -> None:
        self.assertEqual(runner.parse_listing("IMG_1.jpg\nIMG_2.jpg\n"), {"IMG_1.jpg", "IMG_2.jpg"})
        self.assertEqual(runner.parse_listing("ls: /sdcard/DCIM/Camera: No such file or directory\n"), set())
        self.assertEqual(runner.parse_listing(""), set())


class PngTests(unittest.TestCase):
    def test_every_filter_decodes(self) -> None:
        for color, channels in ((0, 1), (2, 3), (6, 4)):
            width, height = 9, 8
            pixels = gradient(width, height, channels)
            for filters in ([0], [1], [2], [3], [4], [0, 1, 2, 3, 4]):
                with self.subTest(color=color, filters=filters):
                    image = runner.decode_png(png(width, height, color, pixels, filters))
                    self.assertEqual((image.width, image.height, image.channels), (width, height, channels))
                    self.assertEqual(image.rows, pixels)

    def test_a_bad_png_is_refused(self) -> None:
        good = png(4, 4, 6, gradient(4, 4, 4), [0])
        for name, data in (("signature", b"GIF89a" + good), ("truncated", good[:40]), ("empty", b""),
                           ("cut data", good[:-20])):
            with self.subTest(name=name), self.assertRaises((ValueError, zlib.error)):
                runner.decode_png(data)
        interlaced = bytearray(good)
        interlaced[8 + 8 + 12] = 1  # IHDR's interlace byte
        with self.assertRaises(ValueError):
            runner.decode_png(bytes(interlaced))

    def test_the_chart_is_told_from_black(self) -> None:
        width, height = 40, 40
        black = runner.decode_png(png(width, height, 6, [bytes(width * 4)] * height, [0]))
        mean, deviation = runner.luma_statistics(black)
        self.assertEqual((mean, deviation), (0.0, 0.0))
        patches = [bytes(value for x in range(width)
                         for value in ((255, 255, 255, 255) if (x // 5 + y // 5) % 2 else (20, 20, 20, 255)))
                   for y in range(height)]
        mean, deviation = runner.luma_statistics(runner.decode_png(png(width, height, 6, patches, [4])))
        self.assertGreater(mean, runner.BLACK_MAXIMUM_MEAN)
        self.assertGreater(deviation, runner.CHART_MINIMUM_DEVIATION)
        self.assertLess(runner.BLACK_MAXIMUM_DEVIATION, runner.CHART_MINIMUM_DEVIATION)


class JpegTests(unittest.TestCase):
    def test_a_complete_still(self) -> None:
        info = runner.parse_jpeg(jpeg(4032, 3024))
        self.assertEqual((info.width, info.height, info.exif, info.complete), (4032, 3024, True, True))

    def test_what_the_check_tells_apart(self) -> None:
        self.assertFalse(runner.parse_jpeg(jpeg(640, 480, exif=False)).exif)
        self.assertFalse(runner.parse_jpeg(jpeg(640, 480, end=False)).complete)
        for data in (b"", b"\x89PNG\r\n", b"\xff\xd8\x00"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                runner.parse_jpeg(data)


if __name__ == "__main__":
    unittest.main()
