#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The orientation runner's wire, its reading of the guest, and its probe application.

run_android_orientation.py plays the host application against a booted release: it reads the guest's control
frames, sends orientation records, and reads Android's rotation from `dumpsys window displays`, the sensor service
from `dumpsys sensorservice` and the navigation bar from `uiautomator dump`. The texts below have the layout those
commands print (WindowOrientationListener, SensorService, SystemUI), so the runner's parsers, its host and the
rotation each request earns run without a guest. The frames are the golden frames of tests/unit/andarium/fixtures.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
import math
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

TESTS = Path(__file__).resolve().parents[2]
SCRIPT = TESTS / "integration/android/run_android_orientation.py"
SPEC = importlib.util.spec_from_file_location("run_android_orientation", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
# Dataclasses resolve their annotations through sys.modules.
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)
probe = runner.orientation_probe_apk
FIXTURE = TESTS / "unit/andarium/fixtures/motion-orientation-golden-frames.txt"


def golden() -> dict[str, bytes]:
    frames = {}
    for line in FIXTURE.read_text().splitlines():
        if line.startswith("#") or ":" not in line:
            continue
        name, _, hex_bytes = line.partition(":")
        frames[name.strip()] = bytes.fromhex(hex_bytes.strip())
    return frames


# A fresh r22 guest in the setup wizard, then the same display after the window manager chose the orientation sensor.
ROTATION_DUMP = """\
  DisplayRotation
    mCurrentAppOrientation=SCREEN_ORIENTATION_UNSPECIFIED
    mLastOrientation=-1
    mRotation=0 mDeferredRotationPauseCount=0
    mLandscapeRotation=ROTATION_90 mSeascapeRotation=ROTATION_270
    mPortraitRotation=ROTATION_0 mUpsideDownRotation=ROTATION_180
    mSupportAutoRotation=true
    WindowOrientationListener
      mEnabled=true
      mCurrentRotation=ROTATION_0
      mSensorType=null
      mSensor={Sensor name="Host Accelerometer", vendor="Andarium", version=1, type=1, maxRange=156.906}
      mRate=2
      AccelSensorJudge
        mProposedRotation=-1
        mPredictedRotation=-1
        mLastFilteredX=0.0

    mCarDockRotation=-1 mDeskDockRotation=-1
    mUserRotationMode=USER_ROTATION_FREE mUserRotation=ROTATION_0 mCameraRotationMode=0 mAllowAllRotations=false
"""

ORIENTATION_DUMP = (
    ROTATION_DUMP
    .replace("mRotation=0 ", "mRotation=3 ")
    .replace("SCREEN_ORIENTATION_UNSPECIFIED", "SCREEN_ORIENTATION_SENSOR_LANDSCAPE")
    .replace('name="Host Accelerometer", vendor="Andarium", version=1, type=1',
             'name="Host Device Orientation", vendor="Andarium", version=1, type=27')
    .replace("AccelSensorJudge", "OrientationSensorJudge")
    .replace("mUserRotationMode=USER_ROTATION_FREE mUserRotation=ROTATION_0",
             "mUserRotationMode=USER_ROTATION_LOCKED mUserRotation=ROTATION_90")
)

SENSOR_DUMP = """\
Sensor List:
0x00000001) Host Accelerometer        | Andarium        | ver: 1 | type: android.sensor.accelerometer(1) | perm: n/a | flags: 0x00000000
\tcontinuous | minRate=1.00Hz | maxRate=100.00Hz | no batching | non-wakeUp |
0x00000002) Host Gyroscope            | Andarium        | ver: 1 | type: android.sensor.gyroscope(4) | perm: n/a | flags: 0x00000000
\tcontinuous | minRate=1.00Hz | maxRate=100.00Hz | no batching | non-wakeUp |
0x00000003) Host Device Orientation   | Andarium        | ver: 1 | type: android.sensor.device_orientation(27) | perm: n/a | flags: 0x00000002
\ton-change | maxDelay=0us | no batching | non-wakeUp |
0x5f676172) Game Rotation Vector Sensor | AOSP            | ver: 3 | type: android.sensor.game_rotation_vector(15) | perm: n/a | flags: 0x00000000
\tcontinuous | maxDelay=0us | maxRate=100.00Hz | no batching | non-wakeUp |
Fusion States:
9-axis fusion disabled (0 clients), gyro-rate= 200.00Hz, q=< 0, 0, 0, 0 > (0), b=< 0, 0, 0 >
Recent Sensor events:
Active sensors:
Host Device Orientation (handle=0x00000003, connections=1)
Socket Buffer size = 39 events

--- Client Listeners (2 registered) ---
"""

HIERARCHY = """\
<?xml version='1.0' encoding='UTF-8' standalone='yes' ?><hierarchy rotation="1">
<node index="0" text="" resource-id="" class="android.widget.FrameLayout" package="com.android.settings" content-desc="" bounds="[0,0][1280,720]">
<node index="0" text="" resource-id="com.android.systemui:id/rotate_suggestion" class="android.widget.ImageView" package="com.android.systemui" content-desc="Rotate screen" bounds="[1100,600][1180,680]"/>
<node index="1" text="Back" resource-id="" class="android.widget.ImageView" package="com.android.systemui" content-desc="Back" bounds="[10,600][90,680]"/>
</node></hierarchy>
"""


class WireTests(unittest.TestCase):
    def test_the_records_are_the_golden_frames(self) -> None:
        frames = golden()
        for value in range(4):
            self.assertEqual(runner.orientation_record(value), frames[f"orientation-{value}"])
        self.assertEqual(len(runner.orientation_record(0)), runner.RECORD_SIZE)

    def test_the_records_the_guest_refuses_are_the_golden_frames(self) -> None:
        frames = golden()
        self.assertEqual(runner.orientation_record(4), frames["reject-orientation-high"])
        self.assertEqual(runner.orientation_record(-1), frames["reject-orientation-negative"])
        self.assertEqual(runner.orientation_record(1.5), frames["reject-orientation-fraction"])
        self.assertEqual(b"ANS1" + struct.pack("<I3f", 27, 1.0, 1.0, 0.0), frames["reject-orientation-second"])
        self.assertEqual(b"ANS1" + struct.pack("<I3f", 27, 1.0, 0.0, 1.0), frames["reject-orientation-third"])
        self.assertEqual(b"ANS1" + struct.pack("<I3f", 27, math.nan, 0.0, 0.0), frames["reject-orientation-nan"])

    def test_the_control_frames_are_read_as_the_fixture_says(self) -> None:
        frames = golden()
        expected = {
            "control-v1-idle": runner.Control(1, 0, 10_000, 10_000),
            "control-v1-motion": runner.Control(1, 3, 20_000, 1_000_000),
            "control-v2-idle": runner.Control(2, 0, 10_000, 10_000),
            "control-v2-orientation": runner.Control(2, 4, 10_000, 10_000),
            "control-v2-all": runner.Control(2, 7, 20_000, 1_000_000),
        }
        for name, control in expected.items():
            self.assertEqual(runner.parse_control(frames[name]), control, name)
        self.assertTrue(expected["control-v2-orientation"].orientation_active)
        self.assertFalse(expected["control-v2-idle"].orientation_active)
        for name in ("reject-control-v1-orientation", "reject-control-reserved", "reject-control-magic"):
            with self.assertRaises(ValueError, msg=name):
                runner.parse_control(frames[name])
        with self.assertRaises(ValueError):
            runner.parse_control(frames["control-v2-idle"][:15])

    def test_each_posture_is_the_orientation_the_accelerometer_judge_reads_from_it(self) -> None:
        # AccelSensorJudge: orientationAngle = -atan2(-x, y) in degrees, and the nearest quarter turn is
        # the rotation (frameworks/base WindowOrientationListener).
        for value, (x, y, _) in runner.POSTURES.items():
            angle = round(-math.degrees(math.atan2(-x, y))) % 360
            self.assertEqual(((angle + 45) // 90) % 4, value, f"posture {value}")


class ParserTests(unittest.TestCase):
    def test_the_rotation_state_of_a_guest_that_judges_the_accelerometer(self) -> None:
        state = runner.parse_rotation_state(ROTATION_DUMP)
        self.assertEqual((state.rotation, state.user_mode, state.user_rotation, state.allow_all),
                         (0, "USER_ROTATION_FREE", 0, False))
        self.assertEqual((state.landscape, state.seascape), (1, 3))
        self.assertEqual(state.app_orientation, "SCREEN_ORIENTATION_UNSPECIFIED")
        self.assertEqual((state.listener_enabled, state.judge, state.proposed), (True, "AccelSensorJudge", -1))
        self.assertIn("Host Accelerometer", state.sensor or "")

    def test_the_rotation_state_of_a_guest_that_follows_the_orientation_sensor(self) -> None:
        state = runner.parse_rotation_state(ORIENTATION_DUMP)
        self.assertEqual((state.rotation, state.user_mode, state.user_rotation), (3, "USER_ROTATION_LOCKED", 1))
        self.assertEqual((state.judge, state.app_orientation), ("OrientationSensorJudge", "SCREEN_ORIENTATION_SENSOR_LANDSCAPE"))
        self.assertIn("Host Device Orientation", state.sensor or "")

    def test_a_dump_without_the_section_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            runner.parse_rotation_state("Display{#0 state=ON size=720x1280 ROTATION_0}\n")

    def test_the_sensor_service_lists_and_the_active_sensors(self) -> None:
        state = runner.parse_sensor_state(SENSOR_DUMP)
        self.assertEqual(state.listed["Host Device Orientation"], 27)
        self.assertEqual(state.listed["Host Accelerometer"], 1)
        self.assertEqual(state.active, {"Host Device Orientation": 1})
        self.assertIn("type: android.sensor.device_orientation(27)", state.orientation_line or "")
        without = runner.parse_sensor_state(SENSOR_DUMP.replace("Host Device Orientation", "Other"))
        self.assertNotIn("Host Device Orientation", without.listed)
        self.assertIsNone(without.orientation_line)

    def test_the_rotation_suggestion_button_is_systemuis_rotate_screen(self) -> None:
        rotation, nodes = runner.parse_hierarchy(HIERARCHY)
        self.assertEqual(rotation, 1)
        button = runner.suggestion_button(nodes)
        self.assertIsNotNone(button)
        assert button is not None
        self.assertEqual(button.centre, (1140, 640))
        # A "Rotate" in another package is not the button.
        _, others = runner.parse_hierarchy(HIERARCHY.replace('package="com.android.systemui" content-desc="Rotate screen"',
                                                             'package="com.example" content-desc="Rotate screen"'))
        self.assertIsNone(runner.suggestion_button(others))


class RotationRuleTests(unittest.TestCase):
    """DisplayRotation.rotationForOrientation, for the requests the runner makes (a phone: no 180 degree turn)."""

    def rotate(self, request: str, value: int | None, last: int, **options: object) -> int:
        return runner.expected_rotation(request, value, last, **options)  # type: ignore[arg-type]

    def test_an_application_without_a_request_follows_the_sensor_except_upside_down(self) -> None:
        for value, expected in ((0, 0), (1, 1), (3, 3)):
            self.assertEqual(self.rotate("unspecified", value, 0), expected)
        self.assertEqual(self.rotate("unspecified", 2, 1), 1)
        self.assertEqual(self.rotate("unspecified", 2, 1, allow_all=True), 2)
        # Before the host says anything the display keeps its rotation.
        self.assertEqual(self.rotate("unspecified", None, 3), 3)

    def test_a_locked_user_rotation_beats_the_sensor_for_an_application_without_a_request(self) -> None:
        self.assertEqual(self.rotate("unspecified", 3, 0, locked=True, user_rotation=1), 1)
        # A sensor request is the application's own, whatever the lock says.
        self.assertEqual(self.rotate("fullSensor", 3, 0, locked=True, user_rotation=1), 3)

    def test_a_fixed_request_is_not_turned_by_the_sensor(self) -> None:
        for value in range(4):
            self.assertEqual(self.rotate("landscape", value, 0), 1, value)
            self.assertEqual(self.rotate("portrait", value, 1), 0, value)

    def test_sensor_landscape_follows_the_sensor_between_the_two_landscapes(self) -> None:
        self.assertEqual(self.rotate("sensorLandscape", 3, 1), 3)
        self.assertEqual(self.rotate("sensorLandscape", 1, 3), 1)
        # Upright or upside down is no landscape: the display keeps the landscape it has.
        self.assertEqual(self.rotate("sensorLandscape", 0, 3), 3)
        self.assertEqual(self.rotate("sensorLandscape", 2, 1), 1)
        self.assertEqual(self.rotate("sensorLandscape", 0, 0), 1)

    def test_full_sensor_takes_all_four(self) -> None:
        for value in range(4):
            self.assertEqual(self.rotate("fullSensor", value, 0), value)


class FakeGuest:
    """The guest's end of the motion connection on loopback: says a frame on connect, keeps the records, and
    ends the connection on a record it refuses."""

    def __init__(self, first: bytes) -> None:
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(4)
        self.port = self.server.getsockname()[1]
        self.first = first
        self.records: list[bytes] = []
        self.connections = 0
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self) -> None:
        while True:
            try:
                connection, _ = self.server.accept()
            except OSError:
                return
            with self.lock:
                self.connections += 1
            connection.sendall(self.first)
            buffer = b""
            while True:
                try:
                    data = connection.recv(64)
                except OSError:
                    break
                if not data:
                    break
                buffer += data
                while len(buffer) >= runner.RECORD_SIZE:
                    record, buffer = buffer[:runner.RECORD_SIZE], buffer[runner.RECORD_SIZE:]
                    with self.lock:
                        self.records.append(record)
                    if record == golden()["reject-orientation-high"]:
                        connection.close()
                        break
            connection.close()

    def close(self) -> None:
        self.server.close()


class HostTests(unittest.TestCase):
    def test_the_host_reads_frames_sends_records_and_notices_the_guest_ending_the_connection(self) -> None:
        frames = golden()
        guest = FakeGuest(frames["control-v2-orientation"])
        host = runner.Host(guest.port)
        try:
            host.connect()
            control = host.wait_frame(5)
            self.assertEqual(control, runner.Control(2, 4, 10_000, 10_000))
            host.send(runner.orientation_record(3))
            deadline = threading.Event()
            for _ in range(100):
                with guest.lock:
                    if guest.records:
                        break
                deadline.wait(0.05)
            self.assertEqual(guest.records, [frames["orientation-3"]])
            host.send(runner.orientation_record(4))
            self.assertTrue(host.closed_by_guest(5))
            # A new connection starts again: its frames are its own.
            host.connect()
            self.assertEqual(host.wait_frame(5), runner.Control(2, 4, 10_000, 10_000))
            self.assertFalse(host.closed)
            self.assertEqual(guest.connections, 2)
        finally:
            host.close()
            guest.close()

    def test_a_host_with_no_guest_listening_gets_no_frame(self) -> None:
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
        listener.listen(1)
        # The launcher's forward accepts and closes when the guest does not listen.
        threading.Thread(target=lambda: listener.accept()[0].close(), daemon=True).start()
        host = runner.Host(port)
        try:
            host.connect()
            self.assertIsNone(host.wait_frame(2))
        finally:
            host.close()
            listener.close()


class ProbeApplicationTests(unittest.TestCase):
    def test_the_manifest_asks_for_each_orientation_from_a_framework_activity(self) -> None:
        tree = probe.decode_manifest(probe.encode_manifest())
        self.assertEqual(tree.tag, "manifest")
        self.assertEqual(tree.attributes["package"], probe.PACKAGE)
        sdk = next(child for child in tree.children if child.tag == "uses-sdk")
        self.assertEqual((sdk.attributes["minSdkVersion"], sdk.attributes["targetSdkVersion"]), (29, 29))
        application = next(child for child in tree.children if child.tag == "application")
        self.assertIs(application.attributes["hasCode"], False)
        found = {child.attributes["name"]: child.attributes["screenOrientation"]
                 for child in application.children if child.tag == "activity"}
        self.assertEqual(found, {probe.ACTIVITY_CLASSES[name]: value for name, value in probe.ORIENTATIONS.items()})
        self.assertEqual(sorted(probe.ORIENTATIONS.values()), [0, 1, 6, 10])
        for child in application.children:
            self.assertIs(child.attributes["exported"], True)
            action = child.children[0].children[0]
            self.assertEqual(action.attributes["name"], "android.intent.action.MAIN")
        self.assertEqual(probe.component("sensorLandscape"),
                         f"{probe.PACKAGE}/android.app.ExpandableListActivity")

    def test_the_attributes_are_in_the_order_of_their_resource_ids(self) -> None:
        # Android reads a binary manifest's attributes by resource id, in order.
        data = probe.encode_manifest()
        ids = list(probe.ATTRIBUTES.values())
        self.assertEqual(ids, sorted(ids))
        self.assertIn(struct.pack("<8I", *ids), data)

    @unittest.skipUnless(shutil.which("keytool") and shutil.which("jarsigner"), "needs a JDK")
    def test_the_package_is_signed_with_a_throwaway_key(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            apk = Path(directory) / "probe.apk"
            probe.build_apk(apk, Path(directory))
            self.assertTrue(apk.is_file())
            result = subprocess.run(["jarsigner", "-verify", str(apk)], capture_output=True, text=True, check=False)
            self.assertIn("jar verified", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
