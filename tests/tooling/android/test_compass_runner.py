#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The compass runner's wire, its reading of the guest, its oracle and its host, without a guest.

run_android_compass.py plays the host application against a booted release: it reads the guest's control frames
("ANC1", "ANC2" and the 20-byte "ANC3"), sends the records of the accelerometer, the gyroscope and the magnetic
field, and reads Android's sensor service, package manager and the probe's report. The frames are the golden
frames of tests/unit/andarium/fixtures; the texts below have the layout the guest's commands print.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
import math
import socket
import struct
import subprocess
import sys
import threading
import unittest
from pathlib import Path

TESTS = Path(__file__).resolve().parents[2]
SCRIPT = TESTS / "integration/android/run_android_compass.py"
SPEC = importlib.util.spec_from_file_location("run_android_compass", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
# Dataclasses resolve their annotations through sys.modules.
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)
FIXTURE = TESTS / "unit/andarium/fixtures/motion-orientation-golden-frames.txt"


def golden() -> dict[str, bytes]:
    frames = {}
    for line in FIXTURE.read_text().splitlines():
        if line.startswith("#") or ":" not in line:
            continue
        name, _, hex_bytes = line.partition(":")
        frames[name.strip()] = bytes.fromhex(hex_bytes.strip())
    return frames


SENSOR_DUMP = """\
Sensor List:
0x00000001) Host Accelerometer        | Andarium        | ver: 1 | type: android.sensor.accelerometer(1) | perm: n/a | flags: 0x00000000
\tcontinuous | minRate=1.00Hz | maxRate=100.00Hz | no batching | non-wakeUp |
0x00000002) Host Gyroscope            | Andarium        | ver: 1 | type: android.sensor.gyroscope(4) | perm: n/a | flags: 0x00000000
\tcontinuous | minRate=1.00Hz | maxRate=100.00Hz | no batching | non-wakeUp |
0x00000004) Host Magnetometer         | Andarium        | ver: 1 | type: android.sensor.magnetic_field(2) | perm: n/a | flags: 0x00000000
\tcontinuous | minRate=1.00Hz | maxRate=50.00Hz | no batching | non-wakeUp |
0x5f676172) Game Rotation Vector Sensor | AOSP            | ver: 3 | type: android.sensor.game_rotation_vector(15) | perm: n/a | flags: 0x00000000
\tcontinuous | maxDelay=0us | maxRate=100.00Hz | no batching | non-wakeUp |
Active sensors:
Socket Buffer size = 39 events
"""
OLDER_DUMP = "\n".join(
    line for line in SENSOR_DUMP.splitlines()
    if "Magnetometer" not in line and "maxRate=50.00Hz" not in line) + "\n"
HELD_DUMP = SENSOR_DUMP.replace(
    "Active sensors:\n", "Active sensors:\nHost Magnetometer (handle=0x00000004, connections=1)\n")
FEATURES = "feature:android.hardware.touchscreen\nfeature:android.hardware.sensor.compass\nfeature:reqGlEsVersion=0x30002\n"
LOG_LINE = ("11-13 09:00:01.000   439   439 I MotionSubHal: Andarium motion: ro.boot.kernarium.orientation=%d, "
            "%d sensors, ro.boot.kernarium.compass=%d")
PROBE_REPORT = ('{"mode":"compass","run_id":"1","valid":true,"accelerometer":{"count":100},'
                '"gyroscope":{"count":100},"magnetometer":{"count":50,"latest":[0.0,25.0,-40.0],"accuracy":3,'
                '"azimuth_deg":0.4},"rotation_vector":{"count":40,"latest":[0,0,0,1],"azimuth_deg":null}}')


class WireTests(unittest.TestCase):
    def test_the_records_are_the_golden_frames(self) -> None:
        frames = golden()
        for status in range(4):
            self.assertEqual(runner.magnetic_record(0.0, 25.0, -40.0, status), frames[f"magnetic-status-{status}"])
        self.assertEqual(runner.sensor_record(1, 0.0, 0.0, runner.GRAVITY), frames["accelerometer-gravity"])
        self.assertEqual(runner.sensor_record(4, 1.0, -2.0, 0.5), frames["gyroscope-turn"])
        self.assertEqual(len(runner.magnetic_record(0, 0, 0)), 20)

    def test_the_records_the_guest_refuses_are_the_golden_frames(self) -> None:
        frames = golden()
        self.assertEqual(runner.magnetic_record(0.0, 25.0, -40.0, 4), frames["reject-magnetic-status-high"])
        self.assertEqual(runner.magnetic_record(0.0, 25.0, -40.0, 255), frames["reject-magnetic-status-byte"])
        self.assertEqual(b"ANS1" + struct.pack("<HBB3f", 2, 0, 1, 0.0, 25.0, -40.0), frames["reject-magnetic-reserved"])
        self.assertEqual(runner.magnetic_record(math.nan, 0.0, 0.0), frames["reject-magnetic-nan"])
        self.assertEqual(runner.magnetic_record(0.0, math.inf, 0.0), frames["reject-magnetic-infinity"])
        self.assertEqual(runner.magnetic_record(0.0, 0.0, -math.inf), frames["reject-magnetic-negative-infinity"])
        self.assertEqual(runner.sensor_record(1, 0.0, 0.0, runner.GRAVITY, 1), frames["reject-accelerometer-status"])
        self.assertEqual(runner.sensor_record(4, 1.0, -2.0, 0.5, 3), frames["reject-gyroscope-status"])
        self.assertEqual(runner.sensor_record(3, 0.0, 0.0, 0.0), frames["reject-type-orientation-angles"])
        self.assertEqual(runner.sensor_record(14, 0.0, 25.0, -40.0), frames["reject-type-uncalibrated-field"])

    def test_the_control_frames_are_read_as_the_fixture_says(self) -> None:
        frames = golden()
        expected = {
            "control-v1-idle": runner.Frame(1, 0, 10_000, 10_000),
            "control-v1-motion": runner.Frame(1, 3, 20_000, 1_000_000),
            "control-v2-orientation": runner.Frame(2, 4, 10_000, 10_000),
            "control-v3-idle": runner.Frame(3, 0, 10_000, 10_000, 20_000),
            "control-v3-magnetometer": runner.Frame(3, 8, 10_000, 10_000, 50_000),
            "control-v3-orientation-idle": runner.Frame(3, 16, 10_000, 10_000, 20_000),
            "control-v3-orientation": runner.Frame(3, 20, 10_000, 10_000, 20_000),
            "control-v3-all": runner.Frame(3, 31, 20_000, 1_000_000, 40_000),
        }
        for name, frame in expected.items():
            self.assertEqual(runner.parse_frame(frames[name]), frame, name)
        self.assertTrue(expected["control-v3-magnetometer"].magnetometer_active)
        self.assertFalse(expected["control-v3-idle"].magnetometer_active)
        self.assertFalse(runner.parse_frame(frames["control-v2-all"]).magnetometer_active)
        # The orientation sensor is listed by "ANC2" always and by "ANC3" by bit 4.
        self.assertTrue(runner.parse_frame(frames["control-v2-idle"]).orientation_listed)
        self.assertFalse(runner.parse_frame(frames["control-v1-idle"]).orientation_listed)
        self.assertFalse(runner.parse_frame(frames["control-v3-idle"]).orientation_listed)
        self.assertTrue(runner.parse_frame(frames["control-v3-orientation-idle"]).orientation_listed)
        self.assertTrue(runner.parse_frame(frames["control-v3-all"]).orientation_active)

    def test_every_reject_control_frame_of_the_fixture_is_refused(self) -> None:
        frames = golden()
        rejected = [name for name in frames if name.startswith("reject-control-")]
        self.assertEqual(len(rejected), 8)
        for name in rejected:
            with self.assertRaises(ValueError, msg=name):
                runner.parse_frame(frames[name])

    def test_the_magic_says_how_long_a_frame_is(self) -> None:
        frames = golden()
        self.assertEqual(runner.frame_length(frames["control-v1-idle"][:4]), 16)
        self.assertEqual(runner.frame_length(frames["control-v2-idle"][:4]), 16)
        self.assertEqual(runner.frame_length(frames["control-v3-idle"][:16]), 20)
        self.assertIsNone(runner.frame_length(frames["reject-control-magic"][:4]))
        self.assertIsNone(runner.frame_length(b"ANS1"))
        with self.assertRaises(ValueError):
            runner.parse_frame(frames["control-v3-idle"][:16])
        with self.assertRaises(ValueError):
            runner.parse_frame(frames["control-v3-idle"] + b"\0")


class OracleTests(unittest.TestCase):
    def test_a_mount_turns_the_field_like_the_applications_axes(self) -> None:
        # The application's own test (MotionCompassTests): a field of (20, -5, -40) in each mount.
        self.assertEqual(runner.mounted("portrait", 20, -5, -40), (20, -5, -40))
        self.assertEqual(runner.mounted("landscapeLeft", 20, -5, -40), (-5, -20, -40))
        self.assertEqual(runner.mounted("landscapeRight", 20, -5, -40), (5, 20, -40))
        self.assertEqual(runner.mounted("portraitUpsideDown", 20, -5, -40), (-20, 5, -40))
        with self.assertRaises(ValueError):
            runner.mounted("flat", 0, 0, 0)

    def test_the_azimuth_of_each_mount_is_what_the_platform_computes(self) -> None:
        gravity = (0.0, 0.0, runner.GRAVITY)
        for mount, heading in runner.MOUNTS.items():
            field = runner.mounted(mount, 0.0, runner.HORIZONTAL, runner.VERTICAL)
            self.assertLess(runner.angle_difference(runner.azimuth(gravity, field), heading), 1e-6, mount)
        self.assertEqual(runner.MOUNTS, {"portrait": 0.0, "portraitUpsideDown": 180.0,
                                         "landscapeLeft": -90.0, "landscapeRight": 90.0})

    def test_a_turn_through_the_compass_points_gives_those_azimuths(self) -> None:
        gravity = (0.0, 0.0, runner.GRAVITY)
        for heading in (0.0, 90.0, 180.0, 270.0, 45.0, 315.0):
            field = runner.field_at_heading(heading)
            self.assertLess(runner.angle_difference(runner.azimuth(gravity, field), heading), 1e-6, heading)
            # The field's strength does not depend on the heading.
            self.assertAlmostEqual(math.sqrt(sum(v * v for v in field)), math.hypot(runner.HORIZONTAL, runner.VERTICAL))
        self.assertAlmostEqual(runner.field_at_heading(0.0)[0], 0.0)
        # A field straight along gravity gives no azimuth.
        with self.assertRaises(ValueError):
            runner.azimuth(gravity, (0.0, 0.0, -40.0))

    def test_the_difference_of_angles_wraps(self) -> None:
        self.assertEqual(runner.angle_difference(179.0, -179.0), 2.0)
        self.assertEqual(runner.angle_difference(0.0, 270.0), 90.0)
        self.assertEqual(runner.angle_difference(-90.0, 270.0), 0.0)


class ParserTests(unittest.TestCase):
    def test_the_features_are_the_names_after_feature(self) -> None:
        features = runner.parse_features(FEATURES)
        self.assertIn("android.hardware.sensor.compass", features)
        self.assertIn("reqGlEsVersion", features)
        self.assertNotIn("android.hardware.sensor.barometer", features)
        self.assertEqual(runner.parse_features("nothing"), set())

    def test_the_sub_hals_log_line_with_and_without_the_compass_property(self) -> None:
        self.assertEqual(runner.HAL_LOG_LINE.findall(LOG_LINE % (1, 4, 1)), [("1", "4", "1")])
        self.assertEqual(runner.HAL_LOG_LINE.findall(LOG_LINE % (0, 2, 0)), [("0", "2", "0")])
        # An older image's line has no compass part.
        older = "I MotionSubHal: Andarium motion: ro.boot.kernarium.orientation=1, 3 sensors"
        self.assertEqual(runner.HAL_LOG_LINE.findall(older), [("1", "3", "")])
        # The orientation runner reads the same line by its own, older pattern.
        self.assertEqual(runner.orient.HAL_LOG_LINE.findall(LOG_LINE % (1, 4, 1)), [("1", "4")])

    def test_the_probes_report(self) -> None:
        report = runner.parse_probe_report(PROBE_REPORT)
        self.assertEqual(report["magnetometer"]["latest"], [0.0, 25.0, -40.0])
        self.assertEqual(report["magnetometer"]["accuracy"], 3)
        for text in ("", "cat: files/compass.json: No such file or directory", "[1]", '{"mode":"events"}'):
            with self.assertRaises(ValueError, msg=text):
                runner.parse_probe_report(text)

    def test_the_sensor_service_lists_the_magnetometer(self) -> None:
        state = runner.orient.parse_sensor_state(SENSOR_DUMP)
        self.assertEqual(state.listed["Host Magnetometer"], 2)
        self.assertEqual(state.active, {})
        held = runner.orient.parse_sensor_state(HELD_DUMP)
        self.assertEqual(held.active, {"Host Magnetometer": 1})
        description = runner.orient.sensor_description(SENSOR_DUMP, "Host Magnetometer")
        self.assertIn("continuous", description or "")
        self.assertIn("maxRate=50.00Hz", description or "")


class FakeGuest:
    """The guest's end of the motion connection on loopback: says its frames, keeps the records, and ends the
    connection on a record of a status above 3 or of a sensor it does not list."""

    def __init__(self, first: bytes, takes_field: bool = True) -> None:
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(4)
        self.port = self.server.getsockname()[1]
        self.first = first
        self.takes_field = takes_field
        self.records: list[bytes] = []
        self.connections: list[socket.socket] = []
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
                self.connections.append(connection)
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
                while len(buffer) >= 20:
                    record, buffer = buffer[:20], buffer[20:]
                    with self.lock:
                        self.records.append(record)
                    if record[4] == 2 and (not self.takes_field or record[6] > 3):
                        connection.close()
                        buffer = b""
                        break
            connection.close()

    def say(self, frame: bytes, *, pieces: int = 1) -> None:
        """Another frame to the newest connection, in pieces if asked."""
        with self.lock:
            connection = self.connections[-1]
        step = -(-len(frame) // pieces)
        for start in range(0, len(frame), step):
            connection.sendall(frame[start:start + step])
            threading.Event().wait(0.05)

    def wait_records(self, predicate, seconds: float = 5.0) -> list[bytes]:
        for _ in range(int(seconds / 0.05)):
            with self.lock:
                found = [record for record in self.records if predicate(record)]
            if found:
                return found
            threading.Event().wait(0.05)
        return []

    def forget(self) -> None:
        with self.lock:
            self.records.clear()

    def close(self) -> None:
        self.server.close()
        with self.lock:
            for connection in self.connections:
                connection.close()


class HostTests(unittest.TestCase):
    def test_the_host_reads_the_three_frames_and_notices_the_guest_ending_the_connection(self) -> None:
        frames = golden()
        guest = FakeGuest(frames["control-v3-idle"])
        host = runner.Host(guest.port)
        try:
            host.connect()
            self.assertEqual(host.wait_frame(5), runner.Frame(3, 0, 10_000, 10_000, 20_000))
            host.send(runner.magnetic_record(0.0, 25.0, -40.0, 3))
            self.assertEqual(guest.wait_records(lambda record: record[4] == 2), [frames["magnetic-status-3"]])
            host.send(runner.magnetic_record(0.0, 25.0, -40.0, 4))
            self.assertTrue(host.closed_by_guest(5))
            host.connect()
            self.assertEqual(host.wait_frame(5), runner.Frame(3, 0, 10_000, 10_000, 20_000))
            self.assertFalse(host.closed)
        finally:
            host.stop()
            guest.close()

    def test_a_frame_that_arrives_in_pieces_is_waited_for(self) -> None:
        frames = golden()
        guest = FakeGuest(frames["control-v1-idle"])
        host = runner.Host(guest.port)
        try:
            host.connect()
            self.assertEqual(host.wait_frame(5), runner.Frame(1, 0, 10_000, 10_000))
            # The first 16 bytes of an "ANC3" frame are the start of one, not a frame of 16 bytes.
            guest.say(frames["control-v3-all"], pieces=5)
            self.assertEqual(host.wait_frame(5, lambda frame: frame.version == 3),
                             runner.Frame(3, 31, 20_000, 1_000_000, 40_000))
            guest.say(frames["control-v1-motion"] + frames["control-v3-magnetometer"])
            self.assertIsNotNone(host.wait_frame(5, lambda frame: frame.magnetometer_us == 50_000))
            self.assertEqual(host.bad, [])
        finally:
            host.stop()
            guest.close()

    def test_a_magic_no_frame_has_is_kept_apart(self) -> None:
        guest = FakeGuest(golden()["reject-control-magic"])
        host = runner.Host(guest.port)
        try:
            host.connect()
            for _ in range(100):
                if host.bad:
                    break
                threading.Event().wait(0.05)
            self.assertEqual(host.frames, [])
            self.assertEqual(len(host.bad), 1)
        finally:
            host.stop()
            guest.close()

    def test_a_field_goes_out_only_while_the_latest_frame_says_the_magnetometer_is_in_use(self) -> None:
        frames = golden()
        guest = FakeGuest(frames["control-v3-idle"])
        host = runner.Host(guest.port)
        try:
            host.connect()
            self.assertIsNotNone(host.wait_frame(5))
            host.stream((0.0, 25.0, -40.0), 2)
            threading.Event().wait(0.5)
            # Nothing is in use: nothing is sent.
            self.assertEqual(guest.wait_records(lambda record: True, 0.5), [])
            guest.say(frames["control-v3-magnetometer"])
            fields = guest.wait_records(lambda record: record[4] == 2)
            self.assertTrue(fields)
            self.assertEqual(fields[0], frames["magnetic-status-2"])
            # The accelerometer and the gyroscope are not in use, so none of theirs is sent.
            self.assertEqual([record for record in guest.records if record[4] != 2], [])
            guest.say(frames["control-v3-all"])
            self.assertTrue(guest.wait_records(lambda record: record[4] == 1))
            self.assertTrue(guest.wait_records(lambda record: record[4] == 4))
            # The magnetometer idle again: the field stops.
            guest.say(frames["control-v3-idle"])
            threading.Event().wait(0.3)
            guest.forget()
            threading.Event().wait(0.5)
            self.assertEqual(guest.wait_records(lambda record: True, 0.3), [])
        finally:
            host.stop()
            guest.close()

    def test_a_guest_without_the_sensor_is_never_sent_a_field_by_the_streamer(self) -> None:
        frames = golden()
        guest = FakeGuest(frames["control-v2-all"], takes_field=False)
        host = runner.Host(guest.port)
        try:
            host.connect()
            self.assertIsNotNone(host.wait_frame(5))
            host.stream((0.0, 25.0, -40.0), 3)
            self.assertTrue(guest.wait_records(lambda record: record[4] == 1))
            threading.Event().wait(0.3)
            self.assertEqual([record for record in guest.records if record[4] == 2], [])
            self.assertFalse(host.closed)
        finally:
            host.stop()
            guest.close()


class ScriptedGuest(runner.Guest):
    """A guest that answers the commands the launch check runs with the text a booted release prints."""

    def __init__(self, *, property_value: str, dump: str, features: str, has_feature: str, log: str) -> None:  # no adb
        self.property_value, self.dump, self.features, self.has_feature, self.log = (
            property_value, dump, features, has_feature, log)
        self.early_log = ""
        self.commands: list[str] = []

    def shell(self, command: str, timeout: float = 0) -> str:
        self.commands.append(command)
        if command.startswith("getprop"):
            return self.property_value + "\n"
        if command == "dumpsys sensorservice":
            return self.dump
        if command == "pm list features":
            return self.features
        if command.startswith("logcat"):
            return self.log
        raise AssertionError(command)

    def adb_run(self, *arguments: str, timeout: float = 0) -> subprocess.CompletedProcess:
        # `cmd package has-feature` exits 1 and prints "false" for a feature the device does not have.
        self.commands.append(arguments[-1])
        assert arguments[-1].startswith("cmd package has-feature"), arguments
        return subprocess.CompletedProcess(
            arguments, 0 if self.has_feature == "true" else 1, stdout=self.has_feature + "\n")

    def alive(self) -> None:
        pass


class LaunchStateTests(unittest.TestCase):
    def sensor_guest(self, **changes: str) -> ScriptedGuest:
        settings = dict(property_value="1", dump=SENSOR_DUMP, features=FEATURES, has_feature="true",
                        log=LOG_LINE % (0, 3, 1))
        settings.update(changes)
        return ScriptedGuest(**settings)

    def older_guest(self, **changes: str) -> ScriptedGuest:
        settings = dict(property_value="", dump=OLDER_DUMP,
                        features=FEATURES.replace("feature:android.hardware.sensor.compass\n", ""),
                        has_feature="false", log=LOG_LINE % (0, 2, 0))
        settings.update(changes)
        return ScriptedGuest(**settings)

    def test_a_release_with_the_argument_lists_the_sensor_and_declares_the_feature(self) -> None:
        note = runner.launch_state(self.sensor_guest(), True, False)
        self.assertIn("listed (type 2), continuous, non-wakeUp, 1 Hz to 50 Hz", note)
        self.assertIn("android.hardware.sensor.compass declared", note)
        self.assertIn("3 sensors, compass=1", note)

    def test_with_the_orientation_sensor_too_the_list_has_four(self) -> None:
        guest = self.sensor_guest(log=LOG_LINE % (1, 4, 1))
        self.assertIn("4 sensors", runner.launch_state(guest, True, True))
        with self.assertRaisesRegex(runner.Failure, "expected 3 sensors"):
            runner.launch_state(self.sensor_guest(log=LOG_LINE % (1, 4, 1)), True, False)

    def test_a_release_without_the_argument_lists_nothing_and_declares_nothing(self) -> None:
        note = runner.launch_state(self.older_guest(), False, False)
        self.assertIn("no magnetic field sensor", note)
        self.assertIn("not declared", note)
        # An older image's own line has no compass part.
        older = "I MotionSubHal: Andarium motion: ro.boot.kernarium.orientation=0, 2 sensors"
        self.assertIn("compass=not said", runner.launch_state(self.older_guest(log=older), False, False))

    def test_an_older_release_given_the_argument_reads_it_and_lists_nothing(self) -> None:
        # The application passes the argument to every image; an older one has the property and no consumer of it.
        guest = self.older_guest(property_value="1", log="I MotionSubHal: Andarium motion: "
                                 "ro.boot.kernarium.orientation=0, 2 sensors")
        self.assertIn("no magnetic field sensor", runner.launch_state(guest, False, False, True))
        with self.assertRaisesRegex(runner.Failure, "reads '1', expected nothing"):
            runner.launch_state(guest, False, False)
        with self.assertRaisesRegex(runner.Failure, "reads '', expected '1'|reads '', expected 1"):
            runner.launch_state(self.older_guest(), False, False, True)

    def test_each_way_the_launch_state_can_be_wrong(self) -> None:
        cases = [
            ("the property is unset", self.sensor_guest(property_value=""), True, "reads ''"),
            ("the sensor is not listed", self.sensor_guest(dump=OLDER_DUMP), True, "not Host Magnetometer"),
            ("the sensor is not 1 Hz to 50 Hz",
             self.sensor_guest(dump=SENSOR_DUMP.replace("maxRate=50.00Hz", "maxRate=100.00Hz")), True,
             "not listed at 1 Hz to 50 Hz"),
            ("the sensor is on-change", self.sensor_guest(dump=SENSOR_DUMP.replace(
                "continuous | minRate=1.00Hz | maxRate=50.00Hz", "on-change | maxDelay=0us")), True,
             "not continuous and non-wake-up"),
            ("the feature is missing", self.sensor_guest(features="feature:android.hardware.touchscreen\n"), True,
             "pm list features says False"),
            ("the package manager disagrees", self.sensor_guest(has_feature="false"), True, "has-feature says 'false'"),
            ("a denial", self.sensor_guest(log=LOG_LINE % (0, 3, 1) + "\navc:  denied  { read } for "
                                           "property=ro.boot.kernarium.compass scontext=u:r:system_server:s0"),
             True, "SELinux denials"),
            ("the log says another count", self.sensor_guest(log=LOG_LINE % (0, 2, 1)), True, "expected 3 sensors"),
            ("the log says the property is off", self.sensor_guest(log=LOG_LINE % (0, 3, 0)), True, "compass=0"),
            ("held at boot", self.sensor_guest(dump=HELD_DUMP), True, "held at boot"),
            ("listed without the argument", self.older_guest(dump=SENSOR_DUMP), False, "without the argument"),
            ("a feature without the argument", self.older_guest(features=FEATURES, has_feature="true"), False,
             "expected False"),
            ("the property is set without the argument", self.older_guest(property_value="1"), False, "reads '1'"),
        ]
        for label, guest, compass, message in cases:
            with self.subTest(label), self.assertRaisesRegex(runner.Failure, message):
                runner.launch_state(guest, compass, False)


if __name__ == "__main__":
    unittest.main(verbosity=2)
