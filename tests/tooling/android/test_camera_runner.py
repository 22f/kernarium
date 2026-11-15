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
import itertools
from pathlib import Path
import re
import struct
import sys
import tempfile
import threading
import time
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
        # Every host is given the small tier for now, so a layout of the large tier asks the launcher for it.
        self.assertEqual(runner.launcher_arguments(runner.make_plan("bf-large-flash", "phone", "live")),
                         ["--camera=synthetic", "--camera-profile=phone",
                          "androidboot.product.vendor.sku=bf-large-flash", "--camera-tier=large"])
        self.assertEqual(runner.launcher_arguments(runner.make_plan("bf-small-flash", "phone", "live")),
                         ["--camera=synthetic", "--camera-profile=phone",
                          "androidboot.product.vendor.sku=bf-small-flash"])
        self.assertNotIn("--camera-tier=large",
                         runner.launcher_arguments(runner.make_plan("front", "laptop", "blocked")))
        # With auto the launcher composes the layout and no SKU is passed.
        self.assertEqual(runner.launcher_arguments(runner.make_plan("bf-small-flash", "phone", "live"), auto=True),
                         ["--camera=synthetic", "--camera-profile=phone", "--camera-layout=auto"])
        self.assertEqual(runner.launcher_arguments(runner.make_plan("bf-small-flash", "phone", "live"), auto=True,
                                                   calibration=Path("/c"))[-1],
                         "--camera-calibration-dir=/c")
        self.assertEqual(runner.launcher_arguments(runner.make_plan("front", "laptop", "blocked"))[0],
                         "--camera=blocked")
        self.assertEqual(runner.launcher_arguments(runner.make_plan("none", "tablet", "none"))[0],
                         "--camera=none")
        # The launcher refuses a camera profile without a camera.
        self.assertNotIn("--camera-profile=tablet",
                         runner.launcher_arguments(runner.make_plan("none", "tablet", "none")))

    def test_the_composed_layout_has_raw_only_once_a_still_taught_the_colour_data(self) -> None:
        # Every host composes the small tier for now: no 2160p front camera, and no concurrent pair.
        self.assertEqual(runner.auto_layout("phone", taught=False), "bf-small-flash-manual")
        self.assertEqual(runner.auto_layout("phone", taught=True), "bf-small-flash-manual-raw")
        self.assertEqual(runner.auto_layout("tablet", taught=False), "bf-small-flash-manual")
        self.assertEqual(runner.auto_layout("tablet", taught=True), "bf-small-flash-manual-raw")
        self.assertEqual(runner.auto_layout("laptop", taught=False), "front")
        for profile in runner.PROFILE_CAMERAS:
            for taught in (False, True):
                name = runner.auto_layout(profile, taught)
                self.assertNotIn("large", name)
                runner.make_plan(name, profile, "live")

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
        self.assertIn('valued("--camera-tier=")', source)

    def test_the_sku_argument_becomes_the_property_the_trigger_reads(self) -> None:
        self.assertEqual(layouts.SKU_BOOT_ARGUMENT, "androidboot." + layouts.SKU_PROPERTY.removeprefix("ro.boot.") + "=")
        rc = (REPOSITORY / "android/lineage-24/overlay/camera/generated" / layouts.INIT_FILE).read_text(encoding="utf-8")
        self.assertIn(f"on property:{layouts.SKU_PROPERTY}=bf-large", rc)

    def test_the_parser_defaults(self) -> None:
        options = runner.build_parser().parse_args([])
        self.assertEqual((options.mode, options.camera_profile, options.layout), ("live", "phone", None))
        self.assertEqual(options.adb_port, runner.slot_ports.slot_port(runner.slot_ports.ADB_OFFSET, 5580))
        self.assertEqual(options.switch_rounds, 4)
        self.assertIsNone(options.audio_hold_library)

    def test_the_switch_rounds_are_a_positive_whole_number(self) -> None:
        parser = runner.build_parser()
        self.assertEqual(parser.parse_args(["--switch-rounds", "12"]).switch_rounds, 12)
        self.assertEqual(parser.parse_args(["--switch-rounds", "1"]).switch_rounds, 1)
        for bad in ("0", "-3", "x", "2.5", ""):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit, msg=bad):
                parser.parse_args(["--switch-rounds", bad])
        layout = "bf-large-flash-manual-concurrent-front2160"
        self.assertEqual(runner.make_plan(layout, "phone", "live").switch_rounds, 4)
        self.assertEqual(runner.make_plan(layout, "phone", "live", 12).switch_rounds, 12)
        with self.assertRaisesRegex(ValueError, "at least one"):
            runner.make_plan(layout, "phone", "live", 0)


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


# A log across a photo to video switch: the denial and the refusal that the camera service logs when binder will
# not hand the display composer's fence to the camera HAL (the lines have the layout logcat prints), the events
# of the switch, and lines that share a word with the denial or the refusal and are neither.
SWITCH_DENIAL = ("11-13 03:50:46.961   495   495 I auditd  : type=1400 audit(0.0:137): avc:  denied  { use } for "
                 'comm="C3Dev-0-ReqQueu" path="anon_inode:sync_file" dev="anon_inodefs" ino=5 '
                 "scontext=u:r:hal_camera_default:s0 tcontext=u:r:hal_graphics_composer_default:s0 tclass=fd "
                 "permissive=0")
SWITCH_REFUSAL = ("11-13 03:50:46.970   495  2263 E Camera3-Device: Camera 0: sendRequestsBatch: RequestThread: "
                  "Unable to submit capture request 1494 to HAL device: Broken pipe (-32)")
SWITCH_PAUSED = ("11-13 03:50:46.900  2180  2180 I wm_on_paused_called: "
                 "[87926199,org.lineageos.aperture.CameraActivity,performPause,6]")
SWITCH_RESUMED = ("11-13 03:50:47.100  2180  2180 I wm_on_resume_called: "
                  "[157384853,org.lineageos.aperture.VideoCamera,RESUME_ACTIVITY,2]")
# What a correct image logs around a switch and a force-stop (the two Camera3-OutputStream lines are cameraserver's
# own, from a stream whose application was stopped), and denials that are not the camera HAL's use of the
# composer's descriptors.
SWITCH_QUIET = """\
11-13 03:50:46.900   495  2263 D Camera3-Device: Camera 0: processCaptureRequest: request 1493 submitted
11-13 03:50:46.910   215   230 I auditd  : type=1400 audit(0.0:138): avc:  denied  { read } for comm="dmesg" \
scontext=u:r:shell:s0 tcontext=u:r:kernel:s0 tclass=system permissive=0
11-13 03:50:46.920   215   230 I auditd  : type=1400 audit(0.0:139): avc:  granted  { use } for \
scontext=u:r:hal_camera_default:s0 tcontext=u:r:surfaceflinger:s0 tclass=fd
11-13 03:50:46.930  1200  1212 W adbd    : Failed to write while dumping service activity: Broken pipe
11-13 03:50:46.940  1200  1212 E adbd    : write failed: Broken pipe (-32)
11-13 03:50:46.950   495  2263 I Camera3-Device: Camera 0: stopped: Broken pipe (-32)
11-13 05:21:55.914   491  1603 E Camera3-OutputStream: returnBufferCheckedLocked: Stream 15: Error queueing buffer to native window: Broken pipe (-32)
11-13 05:21:55.914   491  2366 E Camera3-OutputStream: getBufferLockedCommon: Stream 15: Can't dequeue next output buffer: Broken pipe (-32)
11-13 05:21:56.111   491   566 W Camera3-OutputStream: disconnectLocked: While disconnecting stream 15 from native window, the native window died from under us
11-13 03:50:46.970  5000  5001 E CameraUploader: network write failed: Broken pipe (-32)
11-13 03:50:46.971  5000  5001 E Other-Device: Unable to submit capture request 3 to HAL device: Broken pipe (-32)
"""
SWITCH_UNRELATED_DENIALS = (
    "11-13 03:50:46.970  5000  5001 I auditd: avc: denied { ptrace } for scontext=u:r:untrusted_app:s0 "
    "tcontext=u:r:hal_camera_default:s0 tclass=process permissive=0",
    "11-13 03:50:46.971   495   495 I auditd  : type=1400 audit(0.0:140): avc:  denied  { read } for "
    'comm="android.hardwar" name="foo" scontext=u:r:hal_camera_default:s0 tcontext=u:object_r:vendor_file:s0 '
    "tclass=file permissive=0",
    "11-13 03:50:46.972   495   495 I auditd  : type=1400 audit(0.0:141): avc:  denied  { use } for "
    "scontext=u:r:hal_camera_default:s0 tcontext=u:r:surfaceflinger:s0 tclass=fd permissive=0",
    "11-13 03:50:46.973   495   495 I auditd  : type=1400 audit(0.0:142): avc:  denied  { use } for "
    "scontext=u:r:cameraserver:s0 tcontext=u:r:hal_graphics_composer_default:s0 tclass=fd permissive=0",
)


MEDIA_SCALE = 90000


def edit_list(entries: list[tuple[int, int]]) -> bytes:
    """An `edts` box of one version 0 `elst`: (segment duration in the movie's time scale, media time) entries."""
    body = struct.pack(">BxxxI", 0, len(entries)) + b"".join(struct.pack(">IiI", segment, media_time, 0x10000)
                                                            for segment, media_time in entries)
    return box(b"edts", box(b"elst", body))


def audio_track(aac_frames: int, stretch: float = 1.0, rate: int = 48000, entry: bool = True,
                samples: bool = True, jump: tuple[int, float] | None = None,
                edit: tuple[float, float] | None = None) -> bytes:
    """An audio `trak` box as the recorder writes it: its `mdhd` time scale is the sample rate, its `stts` has one
    entry of `aac_frames` samples, each lasting `stretch` times the 1024 audio samples of an AAC frame, and, with
    `entry`, its `stsd` has an `mp4a` sample entry that states the sample rate. With `jump` = (index, seconds) the
    sample at `index` lasts that many seconds longer (a step in the sample times); with `edit` = (empty seconds,
    skipped seconds) an edit list starts the track after an empty edit and skips the start of its media."""
    delta = round(1024 * stretch)
    mdhd = struct.pack(">B3xIIII", 0, 0, 0, rate, aac_frames * delta) + b"\x00" * 4
    handler = box(b"hdlr", b"\x00\x00\x00\x00\x00\x00\x00\x00soun" + b"\x00" * 12)
    entries = [(aac_frames, delta)]
    if jump is not None:
        index, seconds = jump
        entries = [(index, delta), (1, delta + round(seconds * rate)), (aac_frames - index - 1, delta)]
    stts = box(b"stts", struct.pack(">BxxxI", 0, len(entries)) + b"".join(struct.pack(">II", c, d) for c, d in entries))
    # The fixed part of an audio sample entry: six reserved bytes and the data reference index, the version,
    # revision and vendor, the channel count, sample size and two reserved fields, and the 16.16 sample rate.
    mp4a = box(b"mp4a", b"\x00" * 6 + struct.pack(">H", 1) + b"\x00" * 8 + struct.pack(">HHHH", 1, 16, 0, 0)
               + struct.pack(">I", rate << 16))
    stsd = box(b"stsd", struct.pack(">BxxxI", 0, 1) + mp4a) if entry else b""
    stbl = box(b"stbl", stsd + stts) if samples else b""
    edts = b""
    if edit is not None:
        empty, skipped = edit
        edts = edit_list([(round(empty * 1000), -1), (1_000_000, round(skipped * rate))])
    return box(b"trak", edts + box(b"mdia", box(b"mdhd", mdhd) + handler + (box(b"minf", stbl) if samples else b"")))


def movie(seconds: float, scale: int = 1000, video: bool = True, version: int = 0, fps: float = 30.0,
          samples: bool = True, audio: bytes = b"", video_jump: tuple[int, float] | None = None) -> bytes:
    """A finished movie: `mvhd`, and one track whose `mdhd` has MEDIA_SCALE ticks a second and, for a video
    track with `samples`, an `stts` of one entry of seconds * fps frames (`video_jump` = (index, seconds): the frame
    at `index` lasts that many seconds longer, a freeze); with `audio` (an `audio_track`) a second track follows."""
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
    entries = [(frames, delta)]
    if video_jump is not None:
        index, extra = video_jump
        entries = [(index, delta), (1, delta + round(extra * MEDIA_SCALE)), (frames - index - 1, delta)]
    stts = box(b"stts", struct.pack(">BxxxI", 0, len(entries)) + b"".join(struct.pack(">II", c, d) for c, d in entries))
    track = box(b"mdia", box(b"mdhd", mdhd) + handler + (box(b"minf", box(b"stbl", stts)) if video and samples else b""))
    return (box(b"ftyp", b"isom\x00\x00\x02\x00isom") + box(b"mdat", b"\x00" * 40)
            + box(b"moov", box(b"mvhd", mvhd) + box(b"trak", track) + audio))


def sound(seconds: float = 3.5, stretch: float = 1.0, rate: int = 48000, **options) -> bytes:
    """An audio track that holds `seconds` of audio under a time line of `stretch` times as much."""
    return audio_track(round(seconds * rate / 1024), stretch, rate, **options)


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

    def test_the_audio_track_of_a_movie(self) -> None:
        info = runner.parse_mp4(movie(4.0, audio=sound(3.0)))
        self.assertTrue(info.audio)
        self.assertEqual((info.audio_samples, info.audio_rate), (141, 48000))
        self.assertAlmostEqual(info.audio_coded_seconds, 3.008, places=3)
        self.assertAlmostEqual(info.audio_seconds, 3.008, places=3)
        self.assertAlmostEqual(info.audio_stretch, 1.0, places=3)
        # The video track's own numbers are those of a movie without audio.
        self.assertEqual((info.frames, info.video_seconds), (120, 4.0))
        none = runner.parse_mp4(movie(4.0))
        self.assertEqual((none.audio, none.audio_samples, none.audio_rate, none.audio_stretch), (False, 0, 0, 0.0))

    def test_a_stamped_time_without_audio_behind_it_is_the_stretch(self) -> None:
        for stretch in (1.0, 1.29, 1.39, 4.74):
            with self.subTest(stretch=stretch):
                info = runner.parse_mp4(movie(5.0, audio=sound(3.0, stretch)))
                self.assertAlmostEqual(info.audio_coded_seconds, 3.008, places=3)
                self.assertAlmostEqual(info.audio_seconds, 3.008 * stretch, places=2)
                self.assertAlmostEqual(info.audio_stretch, stretch, places=3)

    def test_where_the_tracks_end_and_their_longest_steps(self) -> None:
        info = runner.parse_mp4(movie(4.0, audio=sound(4.0)))
        self.assertAlmostEqual(info.video_end, 4.0, places=3)
        self.assertAlmostEqual(info.audio_end, 4.0107, places=3)
        self.assertAlmostEqual(info.audio_skew, 0.0107, places=3)
        self.assertAlmostEqual(info.video_gap, 1 / 30, places=3)
        self.assertAlmostEqual(info.audio_gap, 1024 / 48000, places=4)
        # A step in the sample times is the longest duration of one sample, and adds to the end.
        jumped = runner.parse_mp4(movie(5.5, fps=15.0, audio=sound(4.0, jump=(100, 1.5)), video_jump=(30, 1.4)))
        self.assertAlmostEqual(jumped.audio_gap, 1.5213, places=3)
        self.assertAlmostEqual(jumped.video_gap, 1.4667, places=3)
        self.assertAlmostEqual(jumped.audio_end, 4.0107 + 1.5, places=3)
        self.assertAlmostEqual(jumped.video_end, 82 * 6000 / 90000 + 1.4, places=3)
        # Without a sample table, or a track, there is nothing to end.
        self.assertEqual(runner.parse_mp4(movie(4.0, samples=False)).video_end, 0.0)
        self.assertEqual((runner.parse_mp4(movie(4.0)).audio_end, runner.parse_mp4(movie(4.0)).audio_gap), (0.0, 0.0))

    def test_an_edit_list_moves_the_end_by_its_empty_edit_and_skip(self) -> None:
        # The movie's time scale is 1000: an empty edit of 0.5 s starts the track late, the next edit skips 0.02 s of
        # its media (an encoder's priming), so the track ends 0.48 s after its media's duration.
        info = runner.parse_mp4(movie(4.0, audio=sound(4.0, edit=(0.5, 0.02))))
        self.assertAlmostEqual(info.audio_end, 4.0107 + 0.5 - 0.02, places=3)
        self.assertAlmostEqual(info.audio_seconds, 4.0107, places=3)  # the stretch does not see edits
        plain = runner.parse_mp4(movie(4.0, audio=sound(4.0)))
        self.assertAlmostEqual(info.audio_end - plain.audio_end, 0.48, places=3)
        # Version 1 and an edit list with only a skip.
        only_skip = runner.parse_mp4(movie(4.0, audio=sound(4.0, edit=(0.0, 0.03))))
        self.assertAlmostEqual(only_skip.audio_end, 4.0107 - 0.03, places=3)

    def test_the_capture_of_the_audio_flinger_is_read_from_its_dump(self) -> None:
        self.assertEqual(runner.active_capture_frames(audio_flinger_dump("capturing", 12345)), 12345)
        for state in ("standby", "idle", "none"):
            with self.subTest(state=state):
                self.assertIsNone(runner.active_capture_frames(audio_flinger_dump(state, 12345)))
        # The history of a removed record thread (lines with a dash) and an output thread are not capture.
        self.assertIsNone(runner.active_capture_frames("- Input thread 0x1, name AudioIn_E, tid 523, type 3 (RECORD):\n"
                                                       "-   Standby: no\n-   Frames read: 5\n-   1 Tracks of which 1 are active\n"))
        self.assertIsNone(runner.active_capture_frames(""))
        # Two record threads: the one that reads the most is the capture.
        both = audio_flinger_dump("capturing", 100) + audio_flinger_dump("capturing", 900)
        self.assertEqual(runner.active_capture_frames(both), 900)

    def test_the_frames_the_input_made_up_are_read_from_its_log(self) -> None:
        log = ("11-14 21:40:01.000   200   300 W AHAL_StreamAlsaMonoPipe: readInput: the device delivers no frames in time, "
               "making silence up to keep to real time\n"
               "11-14 21:40:02.500   200   300 W AHAL_StreamAlsaMonoPipe: readInput: the device delivers again, after 67200 "
               "frames of silence made up\n"
               "11-14 21:40:09.000   200   300 W AHAL_StreamAlsaMonoPipe: readInput: dropped 480 frames since the last message\n"
               "11-14 21:40:12.000   200   300 W AHAL_StreamAlsaMonoPipe: readInput: the device delivers again, after 1024 "
               "frames of silence made up\n")
        self.assertEqual(runner.made_up_frames(log), [67200, 1024])
        self.assertEqual(runner.made_up_frames(""), [])

    def test_the_sample_rate_is_the_sample_entrys_and_else_the_time_scale(self) -> None:
        for rate in (44100, 48000):
            info = runner.parse_mp4(movie(4.0, audio=sound(3.0, rate=rate)))
            self.assertEqual(info.audio_rate, rate)
            self.assertAlmostEqual(info.audio_coded_seconds, info.audio_samples * 1024 / rate)
        # Without a sample entry the track's time scale, which the recorder sets to the sample rate, stands in.
        self.assertEqual(runner.parse_mp4(movie(4.0, audio=sound(3.0, rate=44100, entry=False))).audio_rate, 44100)
        # A sample entry too short to hold the rate is no entry.
        short = bytearray(movie(4.0, audio=sound(3.0, rate=44100)))
        at = bytes(short).index(b"mp4a") - 4
        short[at:at + 4] = struct.pack(">I", 20)
        self.assertEqual(runner.parse_mp4(bytes(short)).audio_rate, 44100)

    def test_an_audio_track_without_a_sample_table_has_no_stretch(self) -> None:
        info = runner.parse_mp4(movie(4.0, audio=sound(3.0, samples=False)))
        self.assertTrue(info.audio)
        self.assertEqual((info.audio_samples, info.audio_seconds, info.audio_rate, info.audio_stretch), (0, 0.0, 0, 0.0))

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


def audio_flinger_dump(state: str, frames_read: int) -> str:
    """The parts of `dumpsys media.audio_flinger` that the capture check reads: a record thread in the state
    `capturing` (out of standby, one active client), `standby`, `idle` (out of standby, no client) or `none` (no record
    thread), an output thread that the check must not take for it, and the history of a removed record thread."""
    standby = "yes" if state == "standby" else "no"
    clients = "0 Tracks of which 0 are active" if state in ("standby", "idle") else "1 Tracks of which 1 are active"
    record = ("Input thread 0xb4007e6502f60a38, name AudioIn_1E, tid 2500, type 3 (RECORD):\n"
              f"  Standby: {standby}\n  Sample rate: 48000 Hz\n  Frames read: {frames_read}\n  {clients}\n"
              "    Active     Id Client(pid/uid) Session Port Id  S  Flags   Format Chn mask  SRate Source\n"
              "  11-14 12:15:31.604 standby\n") if state != "none" else ""
    return ("Output thread 0xb4007e6502f60a30, name AudioOut_D, tid 2000, type 0 (MIXER):\n  Standby: no\n"
            "  1 Tracks of which 1 are active\n"
            + record +
            "- Input thread 0xb4007e6502f60a38, name AudioIn_E, tid 523, type 3 (RECORD):\n-   Standby: no\n"
            "-   Frames read: 99999999\n-   1 Tracks of which 1 are active\n"
            "Notes:\n  nothing else\n")


class AudioHoldTests(unittest.TestCase):
    """The runner's side of the audio hold library: a request, renamed to its name, and the library's log line."""

    def guest(self, directory: Path, library_lines: list[str] | None = None, alive: bool = True):
        process = mock.Mock(pid=4242)
        process.poll.return_value = None if alive else 1
        process.returncode = 1
        log = directory / "emulator.log"
        log.write_text("")
        return runner.Guest("adb", "127.0.0.1:1", process, log, directory, directory / "hold")

    def serve(self, directory: Path, answer: str) -> threading.Thread:
        """The library: takes the request file named `hold`, removes it and logs `answer` (formatted with the held ns)."""
        def run() -> None:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                request = directory / "hold" / "hold"
                if request.is_file():
                    asked = int(request.read_text())
                    request.unlink()
                    with (directory / "hold" / "holds").open("a") as log:
                        log.write(answer.format(asked=asked, held=asked * 1_000_000) + "\n")
                    return
                time.sleep(0.01)
        thread = threading.Thread(target=run)
        thread.start()
        return thread

    def test_a_hold_is_a_request_the_library_answers(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "hold").mkdir()
            guest = self.guest(directory)
            thread = self.serve(directory, "1 {held} 12345 1")
            self.assertAlmostEqual(guest.hold_audio(0.5), 0.5)
            thread.join()
            # Nothing is left to be read half written, and the next hold is the next line of the log.
            self.assertEqual(sorted(path.name for path in (directory / "hold").iterdir()), ["holds"])
            thread = self.serve(directory, "2 {held} 12346 1")
            self.assertAlmostEqual(guest.hold_audio(0.25), 0.25)
            thread.join()

    def test_a_hold_that_did_not_happen_is_a_failure(self) -> None:
        for answer, what in (("1 0 12345 0", r"held 0 thread\(s\)"), ("1 {held} 12345 2", r"held 2 thread\(s\)"),
                             ("1 100000000 12345 1", r"for 0\.10 s of the 0\.5 s asked")):
            with self.subTest(answer=answer), tempfile.TemporaryDirectory() as name:
                directory = Path(name)
                (directory / "hold").mkdir()
                thread = self.serve(directory, answer)
                with self.assertRaisesRegex(runner.Failure, what + r".*cannot judge the input"):
                    self.guest(directory).hold_audio(0.5)
                thread.join()

    def test_no_answer_and_no_library_and_a_dead_guest_fail_with_their_reasons(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "hold").mkdir()
            with mock.patch.object(runner, "AUDIO_HOLD_WAIT_SECONDS", 0.2), self.assertRaisesRegex(
                    runner.Failure, r"reported no hold within 0\.7 s \(requested 0\.5 s\)"):
                self.guest(directory).hold_audio(0.5)
            with self.assertRaisesRegex(runner.Failure, "the guest exited with status 1"):
                self.guest(directory, alive=False).hold_audio(0.5)
            bare = self.guest(directory)
            bare.hold_directory = None
            with self.assertRaisesRegex(runner.Failure, "no audio hold library: the run was not given --audio-hold-library"):
                bare.hold_audio(0.5)

    def test_the_library_is_inserted_into_the_launcher_the_runner_starts(self) -> None:
        options = runner.build_parser().parse_args(["--audio-hold-library", "build/uml_audio_hold"])
        self.assertEqual(options.audio_hold_library, Path("build/uml_audio_hold"))
        source = SCRIPT.read_text()
        start = source[source.index("def start_guest"):source.index("def positive_integer")]
        self.assertIn('environment["DYLD_INSERT_LIBRARIES"] = str(options.audio_hold_library.resolve())', start)
        self.assertIn('environment["UML_AUDIO_HOLD_DIRECTORY"] = str(hold_directory)', start)
        self.assertIn("env=environment", start)


class FakeGuest:
    """A guest that answers the runner's commands from a script and records them."""

    def __init__(self, plan, front_holds: bool = True, movies=None) -> None:
        self.plan = plan
        self.commands: list[str] = []
        # The scripted guest has no side keys: the sensor privacy step says so and asks it nothing.
        self.hardware_input = False
        self.held = {"0"}
        self.front_holds = front_holds
        self.rows: dict[str, dict[str, str]] = {"1": {"_id": "1", "_data": "/sdcard/old.mp4"}}
        self.recording = False
        self.movie = movies if movies is not None else movie(3.5, audio=sound(3.5))
        # The seconds for which the runner stopped the emulator process, each time it did.
        self.frozen: list[float] = []
        # The audio hold library: the seconds each hold of the sound device's thread was asked for, whether the
        # input then makes silence up (its log line), what the audio flinger's record thread shows, and the
        # input's own log.
        self.hold_directory: Path | None = Path("audio-hold")
        self.audio_held: list[float] = []
        self.makes_up = True
        self.capture_state = "capturing"
        self.audio_frames_read = 100000
        self.audio_looks = 0
        self.stream_text = ""
        self.still_files = {"IMG_old.jpg"}
        self.log = ""
        # The log of a mode switch step. The video mode's start by round number n logs the events of the switch
        # (events[n] = (photo paused, video resumed), both by default) and then switch_logs[n]; the nth read of
        # the log is followed by late_logs[n] (lines that land after it) and, in wrap_after_read, by a log that
        # holds nothing any more.
        self.full_log = "11-13 03:50:00.000   495   495 I Camera3-Device: before any switch\n"
        self.switch_logs: dict[int, str] = {}
        self.events: dict[int, tuple[bool, bool]] = {}
        self.late_logs: dict[int, str] = {}
        self.wrap_after_read: set[int] = set()
        self.no_markers = False
        self.log_reads = 0
        self.video_starts = 0
        # The camera service's frame counters: they rise on every read except in the phases named here.
        self.stalled: set[str] = set()
        # The phases ("photo", "video") in which the application opens no camera.
        self.no_camera_in: set[str] = set()
        self.in_video = False
        self.frames = 1000
        # Commands that fail, as a guest command that exits non-zero does.
        self.failing: set[str] = set()
        # The global settings (the window animation scales) and what they were at each start of the video mode.
        self.settings: dict[str, str] = {}
        self.settings_at_video_start: list[dict[str, str]] = []
        chart = [bytes(value for x in range(40) for value in ((255,) * 4 if (x // 5 + y // 5) % 2 else (20, 20, 20, 255)))
                 for y in range(40)]
        self.chart = runner.decode_png(png(40, 40, 6, chart, [0]))

    def alive(self) -> None:
        pass

    def shell(self, command: str, timeout: float = 0) -> str:
        self.commands.append(command)
        if command in self.failing:
            raise runner.Failure(f"'{command}' exited 1: injected")
        if command.startswith("am start") and "USE_FRONT_CAMERA true" in command:
            # The laptop's only camera is the front one, camera 0.
            front = "0" if self.plan.profile == "laptop" else "1"
            self.held = {front} if self.front_holds else {"0"}
        elif command.startswith("am start"):
            self.held = set() if ("video" if "VIDEO_CAMERA" in command else "photo") in self.no_camera_in else {"0"}
        if command.startswith(f"log -t {runner.SWITCH_MARKER_TAG} '") and not self.no_markers:
            marker = command.split("'")[1]
            number = int(re.search(r"switch (\d+) of", marker).group(1))
            self.full_log += f"11-13 03:50:46.800   100   100 I {runner.SWITCH_MARKER_TAG}: {marker}\n"
        if command == "logcat -d -b all":
            self.log_reads += 1
            text = self.full_log
            self.full_log += self.late_logs.get(self.log_reads, "")
            if self.log_reads in self.wrap_after_read:
                self.full_log = ""
            return text
        if command.startswith("settings get global "):
            return self.settings.get(command.rsplit(" ", 1)[1], "null") + "\n"
        if command.startswith("settings put global "):
            _, _, _, key, value = command.split(" ")
            self.settings[key] = value
        if command.startswith("settings delete global "):
            self.settings.pop(command.rsplit(" ", 1)[1], None)
        if command.startswith("am start") and "VIDEO_CAMERA" in command:
            self.settings_at_video_start.append(dict(self.settings))
            self.video_starts += 1
            self.in_video = True
            paused, resumed = self.events.get(self.video_starts, (True, True))
            self.full_log += (SWITCH_PAUSED + "\n" if paused else "") + (SWITCH_RESUMED + "\n" if resumed else "")
            self.full_log += self.switch_logs.get(self.video_starts, "")
        elif command.startswith("am start") or command.startswith("am force-stop"):
            self.in_video = False
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
        if ("video" if self.in_video else "photo") not in self.stalled:
            self.frames += 25
        return ("Active Camera Clients:\n" + clients + "Active Client listener PIDs: 1\n"
                f"    Stream[3]: Output\n      Frames produced: {self.frames}, last timestamp: 1 ns\n")

    def screenshot(self):
        return self.chart

    def freeze(self, seconds: float) -> None:
        self.frozen.append(seconds)

    def hold_audio(self, seconds: float) -> float:
        self.audio_held.append(seconds)
        if self.makes_up:
            self.stream_text += ("11-14 21:40:01.000   200   300 W AHAL_StreamAlsaMonoPipe: readInput: the device delivers "
                                 f"again, after {round((seconds - 0.1) * 48000)} frames of silence made up\n")
        return seconds

    def audio_flinger(self) -> str:
        self.audio_looks += 1
        # The record thread reads more between two looks while it captures; "stuck" is a client that reads nothing.
        return audio_flinger_dump("capturing" if self.capture_state == "stuck" else self.capture_state,
                                  self.audio_frames_read + (4096 * self.audio_looks if self.capture_state == "capturing" else 0))

    def stream_log(self) -> str:
        return self.stream_text

    def videos(self):
        return dict(self.rows), "Row: 0 ..."

    def stills(self) -> set[str]:
        return set(self.still_files)

    def hal_log(self) -> str:
        return self.log

    def pull(self, remote: str) -> bytes:
        return jpeg(4000, 3000) if remote.endswith(".jpg") else self.movie


CAMERA_RESUMED = "topResumedActivity=ActivityRecord{1a2b3c u0 org.lineageos.aperture/.CameraActivity t12}"
CAMERA_FOCUS = "mCurrentFocus=Window{4d5e6f u0 org.lineageos.aperture/org.lineageos.aperture.CameraActivity}"


def foreground_text(resumed: str = CAMERA_RESUMED, focus: str = CAMERA_FOCUS, wakefulness: str = "Awake") -> str:
    return f"  {resumed}\n  {focus}\n  mWakefulness={wakefulness}\n"


class BlockedSamplerTests(unittest.TestCase):
    """A blocked preview sample counts only while the camera application is the resumed activity and the
    device is awake."""

    BLACK, PICTURE = (0.0, 0.0), (128.2, 21.5)
    FRONT = runner.parse_foreground(foreground_text())
    LAUNCHER = runner.parse_foreground(foreground_text(
        "topResumedActivity=ActivityRecord{7a8b9c u0 org.lineageos.trebuchet/.Launcher t1}",
        "mCurrentFocus=Window{9d8e7f u0 org.lineageos.trebuchet/org.lineageos.trebuchet.Launcher}"))
    ASLEEP = runner.parse_foreground(foreground_text(wakefulness="Asleep"))

    def sampler(self, states: list, values: list, calls: dict):
        """A sampler over scripted foreground states and sample values; `calls` records the screenshots, the
        bring-backs and the settle sleeps."""
        shown, taken = iter(states), iter(values)
        calls.update(screenshots=0, brought=0, slept=[])

        def take():
            calls["screenshots"] += 1
            return next(taken)

        def bring_back():
            calls["brought"] += 1

        return runner.BlockedSampler(lambda: next(shown), take, bring_back, calls["slept"].append)

    def test_the_foreground_is_read_from_the_guest_dump(self) -> None:
        front = runner.parse_foreground(foreground_text())
        self.assertEqual((front.resumed, front.wakefulness), ("org.lineageos.aperture/.CameraActivity", "Awake"))
        self.assertEqual(front.focus, "org.lineageos.aperture/org.lineageos.aperture.CameraActivity")
        self.assertTrue(front.camera_in_front())
        older = runner.parse_foreground(foreground_text(
            "mResumedActivity: ActivityRecord{1a2b3c u0 org.lineageos.aperture/.CameraActivity t12}"))
        self.assertTrue(older.camera_in_front())
        both = runner.parse_foreground("ResumedActivity: ActivityRecord{1 u0 other.app/.Main t2}\n"
                                       + foreground_text())
        self.assertEqual(both.resumed, "org.lineageos.aperture/.CameraActivity")
        self.assertFalse(self.LAUNCHER.camera_in_front())
        self.assertFalse(self.ASLEEP.camera_in_front())
        nothing = runner.parse_foreground("")
        self.assertEqual((nothing.resumed, nothing.focus, nothing.wakefulness), ("", "", ""))
        self.assertFalse(nothing.camera_in_front())
        # A package that only starts like the camera application's is not it.
        self.assertFalse(runner.parse_foreground(foreground_text(
            "topResumedActivity=ActivityRecord{1 u0 org.lineageos.aperture.other/.Main t2}")).camera_in_front())

    def test_in_front_and_black_passes(self) -> None:
        calls: dict = {}
        sampler = self.sampler([self.FRONT] * 4, [self.BLACK, self.BLACK], calls)
        self.assertIsNone(runner.blocked_preview(sampler, lambda seconds: None))
        self.assertEqual((calls["screenshots"], calls["brought"]), (2, 0))

    def test_in_front_and_a_picture_fails_the_picture_check(self) -> None:
        calls: dict = {}
        sampler = self.sampler([self.FRONT] * 4, [self.BLACK, self.PICTURE], calls)
        self.assertEqual(runner.blocked_preview(sampler, lambda seconds: None),
                         "the preview shows a picture (mean 128.2, deviation 21.5, sample 2)")
        self.assertEqual(calls["brought"], 0)

    def test_not_in_front_twice_is_its_own_failure(self) -> None:
        for gone in (self.LAUNCHER, self.ASLEEP):
            with self.subTest(gone=gone):
                calls: dict = {}
                sampler = self.sampler([gone, gone], [], calls)
                out = io.StringIO()
                with contextlib.redirect_stdout(out), self.assertRaises(runner.Failure) as raised:
                    runner.blocked_preview(sampler, lambda seconds: None)
                message = str(raised.exception)
                self.assertIn("a blocked session cannot be judged: the camera application is not in front", message)
                self.assertIn(f"resumed activity {gone.resumed}", message)
                self.assertIn(f"focused window {gone.focus}", message)
                self.assertIn(f"wakefulness {gone.wakefulness}", message)
                self.assertEqual((calls["screenshots"], calls["brought"]), (0, 1))
                self.assertEqual(calls["slept"], [runner.BRING_BACK_SETTLE_SECONDS])

    def test_nothing_known_about_the_foreground_is_a_failure_not_a_pass(self) -> None:
        calls: dict = {}
        empty = runner.parse_foreground("")
        sampler = self.sampler([empty, empty], [self.BLACK], calls)
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(
                runner.Failure, r"resumed activity none, focused window none, wakefulness unknown"):
            runner.blocked_preview(sampler, lambda seconds: None)
        self.assertEqual(calls["screenshots"], 0)

    def test_brought_back_once_and_sampled_again(self) -> None:
        calls: dict = {}
        sampler = self.sampler([self.LAUNCHER, self.FRONT, self.FRONT, self.FRONT, self.FRONT], [self.BLACK, self.BLACK], calls)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertIsNone(runner.blocked_preview(sampler, lambda seconds: None))
        self.assertIn("the camera application is not in front (resumed activity org.lineageos.trebuchet/.Launcher", out.getvalue())
        self.assertEqual((calls["screenshots"], calls["brought"]), (2, 1))

    def test_an_application_that_leaves_during_the_screenshot_does_not_count(self) -> None:
        calls: dict = {}
        # In front before the first screenshot, gone after it (that picture is dropped), back and in front after bring-back.
        sampler = self.sampler([self.FRONT, self.LAUNCHER, self.FRONT, self.FRONT, self.FRONT, self.FRONT],
                               [self.PICTURE, self.BLACK, self.BLACK], calls)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertIsNone(runner.blocked_preview(sampler, lambda seconds: None))
        self.assertEqual((calls["screenshots"], calls["brought"]), (3, 1))

    def test_the_bring_back_is_once_for_the_whole_step(self) -> None:
        calls: dict = {}
        # Sample 1 needs the bring-back; the application is gone again at sample 2: the step fails, no second bring-back.
        sampler = self.sampler([self.LAUNCHER, self.FRONT, self.FRONT, self.LAUNCHER], [self.BLACK], calls)
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(runner.Failure, "cannot be judged"):
            runner.blocked_preview(sampler, lambda seconds: None)
        self.assertEqual((calls["screenshots"], calls["brought"]), (1, 1))

    def test_check_preview_asks_the_guest_and_starts_the_application_again(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "blocked")
        guest = FakeGuest(plan)
        black = runner.decode_png(png(40, 40, 6, [bytes((0, 0, 0, 255)) * 40] * 40, [0]))
        guest.screenshot = lambda: black
        answers = iter([foreground_text(CAMERA_RESUMED.replace("aperture", "trebuchet"), wakefulness="Awake")]
                       + [foreground_text()] * 4)
        base_shell = guest.shell

        def shell(command: str, timeout: float = 0) -> str:
            if command == runner.FOREGROUND_COMMAND:
                guest.commands.append(command)
                return next(answers)
            return base_shell(command, timeout)

        guest.shell = shell
        out = io.StringIO()
        with mock.patch.object(runner.time, "sleep", lambda seconds: None), contextlib.redirect_stdout(out):
            runner.check_preview(guest, plan)
        starts = [command for command in guest.commands if command.startswith("am start")]
        self.assertEqual(len(starts), 2)
        self.assertIn("bringing it back", out.getvalue())
        # The application's own events are read before the second start, and printed with the line that says it left.
        events = [index for index, command in enumerate(guest.commands) if "FATAL EXCEPTION" in command]
        second_start = [index for index, command in enumerate(guest.commands) if command.startswith("am start")][1]
        self.assertEqual(len(events), 1)
        self.assertLess(events[0], second_start)
        self.assertIn("events of the camera application and its windows:", out.getvalue())
        self.assertIn("blocked preview (2 samples): mean 0.0, deviation 0.0", out.getvalue())


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
        # Every screenshot allocates a buffer, and each allocation logs the allocator's start-up lines; with them
        # the last 120 lines of a failed run held nothing of the camera.
        for flood in ("minigbm:virtgpu_init", "minigbm:virgl_get_caps", "minigbm:virgl_add_combination"):
            self.assertIn(flood, commands[0])

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

    def test_what_was_in_front_is_printed_after_the_log(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "blocked")
        guest = FakeGuest(plan)
        commands: list[str] = []
        answers = ["E CameraService: a line\n",
                   "topResumedActivity=ActivityRecord{x u0 org.lineageos.aperture/.CameraActivity}\n",
                   "I ActivityTaskManager: Displayed org.lineageos.aperture\n",
                   "E Aperture: its own line, which does not name the package\n"]
        guest.shell = lambda command, timeout=0: (commands.append(command), answers[len(commands) - 1])[1]
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            runner.print_camera_log(guest)
        text = err.getvalue()
        self.assertLess(text.index("camera log lines of the guest"), text.index("in front at the failure"))
        self.assertLess(text.index("in front at the failure"), text.index("events of the camera application"))
        self.assertIn("in front at the failure:\ntopResumedActivity=ActivityRecord{x u0 org.lineageos.aperture", text)
        self.assertIn("events of the camera application and its windows:\nI ActivityTaskManager", text)
        self.assertIn("dumpsys activity activities", commands[1])
        self.assertIn("mCurrentFocus", commands[1])
        for what in ("FATAL EXCEPTION", "ANR in", "am_crash", "am_anr", "am_proc_died", "org.lineageos.aperture"):
            self.assertIn(what, commands[2])
        # The application's own lines name no package: they come by process id.
        self.assertIn("the camera application's own log lines (by process id):\nE Aperture: its own line", text)
        self.assertIn("pidof org.lineageos.aperture", commands[3])
        self.assertIn("logcat -d -b all --pid=$pid", commands[3])
        self.assertIn("grep -a -v -E", commands[3])

    def test_an_application_without_a_process_has_no_own_lines(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "blocked")
        guest = FakeGuest(plan)
        guest.shell = lambda command, timeout=0: ""
        out = io.StringIO()
        runner.print_application_events(guest, out)
        self.assertIn("the camera application's own log lines (by process id):\n(none: no process)", out.getvalue())

    def test_a_guest_lost_after_the_log_keeps_the_log(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "blocked")
        guest = FakeGuest(plan)
        calls: list[str] = []

        def second_fails(command: str, timeout: float = 0) -> str:
            calls.append(command)
            if len(calls) == 2:
                raise OSError("adb: device offline")
            return "E CameraService: a line\n"

        guest.shell = second_fails
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            runner.print_camera_log(guest)
        self.assertIn("E CameraService: a line", err.getvalue())
        self.assertIn("no foreground state: adb: device offline", err.getvalue())
        self.assertIn("events of the camera application and its windows:", err.getvalue())


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
            self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(4.0, fps=29.97, audio=sound(4.0))))
        self.assertRegex(out.getvalue(), r"video: new\.mp4 4\.00 s, 119 frames over 3\.97 s = 30\.0 fps average")
        self.assertRegex(out.getvalue(), r"audio: 188 AAC frames = 4\.01 s of audio over a 4\.01 s time line at 48000 Hz, "
                                         r"stretch 1\.000")

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

    def test_the_audio_of_a_recording_must_hold_what_its_time_stamps_span(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        for stretch in (0.96, 1.0, 1.007, 1.05):
            with self.subTest(stretch=stretch):
                self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(3.0, audio=sound(3.0, stretch))))
        for stretch in (1.06, 1.29, 1.39, 4.74):
            out = io.StringIO()
            with self.subTest(stretch=stretch), contextlib.redirect_stdout(out), self.assertRaisesRegex(
                    runner.Failure, rf"holds 3\.01 s of audio under a [0-9.]+ s time line \(stretch {stretch}\d*, above 1\.05\)"
                                    r".*plays the clip too fast"):
                self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(3.0, audio=sound(3.0, stretch))))
            self.assertIn(f"stretch {stretch}", out.getvalue())  # printed before the check
        # The other way: a time line shorter than the audio it holds is as wrong.
        for stretch in (0.94, 0.7):
            with self.subTest(stretch=stretch), self.assertRaisesRegex(
                    runner.Failure, rf"stretch {stretch}\d*, below 0\.95\): more audio than its time stamps span"):
                self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(3.0, audio=sound(3.0, stretch))))
        self.assertEqual((runner.AUDIO_STRETCH_MINIMUM, runner.AUDIO_STRETCH_MAXIMUM), (0.95, 1.05))

    def test_a_jump_in_the_audio_table_fails_even_when_the_stretch_is_small(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        # 12 s of audio with one step of 0.55 s: a stretch of 1.046, and a jump of more than half a second.
        data = movie(12.55, audio=sound(12.0, jump=(100, 0.55)))
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaisesRegex(
                runner.Failure, r"jumps 0\.57 s between two samples \(above 0\.5 s\)"):
            self.run_step(runner.check_video, plan, FakeGuest(plan, movies=data))
        self.assertRegex(out.getvalue(), r"stretch 1\.04\d; longest step of its sample times 0\.57\d* s")
        self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(12.4, audio=sound(12.0, jump=(100, 0.4)))))

    def test_the_audio_must_end_with_the_video(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        for seconds, what in ((3.0, r"ends \+1\.0\d s after the video"), (5.1, r"ends -1\.0\d s after the video")):
            with self.subTest(video=seconds), self.assertRaisesRegex(runner.Failure, what + r" \(more than 0\.25 s either way\)"):
                self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(seconds, audio=sound(4.0))))
        self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(4.2, audio=sound(4.0))))

    def test_a_recording_without_audio_or_its_sample_table_fails(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        for data, what in ((movie(4.0), "has no audio track"),
                           (movie(4.0, audio=sound(3.0, samples=False)), "no sample table for its audio track")):
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

    PHONE = "bf-large-flash-manual-concurrent-front2160"

    def stall_step(self, data, **changes):
        """The audio stall step on a guest that returns `data` as its recording; returns the guest and the output."""
        plan = runner.make_plan(self.PHONE, "phone", "live")
        guest = FakeGuest(plan, movies=data)
        for name, value in changes.items():
            setattr(guest, name, value)
        out = io.StringIO()
        try:
            with mock.patch.object(runner.time, "sleep", lambda seconds: None), contextlib.redirect_stdout(out):
                runner.check_audio_stall(guest, plan)
        except runner.Failure as failure:
            failure.guest, failure.out = guest, out.getvalue()
            raise
        return guest, out.getvalue()

    def test_the_stall_step_holds_the_sound_device_three_times_while_the_recording_runs(self) -> None:
        guest, out = self.stall_step(movie(12.0, audio=sound(12.0)))
        self.assertEqual(guest.audio_held, [runner.STALL_SECONDS] * runner.DISTURB_COUNT)
        self.assertEqual((runner.STALL_SECONDS, runner.DISTURB_COUNT), (1.5, 3))
        self.assertEqual(guest.frozen, [])  # the guest itself stands still never
        self.assertEqual(guest.commands.count(f"input keyevent {runner.CAMERA_KEYCODE}"), 2)
        self.assertRegex(out, r"audio capture runs: the record thread has read \d+, then \d+ frames")
        self.assertRegex(out, r"stall 1: the sound device's thread held 1\.50 s from \d+\.\d s after the start key")
        self.assertRegex(out, r"video with the sound device held 3 x 1\.5 s: new\.mp4 12\.00 s, 360 frames")
        self.assertRegex(out, r"the HAL made 201600 frames of silence up in 3 stall\(s\)")

    def test_the_stall_step_needs_the_audio_capture_before_it_holds_anything(self) -> None:
        # Checked before the first hold: a record thread in standby, without a client, absent or reading nothing
        # is a recording whose audio the stall could not have touched.
        for state in ("standby", "idle", "none", "stuck"):
            with self.subTest(state=state), mock.patch.object(runner, "AUDIO_CAPTURE_SECONDS", 0.05), \
                    mock.patch.object(runner.time, "monotonic", mock.Mock(side_effect=itertools.count(0, 0.02))):
                with self.assertRaisesRegex(runner.Failure, r"audio capture: .* \(after 0 s\)") as raised:
                    self.stall_step(movie(12.0, audio=sound(12.0)), capture_state=state)
                self.assertEqual(raised.exception.guest.audio_held, [])
        self.stall_step(movie(12.0, audio=sound(12.0)))

    def test_the_stall_step_fails_for_the_input_that_does_not_make_the_stall_up(self) -> None:
        # Three stalls of 1.5 s under an input that makes only its own frames up: 4.5 s of time line more than audio.
        with self.assertRaisesRegex(runner.Failure, r"stretch 1\.3\d*, above 1\.05\).*plays the clip too fast"):
            self.stall_step(movie(15.0, audio=sound(11.5, 1.3)), makes_up=False)
        # A jump with a small stretch is its own failure.
        with self.assertRaisesRegex(runner.Failure, r"jumps 0\.92 s between two samples"):
            self.stall_step(movie(24.9, audio=sound(24.0, jump=(300, 0.9))))

    def test_the_stall_step_fails_when_the_hal_does_not_say_it_made_the_stall_up(self) -> None:
        with self.assertRaisesRegex(runner.Failure, r"made 0 frames of silence up in 0 stall\(s\) during 3 stalls of 1\.5 s "
                                                    r"\(at least 108000 frames wanted\)") as raised:
            self.stall_step(movie(12.0, audio=sound(12.0)), makes_up=False)
        self.assertIn("the HAL made 0 frames of silence up in 0 stall(s)", raised.exception.out)
        # Only this recording's lines count: what the log held before it is not the stalls'.
        before = ("11-14 21:00:00.000   200   300 W AHAL_StreamAlsaMonoPipe: readInput: the device delivers again, "
                  "after 90000 frames of silence made up\n")
        with self.assertRaisesRegex(runner.Failure, "made 0 frames of silence up"):
            self.stall_step(movie(12.0, audio=sound(12.0)), makes_up=False, stream_text=before)

    def test_a_stall_that_stops_the_video_too_cannot_be_judged(self) -> None:
        with self.assertRaisesRegex(runner.Failure, r"the video has a gap of 1\.53 s \(above 0\.5 s\): the guest stood still"):
            self.stall_step(movie(12.0, audio=sound(12.0), video_jump=(100, 1.5)))

    def test_the_stall_step_does_not_run_without_the_library(self) -> None:
        guest, out = self.stall_step(movie(12.0, audio=sound(12.0)), hold_directory=None)
        self.assertEqual((guest.audio_held, guest.commands), ([], []))
        self.assertIn("audio stall: not run (no --audio-hold-library)", out)

    def freeze_step(self, data):
        plan = runner.make_plan(self.PHONE, "phone", "live")
        guest = FakeGuest(plan, movies=data)
        out = io.StringIO()
        try:
            with mock.patch.object(runner.time, "sleep", lambda seconds: None), contextlib.redirect_stdout(out):
                runner.check_video_freeze(guest, plan)
        except runner.Failure as failure:
            failure.guest, failure.out = guest, out.getvalue()
            raise
        return guest, out.getvalue()

    def test_the_freeze_step_stops_the_emulator_three_times_and_wants_equal_gaps(self) -> None:
        # The video has a gap of 1.5 s and so has the audio (a step of its sample times), and both end together.
        guest, out = self.freeze_step(movie(12.0, fps=15.0, audio=sound(12.0, jump=(100, 1.5)), video_jump=(40, 1.5)))
        self.assertEqual(guest.frozen, [runner.FREEZE_SECONDS] * runner.DISTURB_COUNT)
        self.assertEqual(guest.audio_held, [])
        self.assertEqual(guest.commands.count(f"input keyevent {runner.CAMERA_KEYCODE}"), 2)
        self.assertRegex(out, r"freeze 3: the emulator stopped 1\.5 s from \d+\.\d s after the start key")
        self.assertRegex(out, r"longest gap in the video 1\.5\d+ s")
        self.assertRegex(out, r"longest step of its sample times 1\.52\d* s, ends -0\.011 s after the video")

    def test_the_freeze_step_fails_for_frames_that_come_in_a_burst_after_the_thaw(self) -> None:
        # The burst leaves the gaps equal and runs the audio on past the video by as long as the freeze was.
        with self.assertRaisesRegex(runner.Failure, r"the audio ends \+1\.39 s after the video \(more than 0\.25 s either way\)"):
            self.freeze_step(movie(10.6, fps=15.0, audio=sound(12.0, jump=(100, 1.5)), video_jump=(40, 1.5)))

    def test_the_freeze_step_fails_for_a_freeze_the_video_does_not_show_or_gaps_that_differ(self) -> None:
        with self.assertRaisesRegex(runner.Failure, r"the freeze left a gap of only 0\.03 s in the video"):
            self.freeze_step(movie(12.0, audio=sound(12.0)))
        with self.assertRaisesRegex(runner.Failure, r"audio's longest step is 0\.02 s and the video's 1\.57 s .*same gap in both tracks"):
            self.freeze_step(movie(12.0, fps=15.0, audio=sound(12.0), video_jump=(40, 1.5)))
        with self.assertRaisesRegex(runner.Failure, r"audio's longest step is 1\.47 s and the video's 1\.07 s"):
            self.freeze_step(movie(12.0, fps=15.0, audio=sound(12.0, jump=(100, 1.45)), video_jump=(40, 1.0)))
        with self.assertRaisesRegex(runner.Failure, "has no audio track"):
            self.freeze_step(movie(12.0, video_jump=(40, 1.5)))

    def test_the_whole_run_names_the_reasons_of_the_old_input(self) -> None:
        # What the r26 input leaves in a recording with three stalls of 1.5 s: stamps with no audio behind them. And
        # the input of the first proof image in the freeze step: the gaps equal, the burst after them.
        with self.assertRaisesRegex(runner.Failure, r"stretch 1\.\d+, above 1\.05\)"):
            self.stall_step(movie(12.0, audio=sound(8.2, 1.46)))
        with self.assertRaisesRegex(runner.Failure, r"the audio ends \+[0-9.]+ s after the video"):
            self.freeze_step(movie(7.0, fps=15.0, audio=sound(8.4, jump=(100, 1.5)), video_jump=(40, 1.5)))

    def test_the_guest_stops_its_emulator_process_and_continues_it_whatever_happens(self) -> None:
        for fail in (False, True):
            events: list[tuple] = []
            guest = runner.Guest("adb", "127.0.0.1:1", mock.Mock(pid=4242), Path("emulator.log"), Path("private"))

            def sleep(seconds: float) -> None:
                events.append(("sleep", seconds))
                if fail:
                    raise KeyboardInterrupt

            with self.subTest(fail=fail), mock.patch.object(runner.os, "kill", lambda pid, signal: events.append(
                    ("kill", pid, signal))), mock.patch.object(runner.time, "sleep", sleep):
                if fail:
                    with self.assertRaises(KeyboardInterrupt):
                        guest.freeze(1.5)
                else:
                    guest.freeze(1.5)
            self.assertEqual(events, [("kill", 4242, runner.signal.SIGSTOP), ("sleep", 1.5),
                                      ("kill", 4242, runner.signal.SIGCONT)])

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
        # The switches follow the other live steps, which see the state they always saw.
        self.assertRegex(run_checks, r'if plan.mode == "live":\n\s+check_video\(guest, plan\)\n'
                         r'\s+check_audio_stall\(guest, plan\)\n\s+check_video_freeze\(guest, plan\)\n'
                         r'\s+check_front\(guest, plan\)\n\s+check_mode_switch\(guest, plan\)\n'
                         r'\s+check_hal_log\(guest\)')


class ModeSwitchTests(unittest.TestCase):
    LAYOUT = "bf-large-flash-manual-concurrent-front2160"

    def plan(self, rounds: int = 4, profile: str = "phone"):
        return runner.make_plan(self.LAYOUT if profile == "phone" else "front", profile, "live", rounds)

    def run_step(self, guest, plan=None) -> str:
        """The step with its sleeps skipped and its deadlines short, so that a wait that fails ends at once."""
        self.printed = io.StringIO()
        with mock.patch.object(runner.time, "sleep", lambda seconds: None), \
                mock.patch.object(runner, "SWITCH_OBSERVE_SECONDS", 0.05), contextlib.redirect_stdout(self.printed):
            runner.check_mode_switch(guest, plan or guest.plan)
        return self.printed.getvalue()

    def guest(self, rounds: int = 4, profile: str = "phone"):
        return FakeGuest(self.plan(rounds, profile))

    # -- What counts as a refusal.

    def test_the_denial_and_the_refusal_are_exactly_those_lines(self) -> None:
        self.assertEqual(runner.switch_failures(SWITCH_DENIAL), [(runner.SWITCH_DENIAL_KIND, SWITCH_DENIAL)])
        self.assertEqual(runner.switch_failures(SWITCH_REFUSAL), [(runner.SWITCH_REFUSAL_KIND, SWITCH_REFUSAL)])
        both = runner.switch_failures(f"{SWITCH_QUIET}{SWITCH_DENIAL}\n{SWITCH_REFUSAL}\n")
        self.assertEqual([what for what, _ in both], [runner.SWITCH_DENIAL_KIND, runner.SWITCH_REFUSAL_KIND])
        self.assertEqual(runner.SWITCH_DENIAL_KIND, "an SELinux denial naming hal_camera_default")
        # The kernel's own copy of the record (the process name as the tag, no comm field) and a security
        # context with categories are the same denial.
        copy = SWITCH_DENIAL.replace("I auditd  :", "W C3Dev-0-ReqQueu:").replace('comm="C3Dev-0-ReqQueu" ', "")
        self.assertEqual(len(runner.switch_failures(copy)), 1)
        categories = SWITCH_DENIAL.replace("hal_camera_default:s0", "hal_camera_default:s0:c512,c768")
        self.assertEqual(len(runner.switch_failures(categories)), 1)

    def test_a_correct_image_and_unrelated_events_are_not_refusals(self) -> None:
        self.assertEqual(runner.switch_failures(SWITCH_QUIET), [])
        self.assertEqual(runner.switch_failures(""), [])
        for line in SWITCH_UNRELATED_DENIALS:
            self.assertEqual(runner.switch_failures(line), [], line)
        # One field of the denial at a time: any other source, target, class or permission, or a grant.
        for old, new in (("scontext=u:r:hal_camera_default:s0", "scontext=u:r:cameraserver:s0"),
                         ("scontext=u:r:hal_camera_default:s0", "scontext=u:r:hal_camera_default_x:s0"),
                         ("tcontext=u:r:hal_graphics_composer_default:s0", "tcontext=u:r:surfaceflinger:s0"),
                         ("tcontext=u:r:hal_graphics_composer_default:s0",
                          "tcontext=u:r:hal_graphics_composer_default_x:s0"),
                         ("tclass=fd", "tclass=file"), ("{ use }", "{ read }"), ("{ use }", "{ ioctl }"),
                         ("denied", "granted")):
            self.assertEqual(runner.switch_failures(SWITCH_DENIAL.replace(old, new)), [], new)
        # A permission set that holds `use` among others is still the denial.
        self.assertEqual(len(runner.switch_failures(SWITCH_DENIAL.replace("{ use }", "{ read use }"))), 1)

    def test_the_stream_errors_of_a_stopped_application_do_not_fail_a_correct_image(self) -> None:
        plan = self.plan(2)
        guest = FakeGuest(plan)
        for number in (1, 2):
            guest.switch_logs[number] = SWITCH_QUIET + "\n".join(SWITCH_UNRELATED_DENIALS) + "\n"
        output = self.run_step(guest)
        self.assertIn("mode switch 2 of 2", output)

    # -- The flow.

    def test_every_round_starts_the_photo_mode_then_the_video_mode_and_reads_the_log_after_its_marker(self) -> None:
        guest = self.guest()
        sleeps: list[float] = []
        output = io.StringIO()
        with mock.patch.object(runner.time, "sleep", sleeps.append), contextlib.redirect_stdout(output):
            runner.check_mode_switch(guest, guest.plan)
        self.assertEqual(sleeps.count(runner.SWITCH_HOLD_SECONDS), runner.SWITCH_ROUNDS)
        self.assertEqual(sleeps.count(runner.SWITCH_SETTLE_SECONDS), runner.SWITCH_ROUNDS)
        self.assertEqual(sleeps.count(runner.SWITCH_DRAIN_SECONDS), 1)
        stop = f"am force-stop {runner.CAMERA_PACKAGE}"
        photo = f"am start -a android.media.action.STILL_IMAGE_CAMERA --ez {runner.FRONT_EXTRA} false"
        video = f"am start -a {runner.VIDEO_ACTION} --ez {runner.FRONT_EXTRA} false"
        activity = [command for command in guest.commands if command.startswith("am ")]
        self.assertEqual(activity, [stop, photo, video] * runner.SWITCH_ROUNDS)
        self.assertEqual(len([c for c in guest.commands if c.startswith(f"log -t {runner.SWITCH_MARKER_TAG} ")]),
                         runner.SWITCH_ROUNDS)
        # One read per round (the switch is seen at once) and the read after the last round.
        self.assertEqual(guest.log_reads, runner.SWITCH_ROUNDS + 1)
        self.assertIn(f"mode switch {runner.SWITCH_ROUNDS} of {runner.SWITCH_ROUNDS}: photo activity paused, video "
                      "activity resumed, capture before and after; no refused capture request", output.getvalue())

    def test_the_rounds_are_the_plans(self) -> None:
        for rounds in (1, 2, 12):
            guest = self.guest(rounds)
            output = self.run_step(guest)
            markers = [c for c in guest.commands if c.startswith(f"log -t {runner.SWITCH_MARKER_TAG} ")]
            self.assertEqual(len(markers), rounds)
            self.assertIn(f"photo to video switch {rounds} of {rounds}'", markers[-1])
            self.assertIn(f"mode switch {rounds} of {rounds}:", output)

    def test_the_animations_are_off_for_the_switches_and_restored_after_them(self) -> None:
        for before in ({}, {"window_animation_scale": "0.5", "transition_animation_scale": "1.5",
                            "animator_duration_scale": "2.0"}):
            guest = self.guest()
            guest.settings = dict(before)
            self.run_step(guest)
            self.assertEqual(len(guest.settings_at_video_start), runner.SWITCH_ROUNDS)
            for seen in guest.settings_at_video_start:
                self.assertEqual(seen, {key: "0" for key in runner.ANIMATION_SCALES})
            self.assertEqual(guest.settings, before)

    def test_the_laptop_switches_with_its_front_camera(self) -> None:
        guest = self.guest(2, "laptop")
        self.run_step(guest)
        starts = [command for command in guest.commands if command.startswith("am start")]
        self.assertTrue(all(f"{runner.FRONT_EXTRA} true" in command for command in starts), starts)

    # -- A round passes only when the switch was seen (live capture before, the pause, the resume, capture after).

    def test_a_round_without_the_switch_fails_by_name(self) -> None:
        for events, missing in (((False, False), "no pause of org.lineageos.aperture.CameraActivity and no resume of "
                                                 "org.lineageos.aperture.VideoCamera"),
                                ((True, False), "no resume of org.lineageos.aperture.VideoCamera"),
                                ((False, True), "no pause of org.lineageos.aperture.CameraActivity")):
            guest = self.guest()
            guest.events[1] = events
            with self.assertRaisesRegex(runner.Failure, r"^no mode switch observed in the photo to video switch 1 of 4: "
                                        r"the log after its marker shows " + re.escape(missing)):
                self.run_step(guest)

    def test_the_events_of_an_earlier_round_do_not_make_a_later_one(self) -> None:
        guest = self.guest()
        guest.events[3] = (False, False)
        with self.assertRaisesRegex(runner.Failure, "no mode switch observed in the photo to video switch 3 of 4"):
            self.run_step(guest)
        self.assertEqual(guest.video_starts, 3)

    def test_capture_must_be_running_before_the_switch_and_after_it(self) -> None:
        guest = self.guest()
        guest.stalled = {"photo"}
        with self.assertRaisesRegex(runner.Failure, r"^live capture \(photo, before switch 1\): the frame counters stand at"):
            self.run_step(guest)
        self.assertEqual(guest.video_starts, 0)
        guest = self.guest()
        guest.stalled = {"video"}
        with self.assertRaisesRegex(runner.Failure, r"^live capture \(video, after the photo to video switch 1 of 4\)"):
            self.run_step(guest)

    def test_the_application_must_hold_the_camera_and_show_the_chart_before_and_after(self) -> None:
        with mock.patch.object(runner, "PREVIEW_SECONDS", 0.05):
            guest = self.guest()
            guest.no_camera_in = {"photo"}
            with self.assertRaisesRegex(runner.Failure, r"^the camera application holding camera 0 \(photo\)"):
                self.run_step(guest)
            self.assertEqual(guest.video_starts, 0)
            guest = self.guest()
            guest.no_camera_in = {"video"}
            with self.assertRaisesRegex(runner.Failure, r"^the camera application holding camera 0 \(video\)"):
                self.run_step(guest)
            self.assertEqual(guest.video_starts, 1)

    def test_the_frame_counters_of_the_dump_are_summed(self) -> None:
        dump = "Stream[3]: Output\n  Frames produced: 815, last timestamp: 20 ns\nStream[4]: Output\n  Frames produced: 0, x\n"
        self.assertEqual(runner.frames_produced(dump), 815)
        self.assertEqual(runner.frames_produced("nothing"), 0)

    # -- The refusal is read wherever it lands: between two reads and after the last.

    def test_the_exact_denial_and_refusal_fail_the_round_and_name_both(self) -> None:
        for lines in (f"{SWITCH_REFUSAL}\n{SWITCH_DENIAL}\n", f"{SWITCH_DENIAL}\n{SWITCH_REFUSAL}\n"):
            guest = self.guest()
            guest.switch_logs[3] = lines
            with self.assertRaisesRegex(
                    runner.Failure, r'^an SELinux denial naming hal_camera_default and the camera service\'s "Unable to '
                    r'submit capture request" since the photo to video switch 2 of 4 \(read after the photo to video '
                    r"switch 3 of 4\): .*avc:  denied  \{ use \}.* \| .*Unable to submit"):
                self.run_step(guest)

    def test_a_denial_alone_and_a_refusal_alone_fail_the_step(self) -> None:
        for line, kind in ((SWITCH_DENIAL, "an SELinux denial naming hal_camera_default"),
                           (SWITCH_REFUSAL, 'the camera service\'s "Unable to submit capture request"')):
            guest = self.guest(2, "laptop")
            guest.switch_logs[1] = line + "\n"
            with self.assertRaisesRegex(runner.Failure, "^" + re.escape(kind) + " since the photo to video switch 1 of 2"):
                self.run_step(guest)

    def test_a_refusal_that_lands_between_two_reads_is_read_by_the_next(self) -> None:
        guest = self.guest()
        guest.late_logs[1] = SWITCH_DENIAL + "\n" + SWITCH_REFUSAL + "\n"
        with self.assertRaisesRegex(runner.Failure, r"since the photo to video switch 1 of 4 \(read after the photo to "
                                    r"video switch 2 of 4\)"):
            self.run_step(guest)
        # The same log without the late lines passes.
        self.run_step(self.guest())

    def test_a_refusal_that_lands_after_the_last_round_is_read_at_the_end(self) -> None:
        guest = self.guest(2)
        guest.late_logs[2] = SWITCH_REFUSAL + "\n"
        with self.assertRaisesRegex(runner.Failure, r"in the final read after the last switch \(photo to video switch 2 "
                                    r"of 2\): .*Unable to submit"):
            self.run_step(guest)
        self.assertEqual(guest.log_reads, 3)
        self.run_step(self.guest(2))

    def test_a_log_that_lost_the_previous_marker_cannot_be_judged(self) -> None:
        guest = self.guest()
        guest.wrap_after_read = {1}
        with self.assertRaisesRegex(runner.Failure, "the log has no line for the photo to video switch 1 of 4: it was "
                                    "lost or never written, so the photo to video switches cannot be judged"):
            self.run_step(guest)
        guest = self.guest()
        guest.no_markers = True
        with self.assertRaisesRegex(runner.Failure, "the log has no line for the photo to video switch 1 of 4"):
            self.run_step(guest)

    def test_what_the_log_held_before_the_first_marker_is_not_judged(self) -> None:
        guest = self.guest()
        guest.full_log += SWITCH_DENIAL + "\n" + SWITCH_REFUSAL + "\n"
        self.run_step(guest)

    def test_a_long_run_reads_only_from_the_previous_marker(self) -> None:
        # Twelve rounds outlast a log buffer that keeps three rounds: lines older than the previous marker are
        # gone from the log, and the step still passes because it never needs them.
        guest = self.guest(12)
        original = guest.shell

        def trimming(command, timeout=0):
            text = original(command, timeout)
            if command == "logcat -d -b all":
                markers = [m.end() for m in re.finditer(r"CamSwitch: photo to video switch \d+ of 12\n", text)]
                return text[markers[-2] - 60:] if len(markers) > 2 else text
            return text
        guest.shell = trimming
        self.run_step(guest)

    # -- The animation scales come back on every exit.

    def test_the_animations_are_restored_when_a_switch_fails_too(self) -> None:
        guest = self.guest()
        guest.settings = {"window_animation_scale": "0.5"}
        guest.switch_logs[2] = SWITCH_REFUSAL + "\n"
        with self.assertRaises(runner.Failure):
            self.run_step(guest)
        self.assertEqual(len(guest.settings_at_video_start), 2)
        self.assertEqual(guest.settings, {"window_animation_scale": "0.5"})

    def test_a_failed_second_write_leaves_no_scale_at_zero(self) -> None:
        guest = self.guest()
        guest.settings = {key: "1" for key in runner.ANIMATION_SCALES}
        guest.failing = {"settings put global transition_animation_scale 0"}
        with self.assertRaisesRegex(runner.Failure, "injected"):
            self.run_step(guest)
        self.assertEqual(guest.settings, {key: "1" for key in runner.ANIMATION_SCALES})
        self.assertEqual(guest.video_starts, 0)

    def test_a_failed_read_leaves_the_scales_alone(self) -> None:
        guest = self.guest()
        guest.settings = {key: "1" for key in runner.ANIMATION_SCALES}
        guest.failing = {"settings get global transition_animation_scale"}
        with self.assertRaisesRegex(runner.Failure, "injected"):
            self.run_step(guest)
        self.assertEqual(guest.settings, {key: "1" for key in runner.ANIMATION_SCALES})

    def test_a_restore_that_fails_fails_the_step_and_the_others_are_still_restored(self) -> None:
        guest = self.guest()
        guest.settings = {key: "1" for key in runner.ANIMATION_SCALES}
        guest.failing = {"settings put global window_animation_scale 1"}
        with self.assertRaisesRegex(runner.Failure, "the animation scale window_animation_scale was not restored"):
            self.run_step(guest)
        self.assertEqual(guest.settings, {"window_animation_scale": "0", "transition_animation_scale": "1",
                                          "animator_duration_scale": "1"})

    def test_a_restore_that_fails_does_not_hide_the_failure_of_the_switch(self) -> None:
        guest = self.guest()
        guest.settings = {key: "1" for key in runner.ANIMATION_SCALES}
        guest.failing = {"settings put global window_animation_scale 1"}
        guest.switch_logs[1] = SWITCH_DENIAL + "\n"
        with self.assertRaisesRegex(runner.Failure, "an SELinux denial naming"):
            self.run_step(guest)
        self.assertIn("the animation scale window_animation_scale was not restored", self.printed.getvalue())
        self.assertEqual(guest.settings["transition_animation_scale"], "1")
        self.assertEqual(guest.settings["animator_duration_scale"], "1")


# `dumpsys sensor_privacy` of a guest started with Camera off and the side keys: the camera (sensor 2) is
# blocked by the hardware toggle (type 2); a software toggle is type 1.
SENSOR_PRIVACY_BLOCKED = """SENSOR PRIVACY MANAGER STATE (dumpsys sensor_privacy)
{
  storage_implementation=com.android.server.sensorprivacy.SensorPrivacyStateControllerImpl
  users={
    user_id=0
    sensors={
      sensor=2
      toggles={
        toggle_type=2
        state_type=1
        last_change=20299
      }
    }
  }
}
"""
SENSOR_PRIVACY_LIVE = """SENSOR PRIVACY MANAGER STATE (dumpsys sensor_privacy)
{
  storage_implementation=com.android.server.sensorprivacy.SensorPrivacyStateControllerImpl
}
"""

# The interface dump of the platform's dialog over the camera application, trimmed to the nodes that matter, and
# the dump of the camera application alone.
SENSOR_DIALOG_DUMP = (
    "<?xml version='1.0' encoding='UTF-8' standalone='yes' ?><hierarchy rotation=\"0\">"
    '<node index="0" text="" resource-id="android:id/content" class="android.widget.FrameLayout" '
    'package="com.android.systemui" bounds="[0,0][720,1280]">'
    '<node index="1" text="Unblock device camera?" '
    'resource-id="com.android.systemui:id/sensor_use_started_title_message" '
    'class="android.widget.TextView" package="com.android.systemui" bounds="[0,0][1,1]" />'
    '<node index="0" text="This unblocks access for all apps and services allowed to use your camera." '
    'resource-id="android:id/message" class="android.widget.TextView" package="com.android.systemui" />'
    '<node index="1" text="Unblock" resource-id="android:id/button1" class="android.widget.Button" '
    'package="com.android.systemui" /></node></hierarchy>')
CAMERA_APPLICATION_DUMP = (
    "<?xml version='1.0' encoding='UTF-8' standalone='yes' ?><hierarchy rotation=\"0\">"
    '<node index="0" text="" resource-id="" class="android.widget.FrameLayout" package="org.lineageos.aperture">'
    '<node index="0" text="PHOTO" resource-id="org.lineageos.aperture:id/photoModeButton" '
    'class="android.widget.Button" package="org.lineageos.aperture" />'
    '<node index="1" text="Unblock" resource-id="" class="android.widget.Button" '
    'package="org.example.other" /></node></hierarchy>')
DIALOG_EVENT = ("11-15 03:41:29.579   602   622 I wm_create_activity: [0,132003343,10,"
                "com.android.systemui/.sensorprivacy.SensorUseStartedActivity,NULL,NULL,NULL,277086208]")
STAND_IN_EVENT = ("11-15 03:53:57.190   602   617 I wm_create_activity: [0,226133289,11,"
                  "android.kernarium.sensoruse/.QuietActivity,NULL,NULL,NULL,277086208]")
CAMERA_EVENT = ("11-15 03:41:20.100   602   617 I wm_create_activity: [0,5,10,"
                "org.lineageos.aperture/.CameraActivity,NULL,NULL,NULL,0]")


class SensorDialogTests(unittest.TestCase):
    """The camera switch as Android holds it, and the platform's Unblock dialog that must not appear."""

    def test_the_toggles_of_the_sensor_privacy_service_are_read_from_its_dump(self) -> None:
        self.assertEqual(runner.parse_sensor_privacy(SENSOR_PRIVACY_BLOCKED), [(0, 2, 2, 1)])
        self.assertEqual(runner.parse_sensor_privacy(SENSOR_PRIVACY_LIVE), [])
        self.assertEqual(runner.parse_sensor_privacy(""), [])
        both = SENSOR_PRIVACY_BLOCKED.replace("toggle_type=2", "toggle_type=1\n        state_type=1\n"
                                              "        toggle_type=2", 1)
        self.assertEqual(runner.parse_sensor_privacy(both), [(0, 2, 1, 1), (0, 2, 2, 1)])

    def test_only_the_hardware_toggle_of_the_camera_is_the_lens_cover(self) -> None:
        self.assertTrue(runner.camera_hardware_blocked([(0, 2, 2, 1)]))
        self.assertFalse(runner.camera_hardware_blocked([]))
        self.assertFalse(runner.camera_hardware_blocked([(0, 2, 1, 1)]), "the software toggle")
        self.assertFalse(runner.camera_hardware_blocked([(0, 1, 2, 1)]), "the microphone")
        self.assertFalse(runner.camera_hardware_blocked([(0, 2, 2, 2)]), "a toggle that is not enabled")

    def test_the_dialog_is_found_by_its_title_and_button_in_systemui_alone(self) -> None:
        found = runner.sensor_dialog_nodes(SENSOR_DIALOG_DUMP)
        self.assertEqual(len(found), 2)
        self.assertIn("com.android.systemui:id/sensor_use_started_title_message 'Unblock device camera?'", found)
        self.assertIn("android:id/button1 'Unblock'", found)
        self.assertEqual(runner.sensor_dialog_nodes(CAMERA_APPLICATION_DUMP), [])
        with self.assertRaisesRegex(ValueError, "not XML"):
            runner.sensor_dialog_nodes("<hierarchy><node")

    def test_the_activities_the_platform_created_are_counted_by_name(self) -> None:
        self.assertEqual(runner.sensor_activity_starts(""), (0, 0))
        self.assertEqual(runner.sensor_activity_starts("\n".join((CAMERA_EVENT, DIALOG_EVENT))), (1, 0))
        self.assertEqual(runner.sensor_activity_starts("\n".join((CAMERA_EVENT, STAND_IN_EVENT, STAND_IN_EVENT))),
                         (0, 2))
        self.assertEqual(runner.sensor_activity_starts(
            "I wm_restart_activity: [0,1,10,com.android.systemui/.sensorprivacy.SensorUseStartedActivity]"), (0, 0))

    def test_a_blocked_session_always_has_the_side_keys(self) -> None:
        parser = runner.build_parser()
        self.assertTrue(runner.wants_hardware_input(parser.parse_args(["--mode", "blocked"])))
        self.assertFalse(runner.wants_hardware_input(parser.parse_args(["--mode", "live"])))
        self.assertTrue(runner.wants_hardware_input(parser.parse_args(["--mode", "live", "--hardware-input"])))
        self.assertFalse(runner.wants_hardware_input(parser.parse_args(["--mode", "none"])))
        options = parser.parse_args([])
        self.assertEqual(options.vnc_port, runner.slot_ports.slot_port(runner.VNC_OFFSET, runner.DEFAULT_VNC_PORT))

    def test_the_launcher_has_the_option_the_runner_adds(self) -> None:
        source = (REPOSITORY / "darwin/app/macos/main.cpp").read_text(encoding="utf-8")
        self.assertIn(f'"{runner.HARDWARE_INPUT_OPTION}"', source)

    class Guest:
        def __init__(self, privacy: str, dump: str, events: str, hardware_input: bool = True) -> None:
            self.privacy, self.dump, self.events, self.hardware_input = privacy, dump, events, hardware_input
            self.asked: list[str] = []

        def sensor_privacy(self) -> str:
            self.asked.append("privacy")
            return self.privacy

        def interface_dump(self) -> str:
            self.asked.append("dump")
            return self.dump

        def activity_events(self) -> str:
            self.asked.append("events")
            return self.events

    def run_step(self, mode: str, guest) -> str:
        plan = runner.make_plan("bf-small-flash", "phone", mode)
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            runner.check_sensor_dialog(guest, plan)
        return printed.getvalue()

    def test_a_blocked_session_holds_the_toggle_and_shows_the_stand_in_instead_of_the_dialog(self) -> None:
        guest = self.Guest(SENSOR_PRIVACY_BLOCKED, CAMERA_APPLICATION_DUMP, "\n".join((CAMERA_EVENT, STAND_IN_EVENT)))
        self.assertIn("hardware toggle set; no dialog on the screen", self.run_step("blocked", guest))
        self.assertEqual(guest.asked, ["privacy", "dump", "events"])

    def test_a_live_session_holds_no_toggle_and_starts_no_stand_in(self) -> None:
        guest = self.Guest(SENSOR_PRIVACY_LIVE, CAMERA_APPLICATION_DUMP, CAMERA_EVENT)
        self.assertIn("hardware toggle not set", self.run_step("live", guest))

    def test_the_dialog_of_the_image_before_the_stand_in_fails_a_blocked_session(self) -> None:
        # What the accepted release shows: the toggle is set, the dialog is on the screen and was started.
        guest = self.Guest(SENSOR_PRIVACY_BLOCKED, SENSOR_DIALOG_DUMP, "\n".join((CAMERA_EVENT, DIALOG_EVENT)))
        with self.assertRaisesRegex(runner.Failure, "sensor privacy dialog is on the screen of the blocked"):
            self.run_step("blocked", guest)

    def test_a_dialog_that_was_started_and_left_still_fails(self) -> None:
        guest = self.Guest(SENSOR_PRIVACY_BLOCKED, CAMERA_APPLICATION_DUMP,
                           "\n".join((DIALOG_EVENT, STAND_IN_EVENT)))
        with self.assertRaisesRegex(runner.Failure, "SensorUseStartedActivity was started 1 time"):
            self.run_step("blocked", guest)

    def test_no_dialog_and_no_stand_in_is_not_a_pass_for_a_blocked_session(self) -> None:
        guest = self.Guest(SENSOR_PRIVACY_BLOCKED, CAMERA_APPLICATION_DUMP, CAMERA_EVENT)
        with self.assertRaisesRegex(runner.Failure, "did not start android.kernarium.sensoruse"):
            self.run_step("blocked", guest)

    def test_the_toggle_must_follow_the_mode(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "toggle is not set in a blocked session"):
            self.run_step("blocked", self.Guest(SENSOR_PRIVACY_LIVE, CAMERA_APPLICATION_DUMP, STAND_IN_EVENT))
        with self.assertRaisesRegex(runner.Failure, "toggle is set in a live session"):
            self.run_step("live", self.Guest(SENSOR_PRIVACY_BLOCKED, CAMERA_APPLICATION_DUMP, CAMERA_EVENT))

    def test_a_live_session_that_shows_the_dialog_or_the_stand_in_fails(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "dialog is on the screen of the live"):
            self.run_step("live", self.Guest(SENSOR_PRIVACY_LIVE, SENSOR_DIALOG_DUMP, CAMERA_EVENT))
        with self.assertRaisesRegex(runner.Failure, "stand-in activity .* was started in a live session"):
            self.run_step("live", self.Guest(SENSOR_PRIVACY_LIVE, CAMERA_APPLICATION_DUMP, STAND_IN_EVENT))

    def test_a_guest_without_the_side_keys_says_so_and_asks_nothing(self) -> None:
        guest = self.Guest(SENSOR_PRIVACY_LIVE, "", "", hardware_input=False)
        self.assertIn("not run, the guest has no side keys", self.run_step("blocked", guest))
        self.assertEqual(guest.asked, [])


if __name__ == "__main__":
    unittest.main()
