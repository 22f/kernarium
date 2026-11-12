#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Smoke-test an Android release's cameras against the launcher's synthetic camera.

The guest is the release on a fresh data disk, started with the macOS launcher, ``--camera=synthetic``
(or ``blocked``) and ``--camera-profile=``, and with the camera layout on the vendor SKU
(``androidboot.product.vendor.sku=<layout>``), as the host's application composes it. The contract is
android/lineage-24/README.md ("Camera") and the layout set is android/lineage-24/tools/camera_layouts.py.
The runner checks, in this order:

- the layout reached Android: the SKU, the media profiles variant that its init trigger selects, and
  exactly the camera features the layout earns (and none beside them), which are also the features the
  characteristics of the cameras Android lists earn (so that a layout that disagrees with the cameras
  is caught, not only a guest that disagrees with the layout);
- the cameras Android lists (``dumpsys media.camera``) are the profile's: "0" the back zoom camera (the
  primary back lens of a host without one, the front camera of a host without a back camera), "1" the
  front camera, and "2", "3" and "4" the wide, ultra wide and telephoto lenses a phone has;
- the provider's life (live and blocked): it is a lazy service that init starts with cameraserver, so it
  exits once cameraserver has listed the cameras and holds none, an open of the camera starts it again
  through its interface, and it exits again when the application is stopped, with the cameras still listed
  (``ps -A`` is all a user build offers; a restart of cameraserver itself needs root, so that path is
  the init action the target-files verifier requires, not something this runner exercises);
- live: the camera application previews the synthetic chart (a screenshot is not black), and the camera
  key takes one still, a complete JPEG with Exif data inside the layout's size cap; then, on the same
  boot, it records about four seconds of video with the back camera (a movie file of at least a second
  with a video track), previews and takes a still with the front camera when the profile has one, and the
  camera HAL has logged no failed buffer ("every buffer of the stream is in flight", a request "not
  queued completely", a buffer "never collected");
- blocked: the same cameras are listed, the preview is not the chart on two samples, each taken only while
  the camera application is the resumed activity and the device is awake (the application is brought back
  once; a second absence fails the step), and the camera key takes nothing;
- none: no camera feature and no camera at all.

Every wait has a deadline. The guest's launcher is stopped through its own process on any exit, and the
guest is reached through the shared adb server, always named with ``-s``.

The runner is opt-in: it needs the macos-android17 executable, an installed release and an adb client.
usage: run_android_camera.py --emulator uml-darwin-android17 [--release DIR] [--layout NAME]
       [--camera-profile phone|tablet|laptop] [--mode live|blocked|none]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import math
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from collections.abc import Callable
from dataclasses import dataclass

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import slot_ports  # noqa: E402


def load(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


boot_android = load("boot_android", REPOSITORY / "tools" / "boot_android.py")
camera_layouts = load("camera_layouts", REPOSITORY / "android/lineage-24/tools/camera_layouts.py")

DEFAULT_ADB_PORT = 5580
CAMERA_PACKAGE = "org.lineageos.aperture"
CAMERA_PERMISSIONS = ("android.permission.CAMERA", "android.permission.RECORD_AUDIO")
CAMERA_KEYCODE = 27  # KEYCODE_CAMERA: the camera application's default shutter key
STILL_DIRECTORY = "/sdcard/DCIM/Camera"
# The lazy provider's process, as `ps -A` names it (its binary: android.hardware.camera.provider@2.7-service-google-lazy),
# and how long it may run after the last camera is closed: the service manager tells a lazy service that it
# has no clients on a timer of seconds.
PROVIDER_PROCESS = "android.hardware.camera.provider"
PROVIDER_START_SECONDS = 60
PROVIDER_EXIT_SECONDS = 240
COMMAND_SECONDS = 120
PREVIEW_SECONDS = 90
STILL_SECONDS = 60
NO_STILL_SECONDS = 25
SETTLE_SECONDS = 30
SCREENSHOT_SECONDS = 120
DISPLAY_WIDTH = 720
DISPLAY_HEIGHT = 1280
# The chart of the synthetic camera has colour patches and slanted edges: its luma varies a lot, where
# a black frame, or a blocked session's, varies by compression noise at most.
CHART_MINIMUM_DEVIATION = 12.0
BLACK_MAXIMUM_DEVIATION = 6.0
BLACK_MAXIMUM_MEAN = 24.0
# The preview's share of the screen: the middle, clear of the application's bars.
PREVIEW_BOX = (0.2, 0.25, 0.8, 0.6)
# The stills a layout's tier allows, in pixels (small: 12 megapixels, large: 48), and the least a still
# can be for the picture to mean anything.
STILL_MAXIMUM_PIXELS = {"small": 12_600_000, "large": 50_400_000}
STILL_MINIMUM_SIDE = 480
# A blocked session's preview is sampled twice, this far apart, so that a chart that appears late
# (a camera that opened after all) is seen.
BLOCKED_SAMPLES = 2
BLOCKED_SAMPLE_GAP_SECONDS = 10
# A blocked sample counts only while the camera application is in front; this long after it was brought back.
BRING_BACK_SETTLE_SECONDS = 8
# What is on the screen: the resumed activity, the focused window and the display's power state.
FOREGROUND_COMMAND = ("dumpsys activity activities | grep -a -E 'ResumedActivity'; "
                      "dumpsys window | grep -a -E 'mCurrentFocus|mFocusedApp'; "
                      "dumpsys power | grep -a -E 'mWakefulness='")
# The camera application's own events at a failure: its crashes, ANRs, process deaths and the windows
# (dialogs) that came in front of it.
APP_EVENTS_PATTERN = (f"{CAMERA_PACKAGE}|FATAL EXCEPTION|ANR in|am_crash|am_anr|am_proc_died|am_kill|"
                      "ActivityTaskManager|WindowManager: (Resumed|Destroying|Changing focus)")
APP_EVENTS_LINES = 80
CONCURRENT_FEATURE = "android.hardware.camera.concurrent"
# The camera application's intents: its video mode, and the extra that picks the front camera.
VIDEO_ACTION = "android.media.action.VIDEO_CAMERA"
FRONT_EXTRA = "android.intent.extra.USE_FRONT_CAMERA"
# Video: how long the shutter key's recording runs, how long the file may take to appear, the movie
# duration it must have (the recording is shorter than the hold by the application's start-up), and the
# average frame rate of its video track (the preview and the recording share one camera exposure: a rate
# near half of the camera's means they took an exposure each).
VIDEO_HOLD_SECONDS = 5
VIDEO_SECONDS = 90
VIDEO_MINIMUM_SECONDS = 1.0
VIDEO_MAXIMUM_SECONDS = 15.0
VIDEO_MINIMUM_FPS = 20.0
VIDEO_URI = "content://media/external/video/media"
# The camera HAL's own tags: a failed buffer is an error or a warning of one of these.
HAL_TAGS = ("V4l2Stream", "V4l2FrameSource", "EmulatedSensorHost")

# The cameras each synthetic profile presents, and the camera IDs Android gives them
# (android/lineage-24/overlay/camera/hal/camera_ids.hpp).
PROFILE_CAMERAS = {
    "phone": ("back+front", ("0", "1", "2", "3", "4")),
    "tablet": ("back+front", ("0", "1")),
    "laptop": ("front", ("0",)),
}
# The layout each synthetic profile earns on a host that has not learnt its lenses' colour data: the phone's
# back lenses have flash and manual control, its zoom and front cameras stream together, and its front camera
# records 2160p; the tablet's back camera has flash and manual control, and its front camera cannot make the
# guaranteed concurrent stream sizes; the laptop has one front camera (darwin/platform/synthetic_camera). The
# host offers RAW only for a lens whose colour data an earlier still has kept (darwin/device/camera/README.md,
# Calibration), so a fresh host composes these layouts and the same host after a still the -raw ones, which
# --layout states for a host that has a calibration directory.
DEFAULT_LAYOUTS = {
    "phone": "bf-large-flash-manual-concurrent-front2160",
    "tablet": "bf-small-flash-manual",
    "laptop": "front",
}
MODES = ("live", "blocked", "none")


class Failure(RuntimeError):
    pass


# ---- The plan: what a run expects, from the layout and the profile alone.

@dataclass(frozen=True)
class Plan:
    layout: object  # camera_layouts.Layout
    profile: str
    mode: str
    camera_ids: tuple[str, ...]
    features: frozenset[str]
    media_variant: str

    @property
    def still_maximum_pixels(self) -> int:
        return STILL_MAXIMUM_PIXELS[self.layout.tier or "small"]


def make_plan(layout_name: str, profile: str, mode: str) -> Plan:
    """The expectations of one run; a layout that does not describe the profile's cameras is refused,
    because the features would then promise cameras the guest lacks."""
    layouts = camera_layouts.by_name()
    if layout_name not in layouts:
        raise ValueError(f"{layout_name!r} is not a camera layout")
    if profile not in PROFILE_CAMERAS:
        raise ValueError(f"{profile!r} is not a synthetic camera profile")
    if mode not in MODES:
        raise ValueError(f"{mode!r} is not a mode")
    layout = layouts[layout_name]
    if mode == "none":
        if layout.cameras != "none":
            raise ValueError("the layout of a guest without a camera device is none")
        return Plan(layout, profile, mode, (), frozenset(), layout.media_variant)
    cameras, ids = PROFILE_CAMERAS[profile]
    if layout.cameras != cameras:
        raise ValueError(
            f"the {profile} profile presents {cameras} cameras and the layout {layout_name} describes "
            f"{layout.cameras}")
    return Plan(layout, profile, mode, ids, frozenset(layout.features), layout.media_variant)


# ---- Readers of what the guest prints. The tooling test feeds them text in the same layout.

def parse_camera_features(text: str) -> set[str]:
    """`pm list features`: the camera features."""
    return {match[1] for line in text.splitlines()
            if (match := re.fullmatch(r"feature:(android\.hardware\.camera[\w.]*)(?:=\d+)?", line.strip()))}


def parse_camera_ids(text: str) -> list[str]:
    """`dumpsys media.camera`: the IDs of the camera devices, in the order the service lists them."""
    return re.findall(r'^\s*Device \d+ maps to "([^"]+)"', text, re.MULTILINE)


def active_client_packages(text: str) -> set[str]:
    """`dumpsys media.camera`: the packages in the Active Camera Clients block. The service prints a
    list in brackets, `[]` when empty and otherwise one `(Camera ID: .., Client Package Name: .., ..)`
    line per client between a `[` line and a `]` line."""
    lines = text.splitlines()
    packages: set[str] = set()
    for index, line in enumerate(lines):
        if line.strip() != "Active Camera Clients:":
            continue
        for client in lines[index + 1:]:
            if client.strip() in ("[]", "]"):
                break
            if match := re.search(r"Client Package Name: ([\w.]+)", client):
                packages.add(match[1])
        break
    return packages


def active_client_camera_ids(text: str, package: str) -> set[str]:
    """`dumpsys media.camera`: the IDs of the cameras the package holds, from its lines of the Active
    Camera Clients block (`(Camera ID: 1, Cost: 100, PID: .., Client Package Name: .., ..)`)."""
    return set(re.findall(rf"Camera ID: (\w+),[^\n]*?Client Package Name: {re.escape(package)}\b", text))


def parse_content_rows(text: str) -> list[dict[str, str]]:
    """`content query`: one dict per `Row: N key=value, key=value` line. A value runs to the next `, key=`."""
    rows = []
    for line in text.splitlines():
        if not (head := re.match(r"Row: \d+ (.*)", line.strip())):
            continue
        rows.append({match[1]: match[2] for match in re.finditer(r"(\w+)=(.*?)(?=, \w+=|$)", head[1])})
    return rows


@dataclass
class Movie:
    seconds: float
    video: bool
    audio: bool
    frames: int = 0
    video_seconds: float = 0.0

    @property
    def fps(self) -> float:
        """The average frame rate of the video track: its frames over its duration, 0 without a sample table."""
        return self.frames / self.video_seconds if self.video_seconds > 0 else 0.0


def mp4_boxes(data: bytes, start: int, end: int):
    """The boxes of an MP4 between two offsets: (type, payload start, box end)."""
    position = start
    while position + 8 <= end:
        size, kind = struct.unpack_from(">I4s", data, position)
        header = 8
        if size == 1:
            size, header = struct.unpack_from(">Q", data, position + 8)[0], 16
        elif size == 0:
            size = end - position
        if size < header or position + size > end:
            raise ValueError(f"a box of {size} bytes at {position} does not fit")
        yield kind, position + header, position + size
        position += size


def video_samples(data: bytes, start: int, end: int) -> tuple[int, float]:
    """(frames, seconds) of the first video track of a `moov` box: the sample count and the total sample duration
    of its `stts`, in the time scale of its `mdhd`; (0, 0.0) when the track has neither."""
    for kind, payload, stop in mp4_boxes(data, start, end):
        if kind != b"trak":
            continue
        mdia = {k: (p, s) for k, p, s in mp4_boxes(data, payload, stop)}.get(b"mdia")
        if mdia is None:
            continue
        parts = {k: (p, s) for k, p, s in mp4_boxes(data, *mdia)}
        handler, header = parts.get(b"hdlr"), parts.get(b"mdhd")
        if handler is None or header is None or data[handler[0] + 8:handler[0] + 12] != b"vide":
            continue
        scale = struct.unpack_from(">I", data, header[0] + (20 if data[header[0]] == 1 else 12))[0]
        minf = {k: (p, s) for k, p, s in mp4_boxes(data, *parts[b"minf"])} if b"minf" in parts else {}
        stbl = {k: (p, s) for k, p, s in mp4_boxes(data, *minf[b"stbl"])} if b"stbl" in minf else {}
        if scale == 0 or b"stts" not in stbl:
            return 0, 0.0
        table, table_end = stbl[b"stts"]
        entries = struct.unpack_from(">I", data, table + 4)[0]
        if table + 8 + 8 * entries > table_end:
            raise ValueError("an stts table that does not fit its box")
        samples = [struct.unpack_from(">II", data, table + 8 + 8 * index) for index in range(entries)]
        return sum(count for count, _ in samples), sum(count * delta for count, delta in samples) / scale
    return 0, 0.0


def parse_mp4(data: bytes) -> Movie:
    """The duration and the track kinds of an MP4 file: its `ftyp`, and in `moov` the `mvhd` and the handlers."""
    boxes = {kind: (payload, end) for kind, payload, end in mp4_boxes(data, 0, len(data))}
    if b"ftyp" not in boxes or b"moov" not in boxes:
        raise ValueError("no ftyp and moov: not a finished movie")
    start, end = boxes[b"moov"]
    inner = {kind: (payload, stop) for kind, payload, stop in mp4_boxes(data, start, end)}
    if b"mvhd" not in inner:
        raise ValueError("no mvhd")
    payload, _ = inner[b"mvhd"]
    if data[payload] == 1:
        scale, duration = struct.unpack_from(">IQ", data, payload + 20)
    else:
        scale, duration = struct.unpack_from(">II", data, payload + 12)
    if scale == 0:
        raise ValueError("a time scale of zero")
    moov = data[start:end]
    frames, video_seconds = video_samples(data, start, end)
    return Movie(duration / scale, b"vide" in moov, b"soun" in moov, frames, video_seconds)


def hal_failures(text: str) -> list[str]:
    """`logcat -d` lines of the camera HAL's tags that say a buffer failed: an error of any of them, a request
    that was not queued completely, a buffer that never arrived or was never collected."""
    failures = []
    for line in text.splitlines():
        if not (match := re.search(r"\s([EW])\s+(\w+)\s*: (.*)$", line)):
            continue
        level, tag, message = match.groups()
        if tag not in HAL_TAGS:
            continue
        if level == "E" or any(part in message for part in (
                "was not queued completely", "did not deliver", "never collected")):
            failures.append(line.strip())
    return failures


def camera_connected(text: str, package: str) -> bool:
    """Whether `dumpsys media.camera` shows the package holding a camera: among the active clients, or
    in the service's event log with a connect as the last event of some device (the log is most recent
    first, each line indented by two spaces and starting with the time; a disconnect after the connect
    ends it)."""
    if package in active_client_packages(text):
        return True
    escaped = re.escape(package)
    latest: dict[str, tuple[str, bool]] = {}  # device -> (time of the newest event, it was a connect)
    stamp = r"(?:(\d\d-\d\d \d\d:\d\d:\d\d)\s*:\s*)?"
    event = rf"^[ \t]*{stamp}\b(CONNECT|DISCONNECT) device (\S+) client for package {escaped}\b"
    for match in re.finditer(event, text, re.MULTILINE):
        when, kind, device = match[1] or "", match[2], match[3]
        # The first line seen is the newest unless a later one carries a newer time.
        if device not in latest or when > latest[device][0]:
            latest[device] = (when, kind == "CONNECT")
    return any(connected for _, connected in latest.values())


@dataclass(frozen=True)
class StaticInfo:
    """What a camera's static characteristics say, from the metadata dump of `dumpsys media.camera`."""
    facing: str
    capabilities: frozenset[str]
    flash: bool
    af_modes: frozenset[str]


# The dump prints each camera under a `== Camera HAL device <id> (v..) static information: ==` header,
# as `name (tag): type[count]` and the values on the lines under it in brackets (enum values by name).
# The entries follow the order the HAL's metadata was built in, not the tag order, so only the header
# starts the next camera; the next `== ` header of another kind ends the camera.
# Android 17 prints the autofocus modes of the dump as numbers, the capabilities and the facing as names.
AF_MODE_NAMES = {"0": "OFF", "1": "AUTO", "2": "MACRO", "3": "CONTINUOUS_VIDEO", "4": "CONTINUOUS_PICTURE",
                 "5": "EDOF"}
STATIC_INFO_HEADER = re.compile(r"^\s*== Camera HAL device .* static information: ==\s*$")
STATIC_INFO_ENTRY = re.compile(r"^\s*(android\.[A-Za-z0-9_.]+) \(([0-9a-f]+)\): ")
STATIC_INFO_VALUES = re.compile(r"^\s*\[([^\]]*)\]\s*$")


def parse_static_info(text: str) -> list[StaticInfo]:
    """`dumpsys media.camera`: the characteristics of each camera it dumps, in the order it dumps them."""
    cameras: list[dict[str, list[str]]] = []
    current: dict[str, list[str]] | None = None
    entry: list[str] | None = None
    for line in text.splitlines():
        if line.lstrip().startswith("== "):
            current = {} if STATIC_INFO_HEADER.match(line) else None
            if current is not None:
                cameras.append(current)
            entry = None
            continue
        header = STATIC_INFO_ENTRY.match(line)
        if header and current is not None:
            entry = current.setdefault(header[1], [])
            continue
        values = STATIC_INFO_VALUES.match(line)
        if values and entry is not None:
            entry.extend(values[1].split())
        else:
            entry = None
    return [StaticInfo(
        facing=camera["android.lens.facing"][0],
        capabilities=frozenset(camera.get("android.request.availableCapabilities", ())),
        flash=camera.get("android.flash.info.available", ["FALSE"])[:1] == ["TRUE"],
        af_modes=frozenset(AF_MODE_NAMES.get(mode, mode) for mode in camera.get("android.control.afAvailableModes", ())))
        for camera in cameras if camera.get("android.lens.facing")]


def features_of(infos: list[StaticInfo]) -> set[str]:
    """The camera features the characteristics earn, by the rules of PackageManager's feature docs
    (the back camera has the flash and the autofocus; some camera has the capabilities). The concurrent
    feature is a property of combinations, not of a camera, and is not derived."""
    features: set[str] = set()
    back = [info for info in infos if info.facing == "BACK"]
    if infos:
        features.add("android.hardware.camera.any")
    if back:
        features.add("android.hardware.camera")
    if any(info.facing == "FRONT" for info in infos):
        features.add("android.hardware.camera.front")
    if any("AUTO" in info.af_modes for info in back):
        features.add("android.hardware.camera.autofocus")
    if any(info.flash for info in back):
        features.add("android.hardware.camera.flash")
    if any("MANUAL_SENSOR" in info.capabilities for info in infos):
        features.add("android.hardware.camera.capability.manual_sensor")
    if any("RAW" in info.capabilities for info in infos):
        features.add("android.hardware.camera.capability.raw")
    return features


def parse_provider_pids(text: str) -> list[int]:
    """`ps -A -o PID,NAME`: the pids of the camera provider (its binary's name holds PROVIDER_PROCESS)."""
    pids = []
    for line in text.splitlines():
        words = line.split(None, 1)
        if len(words) == 2 and words[0].isdecimal() and PROVIDER_PROCESS in words[1]:
            pids.append(int(words[0]))
    return pids


def parse_listing(text: str) -> set[str]:
    """`ls`: the names it prints, none for a directory that is not there."""
    return {line.strip() for line in text.splitlines()
            if line.strip() and "No such file" not in line and "Not a directory" not in line}


# ---- PNG (the screenshot) and JPEG (the still).

@dataclass
class Image:
    width: int
    height: int
    channels: int
    rows: list[bytes]


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def paeth(left: int, up: int, corner: int) -> int:
    estimate = left + up - corner
    distance_left, distance_up, distance_corner = abs(estimate - left), abs(estimate - up), abs(estimate - corner)
    if distance_left <= distance_up and distance_left <= distance_corner:
        return left
    return up if distance_up <= distance_corner else corner


def decode_png(data: bytes) -> Image:
    """An 8-bit grey, RGB or RGBA PNG without interlacing, which is what screencap writes."""
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError("not a PNG")
    position, header, compressed = len(PNG_SIGNATURE), None, bytearray()
    while position + 8 <= len(data):
        length, kind = struct.unpack_from(">I4s", data, position)
        body = data[position + 8:position + 8 + length]
        if len(body) != length:
            raise ValueError("truncated PNG chunk")
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
        position += 12 + length
    if header is None or not compressed:
        raise ValueError("PNG without header or data")
    width, height, depth, color, _, _, interlace = header
    channels = {0: 1, 2: 3, 6: 4}.get(color)
    if depth != 8 or channels is None or interlace != 0:
        raise ValueError(f"unsupported PNG (depth {depth}, colour type {color}, interlace {interlace})")
    raw = zlib.decompress(bytes(compressed))
    stride = width * channels
    if len(raw) != height * (stride + 1):
        raise ValueError("PNG data has the wrong size")
    rows: list[bytes] = []
    previous = bytearray(stride)
    for y in range(height):
        offset = y * (stride + 1)
        kind, line = raw[offset], bytearray(raw[offset + 1:offset + 1 + stride])
        if kind == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 255
        elif kind == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 255
        elif kind == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 255
        elif kind == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                corner = previous[i - channels] if i >= channels else 0
                line[i] = (line[i] + paeth(left, previous[i], corner)) & 255
        elif kind != 0:
            raise ValueError(f"unknown PNG filter {kind}")
        rows.append(bytes(line))
        previous = line
    return Image(width, height, channels, rows)


def luma_statistics(image: Image, box: tuple[float, float, float, float] = PREVIEW_BOX) -> tuple[float, float]:
    """The mean and the standard deviation of the luma inside a box of fractions of the image."""
    left, top = int(image.width * box[0]), int(image.height * box[1])
    right, bottom = max(left + 1, int(image.width * box[2])), max(top + 1, int(image.height * box[3]))
    count, total, squares = 0, 0.0, 0.0
    for row in image.rows[top:bottom]:
        for x in range(left, right):
            pixel = row[x * image.channels:x * image.channels + image.channels]
            if image.channels == 1:
                luma = pixel[0]
            else:
                luma = 0.299 * pixel[0] + 0.587 * pixel[1] + 0.114 * pixel[2]
            total += luma
            squares += luma * luma
            count += 1
    mean = total / count
    return mean, math.sqrt(max(0.0, squares / count - mean * mean))


@dataclass
class Jpeg:
    width: int
    height: int
    exif: bool
    complete: bool


def parse_jpeg(data: bytes) -> Jpeg:
    """The size, the Exif segment and the end marker of a JPEG file."""
    if data[:2] != b"\xff\xd8":
        raise ValueError("not a JPEG")
    position, width, height, exif = 2, 0, 0, False
    while position + 4 <= len(data):
        if data[position] != 0xFF:
            raise ValueError(f"no marker at {position}")
        marker = data[position + 1]
        if marker == 0xFF:
            position += 1
            continue
        if marker == 0xD9 or marker == 0xDA:
            break
        length = struct.unpack_from(">H", data, position + 2)[0]
        body = data[position + 4:position + 2 + length]
        if marker == 0xE1 and body.startswith(b"Exif\x00\x00"):
            exif = True
        if marker in (0xC0, 0xC1, 0xC2) and len(body) >= 5:
            height, width = struct.unpack_from(">HH", body, 1)
        position += 2 + length
    if not width or not height:
        raise ValueError("no frame header")
    return Jpeg(width, height, exif, data.rstrip(b"\x00").endswith(b"\xff\xd9"))


# ---- The guest.

class Guest:
    def __init__(self, adb: str, serial: str, process: subprocess.Popen, log: pathlib.Path,
                 private: pathlib.Path) -> None:
        self.adb = adb
        self.serial = serial
        self.process = process
        self.log = log
        self.private = private

    def adb_run(self, *arguments: str, timeout: float = COMMAND_SECONDS,
                binary: bool = False) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, "-s", self.serial, *arguments], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE if binary else subprocess.STDOUT, text=not binary, timeout=timeout,
            check=False)

    def shell(self, command: str, timeout: float = COMMAND_SECONDS) -> str:
        result = self.adb_run("shell", command, timeout=timeout)
        if result.returncode != 0:
            raise Failure(f"'{command}' exited {result.returncode}: {result.stdout.strip()[-500:]}")
        return result.stdout

    def alive(self) -> None:
        if self.process.poll() is not None:
            raise Failure(f"the guest exited with status {self.process.returncode}:\n"
                          + self.log.read_text(errors="replace")[-3000:])

    def prop(self, name: str) -> str:
        return self.shell(f"getprop {name}").strip()

    def features(self) -> set[str]:
        return parse_camera_features(self.shell("pm list features"))

    def media_camera(self) -> str:
        return self.shell("dumpsys media.camera")

    def provider_pids(self) -> list[int]:
        return parse_provider_pids(self.shell("ps -A -o PID,NAME"))

    def stills(self) -> set[str]:
        return {name for name in parse_listing(self.shell(f"ls {STILL_DIRECTORY} 2>&1 || true"))
                if name.lower().endswith((".jpg", ".jpeg"))}

    def videos(self) -> tuple[dict[str, dict[str, str]], str]:
        """The movies MediaStore lists, by their `_id`, and the raw text of the query."""
        text = self.shell(f"content query --uri {VIDEO_URI} --projection _id:_display_name:_data:duration:_size "
                          "2>&1 || true")
        return {row["_id"]: row for row in parse_content_rows(text) if "_id" in row}, text

    def hal_log(self) -> str:
        return self.shell("logcat -d -b all " + " ".join(f"{tag}:W" for tag in HAL_TAGS) + " '*:S' 2>&1 || true")

    def screenshot(self) -> Image:
        result = self.adb_run("exec-out", "screencap", "-p", timeout=SCREENSHOT_SECONDS, binary=True)
        if result.returncode != 0:
            raise Failure(f"screencap exited {result.returncode}: {result.stderr[-300:]!r}")
        try:
            return decode_png(result.stdout)
        except (ValueError, zlib.error) as problem:
            raise Failure(f"the screenshot is unreadable: {problem}") from problem

    def pull(self, remote: str) -> bytes:
        local = self.private / pathlib.PurePosixPath(remote).name
        result = self.adb_run("pull", remote, str(local), timeout=COMMAND_SECONDS)
        if result.returncode != 0 or not local.is_file():
            raise Failure(f"cannot pull {remote}: {result.stdout.strip()[-300:]}")
        return local.read_bytes()


def wait_for(description: str, seconds: float, check: Callable[[], str | None],
             guest: Guest | None = None) -> None:
    """Calls `check` until it returns None (done); its last message is the failure."""
    deadline = time.monotonic() + seconds
    message: str | None = "not checked"
    while time.monotonic() < deadline:
        if guest is not None:
            guest.alive()
        message = check()
        if message is None:
            return
        time.sleep(1.0)
    raise Failure(f"{description}: {message} (after {seconds:.0f} s)")


# ---- The checks.

def check_layout(guest: Guest, plan: Plan) -> None:
    sku = guest.prop(camera_layouts.SKU_PROPERTY)
    if sku != plan.layout.name:
        raise Failure(f"the guest booted with the layout {sku!r}, not {plan.layout.name!r}")
    variant = guest.prop(camera_layouts.MEDIA_PROPERTY)
    if variant != plan.media_variant:
        raise Failure(f"the media profiles variant is {variant!r}, not {plan.media_variant!r}")
    features = guest.features()
    if features != plan.features:
        raise Failure(f"the camera features are {sorted(features)}, not {sorted(plan.features)}: "
                      f"extra {sorted(features - plan.features)}, missing {sorted(plan.features - features)}")
    if guest.prop("ro.vendor.camera.config") != "v4l2":
        raise Failure("ro.vendor.camera.config is not v4l2: the camera HAL would list the stock cameras")


def check_camera_list(guest: Guest, plan: Plan) -> None:
    def listed() -> str | None:
        ids = parse_camera_ids(guest.media_camera())
        if sorted(ids) != sorted(plan.camera_ids):
            return f"cameras {ids}, not {list(plan.camera_ids)}"
        return None

    # The lazy provider starts when cameraserver first asks, and the cameras appear after it enumerates.
    wait_for("the cameras Android lists", SETTLE_SECONDS if plan.camera_ids else 5, listed, guest)


def wait_provider_exit(guest: Guest, when: str) -> float:
    """Waits for the lazy provider to be gone; returns how long it took. `dumpsys media.camera` can wake the provider,
    so nothing that asks cameraserver runs between the close and this wait."""
    began = time.monotonic()
    wait_for(f"the camera provider to exit {when}", PROVIDER_EXIT_SECONDS,
             lambda: None if not guest.provider_pids() else f"it still runs as {guest.provider_pids()}", guest)
    return time.monotonic() - began


def check_characteristics(guest: Guest, plan: Plan) -> None:
    """The features that the layout states are the ones the cameras' own characteristics earn."""
    infos = parse_static_info(guest.media_camera())
    if len(infos) != len(plan.camera_ids):
        raise Failure(f"dumpsys media.camera has the characteristics of {len(infos)} cameras, not "
                      f"{len(plan.camera_ids)}: the dump's format is not the one the runner reads")
    derived = features_of(infos)
    stated = set(plan.features) - {CONCURRENT_FEATURE}
    if derived != stated:
        raise Failure(f"the layout {plan.layout.name} states {sorted(stated)} but the characteristics of the "
                      f"cameras earn {sorted(derived)}: extra {sorted(stated - derived)}, "
                      f"missing {sorted(derived - stated)}")


def blocked_preview(sample: Callable[[], tuple[float, float]], sleep: Callable[[float], None]) -> str | None:
    """A blocked session's preview must not be the chart on any of BLOCKED_SAMPLES samples; the message
    of the first sample that is, else None."""
    for attempt in range(BLOCKED_SAMPLES):
        if attempt:
            sleep(BLOCKED_SAMPLE_GAP_SECONDS)
        mean, deviation = sample()
        if deviation >= CHART_MINIMUM_DEVIATION and mean > BLACK_MAXIMUM_MEAN:
            return f"the preview shows a picture (mean {mean:.1f}, deviation {deviation:.1f}, sample {attempt + 1})"
    return None


@dataclass(frozen=True)
class Foreground:
    """What the guest shows: the resumed activity ("package/.Class"), the focused window, the wakefulness."""
    resumed: str
    focus: str
    wakefulness: str

    def camera_in_front(self) -> bool:
        return self.resumed.startswith(CAMERA_PACKAGE + "/") and self.wakefulness == "Awake"

    def describe(self) -> str:
        return (f"resumed activity {self.resumed or 'none'}, focused window {self.focus or 'none'}, "
                f"wakefulness {self.wakefulness or 'unknown'}")


def parse_foreground(text: str) -> Foreground:
    """The `FOREGROUND_COMMAND` output. The top resumed activity is preferred, any resumed activity next."""
    resumed = ""
    for pattern in (r"topResumedActivity=ActivityRecord\{\S+ u\d+ (\S+)", r"ResumedActivity[=:] *ActivityRecord\{\S+ u\d+ (\S+)"):
        match = re.search(pattern, text)
        if match:
            resumed = match.group(1)
            break
    focus = re.search(r"mCurrentFocus=Window\{\S+ u\d+ ([^}]*)\}", text)
    wakefulness = re.search(r"mWakefulness=(\w+)", text)
    return Foreground(resumed, focus.group(1) if focus else "", wakefulness.group(1) if wakefulness else "")


class BlockedSampler:
    """One blocked-preview sample: taken only while the camera application is the resumed activity and the
    device is awake, checked before and after the screenshot. When it is not, the application is brought
    back once for the whole step and the sample is taken again; a second absence is a Failure of its own
    (never a pass and never a skip), naming what was in front."""

    def __init__(self, read: Callable[[], Foreground], take: Callable[[], tuple[float, float]],
                 bring_back: Callable[[], None], sleep: Callable[[float], None]) -> None:
        self.read, self.take, self.bring_back, self.sleep = read, take, bring_back, sleep
        self.brought_back = False

    def __call__(self) -> tuple[float, float]:
        while True:
            state = self.read()
            if state.camera_in_front():
                value = self.take()
                state = self.read()
                if state.camera_in_front():
                    return value
            if self.brought_back:
                raise Failure(f"a blocked session cannot be judged: the camera application is not in front "
                              f"({state.describe()}) after it was brought back once")
            print(f"  blocked: the camera application is not in front ({state.describe()}); bringing it back")
            self.brought_back = True
            self.bring_back()
            self.sleep(BRING_BACK_SETTLE_SECONDS)


def start_camera_application(guest: Guest, action: str = "android.media.action.STILL_IMAGE_CAMERA",
                             front: bool | None = None) -> None:
    """Starts the application: with its `front` extra (the application keeps its last choice otherwise)."""
    for permission in CAMERA_PERMISSIONS:
        guest.shell(f"pm grant {CAMERA_PACKAGE} {permission} 2>&1 || true")
    guest.shell("input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard 2>&1 || true")
    extra = "" if front is None else f" --ez {FRONT_EXTRA} {'true' if front else 'false'}"
    guest.shell(f"am start -a {action}{extra}")


def check_preview(guest: Guest, plan: Plan) -> None:
    """Live: the application holds a camera and its preview is the chart. Blocked: it is not."""
    start_camera_application(guest)
    if plan.mode == "live":
        wait_for("the camera application holding a camera", PREVIEW_SECONDS,
                 lambda: None if camera_connected(guest.media_camera(), CAMERA_PACKAGE)
                 else "no client of the camera service", guest)
    else:
        time.sleep(15)  # the application opens the camera or is told it cannot; either is judged below
    statistics: list[tuple[float, float]] = []

    def previewing() -> str | None:
        mean, deviation = luma_statistics(guest.screenshot())
        statistics.append((mean, deviation))
        if plan.mode == "live":
            if deviation < CHART_MINIMUM_DEVIATION:
                return f"the preview is flat (mean {mean:.1f}, deviation {deviation:.1f})"
            return None
        if deviation >= CHART_MINIMUM_DEVIATION and mean > BLACK_MAXIMUM_MEAN:
            return f"the preview shows a picture (mean {mean:.1f}, deviation {deviation:.1f})"
        return None

    if plan.mode == "live":
        wait_for("the preview showing the chart", PREVIEW_SECONDS, previewing, guest)
        print(f"  preview: mean {statistics[-1][0]:.1f}, deviation {statistics[-1][1]:.1f}")
    else:
        def take() -> tuple[float, float]:
            guest.alive()
            statistics.append(luma_statistics(guest.screenshot()))
            return statistics[-1]

        sample = BlockedSampler(lambda: parse_foreground(guest.shell(FOREGROUND_COMMAND, timeout=60)), take,
                                lambda: start_camera_application(guest), time.sleep)
        message = blocked_preview(sample, time.sleep)
        if message:
            raise Failure(f"a blocked session {message}")
        print(f"  blocked preview ({BLOCKED_SAMPLES} samples): mean {statistics[-1][0]:.1f}, "
              f"deviation {statistics[-1][1]:.1f}")


def check_still(guest: Guest, plan: Plan, before: set[str]) -> None:
    guest.shell(f"input keyevent {CAMERA_KEYCODE}")
    if plan.mode == "blocked":
        deadline = time.monotonic() + NO_STILL_SECONDS
        while time.monotonic() < deadline:
            guest.alive()
            added = guest.stills() - before
            if added:
                raise Failure(f"a blocked session took a still: {sorted(added)}")
            time.sleep(2.0)
        print("  blocked: the camera key took nothing")
        return
    found: list[str] = []

    def taken() -> str | None:
        added = sorted(guest.stills() - before)
        if not added:
            return "no new still"
        found[:] = added
        return None

    wait_for("a still in the camera folder", STILL_SECONDS, taken, guest)
    time.sleep(3)  # the file is written through; give the last write time to land
    remote = f"{STILL_DIRECTORY}/{found[-1]}"
    try:
        jpeg = parse_jpeg(guest.pull(remote))
    except ValueError as problem:
        raise Failure(f"{remote} is not a JPEG: {problem}") from problem
    if not jpeg.complete:
        raise Failure(f"{remote} has no end marker: the still was cut short")
    if not jpeg.exif:
        raise Failure(f"{remote} has no Exif data: the HAL writes it into the host's JPEG")
    if min(jpeg.width, jpeg.height) < STILL_MINIMUM_SIDE or jpeg.width * jpeg.height > plan.still_maximum_pixels:
        raise Failure(f"{remote} is {jpeg.width}x{jpeg.height}, outside the {plan.layout.tier or 'small'} "
                      f"tier's stills ({STILL_MINIMUM_SIDE} pixels a side to {plan.still_maximum_pixels} pixels)")
    print(f"  still: {found[-1]} {jpeg.width}x{jpeg.height}")


def wait_for_chart(guest: Guest, camera_id: str, what: str) -> None:
    """The application holds camera `camera_id` and its preview is the chart."""
    wait_for(f"the camera application holding camera {camera_id} ({what})", PREVIEW_SECONDS,
             lambda: None if camera_id in active_client_camera_ids(guest.media_camera(), CAMERA_PACKAGE)
             else f"it holds {sorted(active_client_camera_ids(guest.media_camera(), CAMERA_PACKAGE))}", guest)
    seen: list[tuple[float, float]] = []

    def previewing() -> str | None:
        seen.append(luma_statistics(guest.screenshot()))
        mean, deviation = seen[-1]
        return None if deviation >= CHART_MINIMUM_DEVIATION else \
            f"the preview is flat (mean {mean:.1f}, deviation {deviation:.1f})"

    wait_for(f"the {what} preview showing the chart", PREVIEW_SECONDS, previewing, guest)
    print(f"  {what} preview: mean {seen[-1][0]:.1f}, deviation {seen[-1][1]:.1f}")


def check_video(guest: Guest, plan: Plan) -> None:
    """The camera key starts and stops a recording of the application's video mode on the profile's first
    camera: MediaStore lists a new movie of at least VIDEO_MINIMUM_SECONDS with a video track."""
    before, _ = guest.videos()
    guest.shell(f"am force-stop {CAMERA_PACKAGE}")
    start_camera_application(guest, VIDEO_ACTION, front=PROFILE_CAMERAS[plan.profile][0] == "front")
    wait_for_chart(guest, plan.camera_ids[0], "video")
    guest.shell(f"input keyevent {CAMERA_KEYCODE}")
    time.sleep(VIDEO_HOLD_SECONDS)
    guest.shell(f"input keyevent {CAMERA_KEYCODE}")
    raw = ["not queried"]
    found: list[dict[str, str]] = []

    def recorded() -> str | None:
        rows, raw[0] = guest.videos()
        added = [row for _, row in sorted(rows.items()) if row["_id"] not in before]
        if not added:
            return f"MediaStore lists no new movie (query: {raw[0].strip()[-200:]!r})"
        found[:] = added
        return None

    wait_for("a movie in MediaStore", VIDEO_SECONDS, recorded, guest)
    time.sleep(3)  # the file is finished when the application has closed it; give the last write time to land
    path = found[-1].get("_data", "")
    try:
        movie = parse_mp4(guest.pull(path))
    except ValueError as problem:
        raise Failure(f"{path} is not a finished movie: {problem}") from problem
    if not movie.video:
        raise Failure(f"{path} has no video track")
    if not VIDEO_MINIMUM_SECONDS <= movie.seconds <= VIDEO_MAXIMUM_SECONDS:
        raise Failure(f"{path} lasts {movie.seconds:.2f} s; the recording ran {VIDEO_HOLD_SECONDS} s "
                      f"and must last between {VIDEO_MINIMUM_SECONDS} and {VIDEO_MAXIMUM_SECONDS} s")
    print(f"  video: {found[-1].get('_display_name', path)} {movie.seconds:.2f} s, {movie.frames} frames over "
          f"{movie.video_seconds:.2f} s = {movie.fps:.1f} fps average, audio {'yes' if movie.audio else 'no'}")
    if movie.frames == 0:
        raise Failure(f"{path} has no sample table for its video track: the frame rate cannot be told")
    if movie.fps < VIDEO_MINIMUM_FPS:
        raise Failure(f"{path} averages {movie.fps:.1f} fps ({movie.frames} frames over {movie.video_seconds:.2f} s), "
                      f"below {VIDEO_MINIMUM_FPS:.0f}: the preview and the recording do not share one exposure")


def check_front(guest: Guest, plan: Plan) -> None:
    """With a front camera (the profile's camera "1"): the application opens it, the preview is the chart and
    the camera key takes a still."""
    if PROFILE_CAMERAS[plan.profile][0] != "back+front":
        print("  front: the profile's only camera is the front one, checked above")
        return
    before = guest.stills()
    guest.shell(f"am force-stop {CAMERA_PACKAGE}")
    start_camera_application(guest, front=True)
    wait_for_chart(guest, "1", "front")
    check_still(guest, plan, before)


def check_hal_log(guest: Guest) -> None:
    failures = hal_failures(guest.hal_log())
    if failures:
        raise Failure(f"the camera HAL logged {len(failures)} failed buffer lines; the first: {failures[0]}")
    print("  the camera HAL logged no failed buffer")


def run_checks(guest: Guest, plan: Plan) -> None:
    print(f"layout {plan.layout.name}, profile {plan.profile}, {plan.mode}")
    check_layout(guest, plan)
    check_camera_list(guest, plan)
    if plan.camera_ids:
        check_characteristics(guest, plan)
    print(f"  cameras {list(plan.camera_ids)} and features {sorted(plan.features)}")
    if plan.mode == "none":
        if guest.provider_pids():
            raise Failure(f"a layout without cameras runs the camera provider: {guest.provider_pids()}")
        return
    # The provider was started with cameraserver, which listed the cameras, and holds none: it exits. The wait
    # asks cameraserver nothing (a dump can wake it), so the open below is what starts it again.
    exited = wait_provider_exit(guest, "once cameraserver has listed the cameras and none is open")
    print(f"  provider: gone {exited:.0f} s after the listing and the characteristics (host load {os.getloadavg()[0]:.1f})")
    before = guest.stills()
    check_preview(guest, plan)
    running = guest.provider_pids()
    if not running:
        raise Failure("the camera application holds a camera and no camera provider runs: the open did not start it")
    print(f"  provider: started again by the open, pid {running}")
    check_still(guest, plan, before)
    if plan.mode == "live":
        check_video(guest, plan)
        check_front(guest, plan)
        check_hal_log(guest)
    guest.shell(f"am force-stop {CAMERA_PACKAGE}")
    exited = wait_provider_exit(guest, "after the camera application is stopped")
    print(f"  provider: gone {exited:.0f} s after the application stopped")
    check_camera_list(guest, plan)
    print("  the cameras are still listed with the provider gone")
    check_cameraserver_restart(guest, plan)


def cameraserver_restart_skip(debuggable: str) -> str | None:
    """Why a restart of cameraserver cannot be run on this build, or None when it can: stopping a system
    service needs root, which only a debuggable build gives the shell."""
    if debuggable == "1":
        return None
    return "not run on a user build (stopping cameraserver needs root); the action that restarts the " \
        "provider is in the image, which the target-files verifier checks"


def check_cameraserver_restart(guest: Guest, plan: Plan) -> None:
    """cameraserver starts again with the provider gone: the action restarts the provider and the cameras are
    listed again."""
    skipped = cameraserver_restart_skip(guest.prop("ro.debuggable"))
    if skipped:
        print(f"  skip: cameraserver restarted with the provider gone: {skipped}")
        return
    guest.shell("su 0 stop cameraserver")
    wait_for("cameraserver to be stopped", COMMAND_SECONDS, lambda: None if guest.prop(
        "init.svc.cameraserver") == "stopped" else "it is not stopped", guest)
    exited = wait_provider_exit(guest, "after cameraserver stopped")
    guest.shell("su 0 start cameraserver")
    wait_for("the camera provider to be started by cameraserver's start", PROVIDER_START_SECONDS,
             lambda: None if guest.provider_pids() else "no provider process", guest)
    check_camera_list(guest, plan)
    print(f"  cameraserver restarted {exited:.0f} s after the provider was gone: the provider started again "
          "and the cameras are listed again")


# ---- Starting and stopping the guest.

CAMERA_SOURCES = {"live": "synthetic", "blocked": "blocked", "none": "none"}


def launcher_arguments(plan: Plan) -> list[str]:
    """What the plan adds to the launcher's command: the camera source and profile, and the layout,
    which the launcher forwards to the kernel as given, as the host's application states the layout it
    composed."""
    return [
        f"--camera={CAMERA_SOURCES[plan.mode]}",
        f"--camera-profile={plan.profile}",
        f"{camera_layouts.SKU_BOOT_ARGUMENT}{plan.layout.name}",
    ]


def start_guest(options: argparse.Namespace, plan: Plan, adb: str, private: pathlib.Path) -> Guest:
    instance = options.instance or private / "instance"
    boot_options = boot_android.parse_arguments([
        *(["--release", str(options.release)] if options.release else []),
        *(["--emulator", str(options.emulator)] if options.emulator else []),
        "--instance", str(instance),
        "--adb-port", str(options.adb_port),
        "--cpus", str(options.cpus),
        "--memory", options.memory,
        "--display", "offscreen",
        "--width", str(DISPLAY_WIDTH),
        "--height", str(DISPLAY_HEIGHT),
        "--audio-output", "null",
        "--engine", "gadgets",
    ])
    emulator = boot_android.resolve_executable(boot_options.emulator)
    release = boot_android.release_path(boot_options.release)
    identity = boot_android.android_instance.read_release(release)
    decision = boot_android.plan_start(boot_options, identity, instance)
    boot_android.android_instance.apply(instance, identity, decision)
    command = [
        *boot_android.emulator_command(
            boot_options, release, instance / boot_android.android_instance.DATA_DISK, emulator,
            boot_android.data_capacity(boot_options, identity)),
        *launcher_arguments(plan),
    ]
    log = private / "emulator.log"
    stream = log.open("wb")
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=stream,
                               start_new_session=True, env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
    stream.close()
    serial = f"127.0.0.1:{options.adb_port}"
    guest = Guest(adb, serial, process, log, private)
    boot_android.wait_for_adb(adb, serial, process, options.boot_timeout)
    if not boot_android.wait_for_boot_completed(adb, serial, process, options.boot_timeout):
        raise Failure("Android did not reach sys.boot_completed")
    boot_android.android_instance.booted(instance, identity)
    if not options.instance:
        boot_android.provision_guest(adb, serial)
    print(f"booted; settling for {SETTLE_SECONDS} s")
    time.sleep(SETTLE_SECONDS)
    return guest


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", type=pathlib.Path,
                        help="the macos-android17 uml-darwin-android17 (the launcher's default)")
    parser.add_argument("--release", type=pathlib.Path, help="installed Android release (default: accepted)")
    parser.add_argument("--instance", type=pathlib.Path,
                        help="an existing instance to boot; the default is a fresh one that is removed at the end")
    parser.add_argument("--camera-profile", choices=sorted(PROFILE_CAMERAS), default="phone",
                        help="the cameras the synthetic source presents")
    parser.add_argument("--layout", help="the camera layout to state on the vendor SKU "
                        "(default: the profile's on a host without RAW calibration, or none for --mode none)")
    parser.add_argument("--mode", choices=MODES, default="live",
                        help="live: the synthetic chart; blocked: a session started with Camera off; "
                        "none: no camera device")
    parser.add_argument("--adb", default="adb", help="adb client to use")
    parser.add_argument("--adb-port", type=int, default=slot_ports.slot_port(slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_ADB_PORT})")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--memory", default="4G")
    parser.add_argument("--boot-timeout", type=int, default=900,
                        help="seconds to wait for adbd and for boot_completed")
    return parser


# The tags of the camera path and of the buffers it allocates; the application's own noise (the surface
# composer's per-frame lines, adb's own polling of the camera folder) is left out.
FAILURE_LOG_PATTERN = ("V4l2Stream|V4l2FrameSource|EmulatedSensor|EmulatedRequest|CameraService|Camera2Client|Camera3|"
                       "cameraserver|camera.provider|CameraProvider|CameraX|Aperture|vrend|gralloc|minigbm|virtio|BufferQueue")
FAILURE_LOG_NOISE = "SurfaceFlinger|adbd|out of bounds transparent region"
FAILURE_LOG_LINES = 120

def print_camera_log(guest: Guest) -> None:
    """At a failure, the guest's own log lines about the camera path and its buffers, then what was in front and
    the camera application's own events (nothing is asserted on them): they tell a failure of the image from
    one of the runner."""
    try:
        text = guest.shell(f"logcat -d -b all | grep -a -E '{FAILURE_LOG_PATTERN}' | grep -a -v -E '{FAILURE_LOG_NOISE}' "
                           f"| tail -n {FAILURE_LOG_LINES}", timeout=120)
    except Exception as problem:  # the guest may be gone; the failure being reported is the one that matters
        print(f"(no camera log lines: {problem})", file=sys.stderr)
        return
    print("camera log lines of the guest:\n" + text.rstrip(), file=sys.stderr)
    try:
        foreground = guest.shell(FOREGROUND_COMMAND, timeout=60)
        print("in front at the failure:\n" + foreground.rstrip(), file=sys.stderr)
    except Exception as problem:
        print(f"(no foreground state: {problem})", file=sys.stderr)
    try:
        events = guest.shell(f"logcat -d -b all | grep -a -E '{APP_EVENTS_PATTERN}' | grep -a -v -E '{FAILURE_LOG_NOISE}' "
                             f"| tail -n {APP_EVENTS_LINES}", timeout=120)
        print("events of the camera application and its windows:\n" + events.rstrip(), file=sys.stderr)
    except Exception as problem:
        print(f"(no application events: {problem})", file=sys.stderr)


def main() -> int:
    options = build_parser().parse_args()
    if options.cpus <= 0 or options.boot_timeout <= 0:
        print("counts and timeouts must be positive", file=sys.stderr)
        return 2
    layout = options.layout or ("none" if options.mode == "none" else DEFAULT_LAYOUTS[options.camera_profile])
    try:
        plan = make_plan(layout, options.camera_profile, options.mode)
    except ValueError as problem:
        print(f"{problem}", file=sys.stderr)
        return 2
    adb = boot_android.resolve_executable(options.adb, sdk_fallback=True)
    # Short, so every socket path below it stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-camera-"))
    guest: Guest | None = None
    try:
        guest = start_guest(options, plan, adb, private)
        run_checks(guest, plan)
        print("PASS")
        return 0
    except (Failure, boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        if guest is not None and not isinstance(problem, boot_android.LauncherError):
            print_camera_log(guest)
        log = private / "emulator.log"
        if log.is_file():
            print("launcher log tail:\n" + "\n".join(log.read_text(errors="replace").splitlines()[-30:]),
                  file=sys.stderr)
        return 1
    finally:
        if guest is not None:
            boot_android.disconnect_adb(adb, guest.serial)
            boot_android.stop_guest(guest.process)
        shutil.rmtree(private, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
