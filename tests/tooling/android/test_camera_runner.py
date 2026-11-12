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

# The log is most recent first: a disconnect above a connect ends it.
MEDIA_CAMERA_DISCONNECTED = MEDIA_CAMERA.replace(
    "  01-01 00:01:02 : CONNECT device 0",
    "  01-01 00:01:30 : DISCONNECT device 0 client for package org.lineageos.aperture (PID 1301) (PRIORITY: 0)\n"
    "  01-01 00:01:02 : CONNECT device 0")
MEDIA_CAMERA_RECONNECTED = MEDIA_CAMERA_DISCONNECTED.replace(
    "  01-01 00:01:30 : DISCONNECT",
    "  01-01 00:02:10 : CONNECT device 1 client for package org.lineageos.aperture (PID 1330) for Camera API 2 with "
    "Camera API 2.0 and cameraId 1\n  01-01 00:01:30 : DISCONNECT")

MEDIA_CAMERA_IDLE = MEDIA_CAMERA.split("== Camera service events log")[0]
# The service prints each client of the list on a line of its own (and runs "State" into "User Id").
ACTIVE_CLIENTS = (
    "[\n(Camera ID: 0, Cost: 100, PID: 1301, Score: 0, State: 2User Id: 0, "
    "Client Package Name: org.lineageos.aperture, Conflicting Client Devices: {})\n]")
MEDIA_CAMERA_ACTIVE = MEDIA_CAMERA_IDLE.replace("[]", ACTIVE_CLIENTS)
# Another package holds the camera, and the log says this one connected before.
MEDIA_CAMERA_OTHER_ACTIVE = MEDIA_CAMERA.replace("[]", ACTIVE_CLIENTS.replace(
    "org.lineageos.aperture", "org.example.other"))


def static_info(device: str, facing: str, capabilities: str, flash: str, af_modes: str) -> str:
    """One camera's static metadata as `dumpsys media.camera` prints it: under its header, the entries in
    the order of their names (the HAL's metadata is built from a JSON file), so the tags fall where the
    names of the sections do not follow the tags (android.info after android.flash, before android.lens),
    and the values of an entry in brackets under it."""
    return f"""\
== Camera HAL device device@1.1/internal/{device} (v3.7) static information: ==
  Resource cost: 100
  Conflicting devices: none
  API1 info:
    Has a flash unit: {flash.lower()}
    Facing: {facing}
    Orientation: 90
  CameraCharacteristics:
    android.colorCorrection.availableAberrationModes (00004): byte[3]
        [OFF FAST HIGH_QUALITY ]
    android.control.aeAvailableModes (1000e): byte[2]
        [OFF ON ]
    android.control.afAvailableModes (10012): byte[{len(af_modes.split())}]
        [{af_modes} ]
    android.flash.info.available (60000): byte[1]
        [{flash} ]
    android.info.supportedHardwareLevel (150000): byte[1]
        [LIMITED ]
    android.lens.facing (80005): byte[1]
        [{facing} ]
    android.request.availableCapabilities (c0012): byte[{len(capabilities.split())}]
        [{capabilities} ]
    android.request.availableRequestKeys (c0013): int32[40]
        [786432 786433 786434 786435 786436 786437 786438 786439 786440 786441 786442 786443 786444 786445 786446 786447 ]
        [786448 786449 786450 786451 786452 786453 786454 786455 786456 786457 786458 786459 786460 786461 786462 786463 ]
        [786464 786465 786466 786467 786468 786469 786470 786471 ]
"""


PHONE_STATIC_INFO = (
    "== Camera Provider HAL internal/0 (v2.7, remote) static info: 2 devices: ==\n"
    + static_info("0", "BACK", "BACKWARD_COMPATIBLE MANUAL_SENSOR RAW READ_SENSOR_SETTINGS",
                  "TRUE", "OFF AUTO MACRO CONTINUOUS_PICTURE CONTINUOUS_VIDEO")
    + static_info("1", "FRONT", "BACKWARD_COMPATIBLE MANUAL_SENSOR RAW", "FALSE", "OFF AUTO CONTINUOUS_PICTURE")
    + "== Camera device dump: ==\nDumping the request that is in flight:\n"
      "  android.control.mode (10001): byte[1]\n        [AUTO ]\n")


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

    def test_the_default_layout_of_a_profile_is_the_synthetic_camera_it_presents(self) -> None:
        # darwin/platform/synthetic_camera: the phone's back lenses have flash, manual control and RAW
        # and its zoom and front cameras stream together; the tablet's front camera cannot make the
        # guaranteed concurrent sizes; the laptop has one front camera.
        phone = layouts.by_name()[runner.DEFAULT_LAYOUTS["phone"]]
        self.assertEqual((phone.flash, phone.manual, phone.raw, phone.concurrent, phone.front2160),
                         (True, True, True, True, True))
        tablet = layouts.by_name()[runner.DEFAULT_LAYOUTS["tablet"]]
        self.assertEqual((tablet.flash, tablet.manual, tablet.raw, tablet.concurrent),
                         (True, True, True, False))
        self.assertEqual(layouts.by_name()[runner.DEFAULT_LAYOUTS["laptop"]].features,
                         ("android.hardware.camera.any", "android.hardware.camera.front"))

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
        # With auto the launcher composes the layout and no SKU is passed.
        self.assertEqual(runner.launcher_arguments(runner.make_plan("bf-large-flash", "phone", "live"), auto=True),
                         ["--camera=synthetic", "--camera-profile=phone", "--camera-layout=auto"])
        self.assertEqual(runner.launcher_arguments(runner.make_plan("bf-large-flash", "phone", "live"), auto=True,
                                                   calibration=Path("/c"))[-1],
                         "--camera-calibration-dir=/c")
        self.assertEqual(runner.launcher_arguments(runner.make_plan("front", "laptop", "blocked"))[0],
                         "--camera=blocked")
        self.assertEqual(runner.launcher_arguments(runner.make_plan("none", "tablet", "none"))[0],
                         "--camera=none")

    def test_the_composed_layout_has_raw_only_once_a_still_taught_the_colour_data(self) -> None:
        self.assertEqual(runner.auto_layout("phone", taught=False), "bf-large-flash-manual-concurrent-front2160")
        self.assertEqual(runner.auto_layout("phone", taught=True), runner.DEFAULT_LAYOUTS["phone"])
        self.assertEqual(runner.auto_layout("tablet", taught=False), "bf-small-flash-manual")
        self.assertEqual(runner.auto_layout("laptop", taught=False), "front")
        for profile in runner.PROFILE_CAMERAS:
            for taught in (False, True):
                runner.make_plan(runner.auto_layout(profile, taught), profile, "live")

    def test_the_launcher_accepts_what_the_runner_passes(self) -> None:
        source = (REPOSITORY / "darwin/app/macos/main.cpp").read_text(encoding="utf-8")
        for value in set(runner.CAMERA_SOURCES.values()):
            self.assertRegex(source, rf'value == "{value}"', value)
        for profile in runner.PROFILE_CAMERAS:
            self.assertRegex(source, rf'value == "{profile}"', profile)
        self.assertIn('valued("--camera=")', source)
        self.assertIn('valued("--camera-profile=")', source)
        self.assertIn('valued("--camera-layout=")', source)
        self.assertIn('valued("--camera-calibration-dir=")', source)

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

    def test_a_disconnect_after_the_connect_ends_the_connection(self) -> None:
        package = runner.CAMERA_PACKAGE
        self.assertFalse(runner.camera_connected(MEDIA_CAMERA_DISCONNECTED, package))
        self.assertTrue(runner.camera_connected(MEDIA_CAMERA_RECONNECTED, package))
        # Another package's disconnect does not end this package's connection.
        self.assertTrue(runner.camera_connected(MEDIA_CAMERA, package))
        # An active client is a connection whatever the log says.
        active = MEDIA_CAMERA_DISCONNECTED.replace("[]", ACTIVE_CLIENTS)
        self.assertTrue(runner.camera_connected(active, package))
        # The clients are the lines of the list, and only another package's does not count.
        self.assertEqual(runner.active_client_packages(MEDIA_CAMERA_ACTIVE), {package})
        self.assertEqual(runner.active_client_packages(MEDIA_CAMERA_OTHER_ACTIVE), {"org.example.other"})
        self.assertEqual(runner.active_client_packages(MEDIA_CAMERA_IDLE), set())
        self.assertFalse(runner.camera_connected(
            MEDIA_CAMERA_OTHER_ACTIVE.replace("CONNECT device 0", "DISCONNECT device 0"), package))

    def test_the_static_info_of_each_camera(self) -> None:
        infos = runner.parse_static_info(PHONE_STATIC_INFO)
        self.assertEqual([info.facing for info in infos], ["BACK", "FRONT"])
        self.assertEqual(infos[0].capabilities, {"BACKWARD_COMPATIBLE", "MANUAL_SENSOR", "RAW", "READ_SENSOR_SETTINGS"})
        self.assertEqual((infos[0].flash, infos[1].flash), (True, False))
        self.assertEqual(infos[0].af_modes, {"OFF", "AUTO", "MACRO", "CONTINUOUS_PICTURE", "CONTINUOUS_VIDEO"})
        self.assertEqual(runner.parse_static_info("Number of camera devices: 0\n"), [])
        # Entries under a header of another kind belong to no camera.
        self.assertEqual(runner.parse_static_info(
            "== Camera device dump: ==\n  android.lens.facing (80005): byte[1]\n        [BACK ]\n"), [])

    def test_a_camera_starts_at_its_header_not_at_a_falling_tag(self) -> None:
        # In the fixture the tags fall inside a camera (android.info is dumped before android.lens).
        tags = [int(m[1], 16) for m in re.finditer(r"\((\w+)\): ", static_info("0", "BACK", "RAW", "TRUE", "OFF"))]
        self.assertTrue(any(later < earlier for earlier, later in zip(tags, tags[1:])))
        infos = runner.parse_static_info(static_info("0", "BACK", "RAW", "TRUE", "OFF AUTO"))
        self.assertEqual(len(infos), 1)
        self.assertEqual((infos[0].flash, infos[0].af_modes), (True, {"OFF", "AUTO"}))

    def test_the_features_the_characteristics_earn(self) -> None:
        infos = runner.parse_static_info(PHONE_STATIC_INFO)
        earned = runner.features_of(infos)
        self.assertEqual(earned, {
            "android.hardware.camera", "android.hardware.camera.any", "android.hardware.camera.autofocus",
            "android.hardware.camera.capability.manual_sensor", "android.hardware.camera.capability.raw",
            "android.hardware.camera.flash", "android.hardware.camera.front"})
        # They are what the phone's default layout states, bar the combination feature.
        stated = set(layouts.by_name()[runner.DEFAULT_LAYOUTS["phone"]].features)
        self.assertEqual(stated - earned, {runner.CONCURRENT_FEATURE})
        # A back camera without flash or RAW, and a layout that says it has them, differ.
        plain = runner.parse_static_info(static_info("0", "BACK", "BACKWARD_COMPATIBLE", "FALSE", "OFF AUTO"))
        self.assertEqual(runner.features_of(plain), {"android.hardware.camera", "android.hardware.camera.any",
                                                     "android.hardware.camera.autofocus"})
        # The flash and the autofocus of a front camera earn nothing: only the back camera's count.
        front = runner.parse_static_info(static_info("1", "FRONT", "BACKWARD_COMPATIBLE MANUAL_SENSOR", "TRUE", "OFF AUTO"))
        self.assertEqual(runner.features_of(front), {"android.hardware.camera.any", "android.hardware.camera.front",
                                                     "android.hardware.camera.capability.manual_sensor"})
        self.assertEqual(runner.features_of([]), set())

    def test_the_blocked_preview_is_sampled_twice(self) -> None:
        chart, black = (120.0, 60.0), (2.0, 1.0)
        sleeps: list[float] = []
        for samples, expected in (([black, black], None),
                                  ([black, chart], "the preview shows a picture (mean 120.0, deviation 60.0, sample 2)"),
                                  ([chart, black], "the preview shows a picture (mean 120.0, deviation 60.0, sample 1)")):
            with self.subTest(samples=samples):
                taken = iter(samples)
                sleeps.clear()
                self.assertEqual(runner.blocked_preview(lambda: next(taken), sleeps.append), expected)
                self.assertEqual(sleeps, [runner.BLOCKED_SAMPLE_GAP_SECONDS] if expected is None
                                 or "sample 2" in expected else [])
        self.assertEqual(runner.BLOCKED_SAMPLES, 2)

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
