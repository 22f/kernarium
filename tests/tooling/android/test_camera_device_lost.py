#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The camera HAL tells the framework that the device is gone when the host has ended a camera.

When the application leaves the foreground while the guest records, the host ends the video of the camera: its video
buffers fail under a black, interrupted metadata record for as long as the guest keeps the stream (the wire README of
kernel/common/video). The HAL (route p0030, its last topic) must turn that into ERROR_DEVICE, which Camera2 hands to the
application as onError(ERROR_CAMERA_DEVICE) and CameraX answers by closing the camera, so that a recording it holds ends with
the clip saved. Without it the buffers were failed buffers, which an application cannot tell from a hiccup, and the recording
ran on, dark.

The decision itself (camera_lost) is a unit row of the overlay header. What this row holds is the wiring in the HAL sources, from
the series' own patches: the frame source asks the rule only of a failed buffer of a video stream and latches the answer, the
sensor reports ERROR_DEVICE for a request of a camera that is lost and ERROR_REQUEST for any other request it fails as a whole,
and nothing else reports ERROR_DEVICE. Nothing here boots anything.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

REPOSITORY = Path(__file__).resolve().parents[3]
SERIES = REPOSITORY / "android/lineage-24/patches/projects/p0030/series"
HWL = "devices/EmulatedCamera/hwl"
FRAME_SOURCE = f"{HWL}/IFrameSource.h"
V4L2_HEADER = f"{HWL}/V4l2FrameSource.h"
V4L2_SOURCE = f"{HWL}/V4l2FrameSource.cpp"
SENSOR = f"{HWL}/EmulatedSensorHost.cpp"


def patch_names() -> list[str]:
    return [line.split("\t", 1)[1] for line in (SERIES / "series.tsv").read_text(encoding="utf-8").splitlines()
            if line.startswith("patch\t")]


def rebuilt_files() -> dict[str, str]:
    """The three sources that the series creates, as the whole series leaves them: every patch that touches one is applied, in
    the order of series.tsv, to an empty repository, limited to those paths."""
    paths = (V4L2_HEADER, V4L2_SOURCE, SENSOR)
    with tempfile.TemporaryDirectory(prefix="camera-lost-") as directory:
        subprocess.run(["git", "init", "-q", directory], check=True)
        for name in patch_names():
            text = (SERIES / name).read_text(encoding="utf-8")
            if not any(f"diff --git a/{path} " in text for path in paths):
                continue
            subprocess.run(["git", "-C", directory, "apply", *(f"--include={path}" for path in paths), str(SERIES / name)],
                           check=True)
        return {path: (Path(directory) / path).read_text(encoding="utf-8") for path in paths}


def added_lines(path: str) -> str:
    """The lines that the series adds to a file that the base already has (the interface header), in series order."""
    added: list[str] = []
    for name in patch_names():
        inside = False
        for line in (SERIES / name).read_text(encoding="utf-8").splitlines():
            if line.startswith("diff --git "):
                inside = line.startswith(f"diff --git a/{path} ")
            elif inside and line.startswith("+") and not line.startswith("+++"):
                added.append(line[1:])
    return "\n".join(added)


def block(source: str, opening: str) -> str:
    """The text between the braces that follow the first `opening` (up to and including its `{`), by brace depth."""
    start = source.index(opening) + len(opening)
    depth = 1
    for position in range(start, len(source)):
        depth += {"{": 1, "}": -1}.get(source[position], 0)
        if depth == 0:
            return source[start:position]
    raise AssertionError(f"{opening!r} is never closed")


class DeviceLostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        files = rebuilt_files()
        cls.header = files[V4L2_HEADER]
        cls.source = files[V4L2_SOURCE]
        cls.sensor = files[SENSOR]
        cls.interface = added_lines(FRAME_SOURCE)

    def test_a_frame_source_is_not_lost_unless_it_says_so(self) -> None:
        self.assertRegex(self.interface, r"virtual bool DeviceLost\(\) const \{\s*return false;\s*\}")

    def test_the_v4l2_source_latches_the_loss_and_reports_it(self) -> None:
        self.assertRegex(self.header, r"bool DeviceLost\(\) const override \{\s*return device_lost_;\s*\}")
        self.assertRegex(self.header, r"std::atomic<bool> device_lost_\{false\};")
        self.assertIn("camera_request.hpp", self.header, "the rule lives in the overlay's request header")

    def test_only_a_failed_buffer_of_a_video_stream_is_asked_about_the_loss(self) -> None:
        # The one place that notes a failed buffer is the video path's error branch: a failed still (ProduceBlob) never ends a
        # camera, and nothing that does not fail is asked.
        self.assertEqual(self.source.count("NoteFailedBuffer(camera, *pending.stream)"), 1)
        produce_frame = block(self.source[self.source.index("status_t V4l2FrameSource::ProduceFrame("):], ") {")
        failed = block(produce_frame, "if (completion.error) {")
        self.assertRegex(failed, r"NoteFailedBuffer\(camera, \*pending\.stream\);\s*return UNKNOWN_ERROR;")
        produce_blob = self.source[self.source.index("status_t V4l2FrameSource::ProduceBlob("):]
        produce_blob = produce_blob[:produce_blob.index("\n}\n")]
        self.assertNotIn("NoteFailedBuffer", produce_blob)

    def test_the_decision_is_the_overlay_rule_over_the_newest_record(self) -> None:
        note = block(self.source, "void V4l2FrameSource::NoteFailedBuffer(Camera* camera, const V4l2Stream& stream) {")
        self.assertIn("camera->meta->Latest(&latest)", note, "the camera's newest metadata record decides, not a guess")
        self.assertRegex(note, r"andarium::camera::camera_lost\(\s*stream\.params\(\)\.kind == V4l2Stream::Kind::kVideo,\s*true,")
        self.assertIn("device_lost_.exchange(true)", note, "the loss is latched, and logged once")

    def test_the_sensor_reports_the_device_gone_for_a_lost_camera_and_a_request_error_otherwise(self) -> None:
        self.assertIn("const bool device_lost = frame_source_->DeviceLost();", self.sensor)
        # One branch fails the request as a whole: for a lost camera whatever else was produced, otherwise by the source's rule.
        failing = block(self.sensor, "if (device_lost || frame_source_->FailsRequest(outputs, failed)) {")
        self.assertRegex(failing, r"is_failed_request = true;")
        self.assertRegex(failing, r"\.error_code = device_lost \? ErrorCode::kErrorDevice : ErrorCode::kErrorRequest")
        self.assertIn("result->result_metadata.reset();", failing, "a failed request carries no results")

    def test_nothing_else_reports_the_device_gone(self) -> None:
        # ERROR_DEVICE closes the camera for the application; an ordinary hiccup must never take that path.
        self.assertEqual(self.sensor.count("ErrorCode::kErrorDevice"), 1)
        self.assertNotIn("kErrorDevice", self.source)

    def test_the_topic_is_the_last_one_of_the_series(self) -> None:
        last = (SERIES / patch_names()[-1]).read_text(encoding="utf-8")
        self.assertIn("DeviceLost", last)
        self.assertIn("kErrorDevice", last)


if __name__ == "__main__":
    unittest.main()
