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

import contextlib
import importlib.util
import io
from pathlib import Path
import re
import struct
import sys
import unittest
from unittest import mock
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
        # darwin/platform/synthetic_camera: the phone's back lenses have flash and manual control and its
        # zoom and front cameras stream together; the tablet's front camera cannot make the guaranteed
        # concurrent sizes; the laptop has one front camera. A fresh host offers no RAW: it comes with the
        # colour data a first still keeps (darwin/device/camera/README.md, Calibration).
        phone = layouts.by_name()[runner.DEFAULT_LAYOUTS["phone"]]
        self.assertEqual((phone.flash, phone.manual, phone.raw, phone.concurrent, phone.front2160),
                         (True, True, False, True, True))
        tablet = layouts.by_name()[runner.DEFAULT_LAYOUTS["tablet"]]
        self.assertEqual((tablet.flash, tablet.manual, tablet.raw, tablet.concurrent),
                         (True, True, False, False))
        self.assertEqual(layouts.by_name()[runner.DEFAULT_LAYOUTS["laptop"]].features,
                         ("android.hardware.camera.any", "android.hardware.camera.front"))
        for name in runner.DEFAULT_LAYOUTS.values():
            self.assertFalse(layouts.by_name()[name].raw, name)
            # the layout a calibrated host composes is the same with RAW, and it exists
            if layouts.by_name()[name].manual:
                self.assertTrue(name.replace("manual", "manual-raw", 1) in layouts.by_name(), name)

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
        # The phone's default layout states them, bar RAW (a fresh host has no calibration) and the combination feature.
        stated = set(layouts.by_name()[runner.DEFAULT_LAYOUTS["phone"]].features)
        self.assertEqual(stated - earned, {runner.CONCURRENT_FEATURE})
        self.assertEqual(earned - stated, {"android.hardware.camera.capability.raw"})
        # A back camera without flash or RAW, and a layout that says it has them, differ.
        plain = runner.parse_static_info(static_info("0", "BACK", "BACKWARD_COMPATIBLE", "FALSE", "OFF AUTO"))
        self.assertEqual(runner.features_of(plain), {"android.hardware.camera", "android.hardware.camera.any",
                                                     "android.hardware.camera.autofocus"})
        # The flash and the autofocus of a front camera earn nothing: only the back camera's count.
        front = runner.parse_static_info(static_info("1", "FRONT", "BACKWARD_COMPATIBLE MANUAL_SENSOR", "TRUE", "OFF AUTO"))
        self.assertEqual(runner.features_of(front), {"android.hardware.camera.any", "android.hardware.camera.front",
                                                     "android.hardware.camera.capability.manual_sensor"})
        self.assertEqual(runner.features_of([]), set())
        # Android 17 prints the autofocus modes as numbers: 1 is AUTO.
        numbered = runner.parse_static_info(static_info("0", "BACK", "BACKWARD_COMPATIBLE", "FALSE", "0 1 2 3 4"))
        self.assertEqual(numbered[0].af_modes, {"OFF", "AUTO", "MACRO", "CONTINUOUS_VIDEO", "CONTINUOUS_PICTURE"})
        self.assertIn("android.hardware.camera.autofocus", runner.features_of(numbered))
        unfocused = runner.parse_static_info(static_info("0", "BACK", "BACKWARD_COMPATIBLE", "FALSE", "0"))
        self.assertNotIn("android.hardware.camera.autofocus", runner.features_of(unfocused))

    def test_the_cameraserver_restart_needs_a_debuggable_build(self) -> None:
        self.assertIsNone(runner.cameraserver_restart_skip("1"))
        for value in ("0", "", "unknown"):
            self.assertIn("user build", runner.cameraserver_restart_skip(value) or "")

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


class ProviderLifeTests(unittest.TestCase):
    PS = """\
  PID NAME
    1 init
  612 cameraserver
 1480 android.hardware.camera.provider@2.7-service-google-lazy
 1500 org.lineageos.aperture
"""

    def test_the_provider_process_is_read_from_the_process_listing(self) -> None:
        self.assertEqual(runner.parse_provider_pids(self.PS), [1480])
        self.assertEqual(runner.parse_provider_pids("  PID NAME\n  612 cameraserver\n"), [])
        self.assertEqual(runner.parse_provider_pids(""), [])

    class Guest:
        def __init__(self, listings: list[list[int]]) -> None:
            self.listings = listings

        def alive(self) -> None:
            pass

        def provider_pids(self) -> list[int]:
            return self.listings.pop(0) if len(self.listings) > 1 else self.listings[0]

    def test_the_wait_ends_when_the_provider_is_gone(self) -> None:
        guest = self.Guest([[1480], [1480], []])
        with mock.patch.object(runner.time, "sleep"):
            self.assertGreaterEqual(runner.wait_provider_exit(guest, "after the listing"), 0)

    def test_a_provider_that_never_exits_fails_the_wait(self) -> None:
        guest = self.Guest([[1480]])
        with mock.patch.object(runner.time, "sleep"), mock.patch.object(runner, "PROVIDER_EXIT_SECONDS", 0.05):
            with self.assertRaisesRegex(runner.Failure, "camera provider to exit after the listing: it still runs as \\[1480\\]"):
                runner.wait_provider_exit(guest, "after the listing")


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


REAL_DUMP_CLIENT = (
    "Active Camera Clients:\n(Camera ID: 1, Cost: 100, PID: 2131, Score: 0, State: 2User Id: 0, "
    "Client Package Name: org.lineageos.aperture, Conflicting Client Devices: {2, 3, 4, })\n"
    "Active Client listener PIDs: 605, 876, 2131\n")
CONTENT_ROWS = (
    "Row: 0 _id=33, _display_name=VID_20261112_123456.mp4, _data=/storage/emulated/0/Movies/Aperture/"
    "VID_20261112_123456.mp4, duration=4021, _size=912345\n"
    "Row: 1 _id=34, _display_name=a, b.mp4, _data=/storage/emulated/0/Movies/x.mp4, duration=NULL, _size=0\n")
HAL_LOG = """\
11-12 12:38:31.955  2194  2215 I GCH_AidlCameraDeviceSession: ProcessBatchCaptureResult: First frame done
11-12 12:38:32.059  2194  2204 E V4l2Stream: Queue: every buffer of the stream is in flight
11-12 12:38:32.059  2194  2204 E V4l2FrameSource: BeginRequest: cannot queue a 1008x756 buffer of format 23
11-12 12:38:32.059  2194  2204 W EmulatedSensorHost: CaptureFromHost: frame 2 was not queued completely (-2)
11-12 12:38:32.100  2194  2204 W V4l2FrameSource: BeginRequest: 1 buffers of earlier requests were never collected
11-12 12:38:32.200  2194  2204 W V4l2Stream: Wait: the camera did not deliver the buffer of slot 0; the stream starts over
11-12 12:38:32.300  2194  2204 W V4l2FrameSource: QueueBuffer: the node refused the dma-buf; the 1x1 stream copies
11-12 12:38:32.400  2194  2204 E SomeOtherTag: unrelated error
"""


def box(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I4s", len(payload) + 8, kind) + payload


MEDIA_SCALE = 90000


def movie(seconds: float, scale: int = 1000, video: bool = True, version: int = 0, fps: float = 30.0,
          samples: bool = True) -> bytes:
    """A finished movie: `mvhd`, and one track whose `mdhd` has MEDIA_SCALE ticks a second and, for a video
    track with `samples`, an `stts` of one entry of seconds * fps frames."""
    if version == 1:
        mvhd = struct.pack(">B3xQQIQ", 1, 0, 0, scale, int(seconds * scale)) + b"\x00" * 80
    else:
        mvhd = struct.pack(">B3xIIII", 0, 0, 0, scale, int(seconds * scale)) + b"\x00" * 80
    frames, delta = int(seconds * fps), round(MEDIA_SCALE / fps)
    if version == 1:
        mdhd = struct.pack(">B3xQQIQ", 1, 0, 0, MEDIA_SCALE, frames * delta) + b"\x00" * 4
    else:
        mdhd = struct.pack(">B3xIIII", 0, 0, 0, MEDIA_SCALE, frames * delta) + b"\x00" * 4
    handler = box(b"hdlr", b"\x00\x00\x00\x00\x00\x00\x00\x00" + (b"vide" if video else b"soun") + b"\x00" * 12)
    stts = box(b"stts", struct.pack(">BxxxI", 0, 1) + struct.pack(">II", frames, delta))
    track = box(b"mdia", box(b"mdhd", mdhd) + handler + (box(b"minf", box(b"stbl", stts)) if video and samples else b""))
    return (box(b"ftyp", b"isom\x00\x00\x02\x00isom") + box(b"mdat", b"\x00" * 40)
            + box(b"moov", box(b"mvhd", mvhd) + box(b"trak", track)))


class VideoAndFrontTests(unittest.TestCase):
    def test_the_cameras_a_package_holds(self) -> None:
        package = "org.lineageos.aperture"
        self.assertEqual(runner.active_client_camera_ids(REAL_DUMP_CLIENT, package), {"1"})
        self.assertEqual(runner.active_client_camera_ids(MEDIA_CAMERA_ACTIVE, package), {"0"})
        self.assertEqual(runner.active_client_camera_ids(MEDIA_CAMERA_IDLE, package), set())
        self.assertEqual(runner.active_client_camera_ids(MEDIA_CAMERA_OTHER_ACTIVE, package), set())

    def test_the_rows_of_a_content_query(self) -> None:
        rows = runner.parse_content_rows(CONTENT_ROWS + "No result found.\n")
        self.assertEqual([row["_id"] for row in rows], ["33", "34"])
        self.assertEqual(rows[0]["duration"], "4021")
        self.assertTrue(rows[0]["_data"].endswith("VID_20261112_123456.mp4"))
        self.assertEqual(rows[1]["_display_name"], "a, b.mp4")
        self.assertEqual(runner.parse_content_rows("No result found.\n"), [])

    def test_the_duration_and_tracks_of_a_movie(self) -> None:
        for version in (0, 1):
            info = runner.parse_mp4(movie(4.0, version=version))
            self.assertAlmostEqual(info.seconds, 4.0)
            self.assertTrue(info.video)
            self.assertFalse(info.audio)
        self.assertFalse(runner.parse_mp4(movie(2.0, video=False)).video)
        self.assertAlmostEqual(runner.parse_mp4(movie(1.5, scale=90000)).seconds, 1.5)

    def test_the_average_frame_rate_of_a_movie(self) -> None:
        for version in (0, 1):
            for fps in (15.0, 24.0, 30.0):
                with self.subTest(version=version, fps=fps):
                    info = runner.parse_mp4(movie(4.0, version=version, fps=fps))
                    self.assertEqual(info.frames, int(4.0 * fps))
                    self.assertAlmostEqual(info.video_seconds, 4.0, places=2)
                    self.assertAlmostEqual(info.fps, fps, places=1)
        self.assertEqual(runner.parse_mp4(movie(3.0, video=False)).fps, 0.0)
        no_samples = runner.parse_mp4(movie(3.0, samples=False))
        self.assertEqual((no_samples.frames, no_samples.fps), (0, 0.0))

    def test_a_sample_table_that_does_not_fit_is_refused(self) -> None:
        whole = bytearray(movie(2.0))
        at = bytes(whole).index(b"stts") + 8
        whole[at:at + 4] = struct.pack(">I", 1000)  # entry count: far more than the box holds
        with self.assertRaisesRegex(ValueError, "does not fit"):
            runner.parse_mp4(bytes(whole))

    def test_an_unfinished_movie_is_refused(self) -> None:
        whole = movie(3.0)
        for data in (b"", b"\x00\x00\x00\x18ftypisom", whole[:-30], box(b"ftyp", b"isom") + box(b"mdat", b"\x00" * 8)):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                runner.parse_mp4(data)

    def test_the_hal_log_failures(self) -> None:
        failures = runner.hal_failures(HAL_LOG)
        self.assertEqual(len(failures), 5)
        self.assertTrue(all("SomeOtherTag" not in line and "dma-buf" not in line for line in failures))
        self.assertEqual(runner.hal_failures(HAL_LOG.split("\n")[0] + "\n"), [])
        self.assertEqual(runner.hal_failures(""), [])


class FakeGuest:
    """A guest that answers the runner's commands from a script and records them."""

    def __init__(self, plan, front_holds: bool = True, movies=None) -> None:
        self.plan = plan
        self.commands: list[str] = []
        self.held = {"0"}
        self.front_holds = front_holds
        self.rows: dict[str, dict[str, str]] = {"1": {"_id": "1", "_data": "/sdcard/old.mp4"}}
        self.recording = False
        self.movie = movies if movies is not None else movie(3.5)
        self.still_files = {"IMG_old.jpg"}
        self.log = ""
        chart = [bytes(value for x in range(40) for value in ((255,) * 4 if (x // 5 + y // 5) % 2 else (20, 20, 20, 255)))
                 for y in range(40)]
        self.chart = runner.decode_png(png(40, 40, 6, chart, [0]))

    def alive(self) -> None:
        pass

    def shell(self, command: str, timeout: float = 0) -> str:
        self.commands.append(command)
        if command.startswith("am start") and "USE_FRONT_CAMERA true" in command:
            # The laptop's only camera is the front one, camera 0.
            front = "0" if self.plan.profile == "laptop" else "1"
            self.held = {front} if self.front_holds else {"0"}
        elif command.startswith("am start"):
            self.held = {"0"}
        if command == f"input keyevent {runner.CAMERA_KEYCODE}":
            if "VIDEO_CAMERA" in " ".join(self.commands):
                if self.recording:
                    self.rows["2"] = {"_id": "2", "_data": "/sdcard/new.mp4", "_display_name": "new.mp4"}
                self.recording = not self.recording
            else:
                self.still_files.add("IMG_new%d.jpg" % len(self.still_files))
        return ""

    def media_camera(self) -> str:
        clients = "".join(
            f"(Camera ID: {camera}, Cost: 100, PID: 1301, Score: 0, State: 2User Id: 0, "
            "Client Package Name: org.lineageos.aperture, Conflicting Client Devices: {})\n" for camera in sorted(self.held))
        return "Active Camera Clients:\n" + clients + "Active Client listener PIDs: 1\n"

    def screenshot(self):
        return self.chart

    def videos(self):
        return dict(self.rows), "Row: 0 ..."

    def stills(self) -> set[str]:
        return set(self.still_files)

    def hal_log(self) -> str:
        return self.log

    def pull(self, remote: str) -> bytes:
        return jpeg(4000, 3000) if remote.endswith(".jpg") else self.movie


class FailureLogTests(unittest.TestCase):
    def test_the_camera_log_lines_are_printed_at_a_failure(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        guest = FakeGuest(plan)
        commands: list[str] = []
        guest.shell = lambda command, timeout=0: (commands.append(command), "E V4l2FrameSource: a line\n")[1]
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            runner.print_camera_log(guest)
        self.assertIn("camera log lines of the guest:\nE V4l2FrameSource: a line", err.getvalue())
        self.assertIn("logcat -d -b all | grep -a -E", commands[0])
        self.assertIn("vrend", commands[0])
        self.assertIn("grep -a -v -E 'SurfaceFlinger|adbd", commands[0])

    def test_a_guest_that_is_gone_does_not_hide_the_failure(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        guest = FakeGuest(plan)

        def gone(command: str, timeout: float = 0) -> str:
            raise OSError("adb: device offline")

        guest.shell = gone
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            runner.print_camera_log(guest)
        self.assertIn("no camera log lines: adb: device offline", err.getvalue())


class VideoAndFrontFlowTests(unittest.TestCase):
    def run_step(self, step, plan, guest) -> None:
        with mock.patch.object(runner.time, "sleep", lambda seconds: None):
            step(guest, plan)

    def test_the_video_step_records_with_the_shutter_key_in_video_mode(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        guest = FakeGuest(plan)
        sleeps: list[float] = []
        with mock.patch.object(runner.time, "sleep", sleeps.append):
            runner.check_video(guest, plan)
        starts = [command for command in guest.commands if command.startswith("am start")]
        self.assertEqual(starts, [f"am start -a {runner.VIDEO_ACTION} --ez {runner.FRONT_EXTRA} false"])
        self.assertEqual(guest.commands.count(f"input keyevent {runner.CAMERA_KEYCODE}"), 2)
        self.assertIn(f"am force-stop {runner.CAMERA_PACKAGE}", guest.commands)
        self.assertEqual(runner.VIDEO_HOLD_SECONDS, 5)
        self.assertIn(runner.VIDEO_HOLD_SECONDS, sleeps)

    def test_the_video_step_prints_the_average_frame_rate(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(4.0, fps=29.97)))
        self.assertRegex(out.getvalue(), r"video: new\.mp4 4\.00 s, 119 frames over 3\.97 s = 30\.0 fps average")

    def test_a_movie_below_the_frame_rate_floor_fails_after_it_is_printed(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaisesRegex(runner.Failure, r"averages 15\.0 fps .*below 20"):
            self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(4.0, fps=15.0)))
        self.assertIn("= 15.0 fps average", out.getvalue())

    def test_a_movie_without_a_sample_table_fails(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        with self.assertRaisesRegex(runner.Failure, "no sample table"):
            self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(4.0, samples=False)))

    def test_a_movie_that_is_too_short_or_has_no_video_fails(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        for data, what in ((movie(0.4), "lasts 0.40 s"), (movie(3.0, video=False), "no video track"),
                           (b"junk", "not a finished movie")):
            with self.subTest(what=what), self.assertRaisesRegex(runner.Failure, what):
                self.run_step(runner.check_video, plan, FakeGuest(plan, movies=data))

    def test_no_new_movie_fails_with_the_query(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        guest = FakeGuest(plan)
        guest.rows = {"1": {"_id": "1"}}
        guest.shell = lambda command, timeout=0: ""  # the shutter key records nothing
        with mock.patch.object(runner, "VIDEO_SECONDS", 0.2), self.assertRaisesRegex(
                runner.Failure, "MediaStore lists no new movie"):
            self.run_step(runner.check_video, plan, guest)

    def test_the_laptop_records_with_its_front_camera(self) -> None:
        plan = runner.make_plan("front", "laptop", "live")
        guest = FakeGuest(plan)
        self.run_step(runner.check_video, plan, guest)
        self.assertIn(f"am start -a {runner.VIDEO_ACTION} --ez {runner.FRONT_EXTRA} true", guest.commands)

    def test_the_front_step_opens_camera_1_and_takes_a_still(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        guest = FakeGuest(plan)
        self.run_step(runner.check_front, plan, guest)
        self.assertIn(f"am start -a android.media.action.STILL_IMAGE_CAMERA --ez {runner.FRONT_EXTRA} true",
                      guest.commands)
        self.assertEqual(guest.commands.count(f"input keyevent {runner.CAMERA_KEYCODE}"), 1)

    def test_the_front_step_fails_when_the_application_holds_the_back_camera(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        with mock.patch.object(runner, "PREVIEW_SECONDS", 0.2), self.assertRaisesRegex(
                runner.Failure, r"holding camera 1 \(front\).*it holds \['0'\]"):
            self.run_step(runner.check_front, plan, FakeGuest(plan, front_holds=False))

    def test_a_front_only_profile_has_no_second_facing(self) -> None:
        plan = runner.make_plan("front", "laptop", "live")
        guest = FakeGuest(plan)
        self.run_step(runner.check_front, plan, guest)
        self.assertEqual(guest.commands, [])

    def test_the_hal_log_check(self) -> None:
        guest = FakeGuest(None)
        runner.check_hal_log(guest)
        guest.log = HAL_LOG
        with self.assertRaisesRegex(runner.Failure, "5 failed buffer lines; the first: .*every buffer"):
            runner.check_hal_log(guest)

    def test_the_new_steps_run_in_live_mode_only(self) -> None:
        source = SCRIPT.read_text()
        run_checks = source[source.index("def run_checks"):source.index("def cameraserver_restart_skip")]
        self.assertRegex(run_checks, r'if plan.mode == "live":\n\s+check_video\(guest, plan\)\n\s+check_front\(guest, plan\)\n'
                         r'\s+check_hal_log\(guest\)')


if __name__ == "__main__":
    unittest.main()
