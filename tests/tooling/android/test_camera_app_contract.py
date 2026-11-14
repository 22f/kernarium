#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The camera application's product settings, the patch that defines them and the verifier agree.

The product overlay of the camera application (Aperture) replaces resources the application's own
patch series defines: the switch for free zoom (default on, off in the overlay) and the two settings
that list each lens of the host. A name that one side renames leaves the overlay applying to nothing,
and the verifier's constants must name the same settings and values as the overlay.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import re
import shutil
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

OVERLAY_CONFIG = Path("overlay/overlay/packages/apps/Aperture/app/src/main/res/values/config.xml")
SERIES_DIRECTORY = Path("patches/projects/p0031/series")
VERIFIER = Path("tools/verify-target-files.py")
ZOOM = "config_enableZoom"
AUX_CAMERAS = "config_enableAuxCameras"
IGNORED_AUX_CAMERAS = "config_ignoredAuxCameraIds"
MAIN_CAMERAS = "LensZoomRatioUtils.getMainCameras("


def fail(message: str) -> None:
    raise SystemExit(f"android camera application contract fixture: {message}")


def expect_failure(function, label: str) -> None:
    try:
        function()
    except SystemExit:
        return
    fail(f"the check accepted {label}")


def overlay_values(config: Path) -> tuple[dict[str, str], dict[str, list[str]]]:
    booleans: dict[str, str] = {}
    arrays: dict[str, list[str]] = {}
    for element in ET.parse(config).getroot():
        name = element.get("name")
        if element.tag == "bool":
            booleans[name] = (element.text or "").strip()
        elif element.tag == "string-array":
            arrays[name] = [(item.text or "").strip() for item in element.findall("item")]
    return booleans, arrays


def series_patches(series_directory: Path) -> str:
    """The text of every patch of the series, in the order series.tsv lists them."""
    names = [
        line.split("\t", 1)[1]
        for line in (series_directory / "series.tsv").read_text(encoding="utf-8").splitlines()
        if line.startswith("patch\t")
    ]
    if not names:
        fail(f"{series_directory} lists no patch")
    return "\n".join((series_directory / name).read_text(encoding="utf-8") for name in names)


def load_verifier(path: Path):
    specification = importlib.util.spec_from_file_location("verify_target_files", path)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def check(android_root: Path) -> None:
    booleans, arrays = overlay_values(android_root / OVERLAY_CONFIG)
    if booleans.get(ZOOM) != "false":
        fail(f"the overlay must set {ZOOM} to false: {booleans.get(ZOOM)!r}")
    if booleans.get(AUX_CAMERAS) != "true":
        fail(f"the overlay must set {AUX_CAMERAS} to true: {booleans.get(AUX_CAMERAS)!r}")
    if arrays.get(IGNORED_AUX_CAMERAS) != ["2"]:
        fail(f"the overlay must ignore camera 2 and no other: {arrays.get(IGNORED_AUX_CAMERAS)!r}")

    verifier = load_verifier(android_root / VERIFIER)
    for name, value in (("CAMERA_ZOOM", ZOOM), ("CAMERA_AUX_CAMERAS", AUX_CAMERAS),
                        ("CAMERA_IGNORED_AUX_CAMERAS", IGNORED_AUX_CAMERAS)):
        if getattr(verifier, name, None) != value:
            fail(f"the verifier's {name} is not {value}")
    if verifier.CAMERA_IGNORED_AUX_IDS != arrays[IGNORED_AUX_CAMERAS]:
        fail("the verifier and the overlay ignore different camera IDs")

    # The application's own patch defines the switch, on by default, and reads it through the
    # overlay repository, which is what lets a product's overlay turn it off.
    patches = series_patches(android_root / SERIES_DIRECTORY)
    added = [line[1:].strip() for line in patches.splitlines() if line.startswith("+")]
    if f'<bool name="{ZOOM}">true</bool>' not in added:
        fail(f"the camera application's patches do not define {ZOOM} as true by default")
    for needle in (f"getBoolean(R.bool.{ZOOM})", "isPinchToZoomEnabled = overlaysRepository.enableZoom"):
        if not any(needle in line for line in added):
            fail(f"the camera application's patches never use the switch: no line adds {needle}")
    # The lens buttons' labels come from one rule, snapped to the labels of a phone's lens buttons.
    labels = re.search(r"floatArrayOf\(([^)]*)\)", patches)
    if labels is None or [item.strip() for item in labels.group(1).split(",")] != [
            "0.5f", "1f", "2f", "2.5f", "3f", "4f", "5f"]:
        fail("the camera application's patches do not snap the lens labels to 0.5, 1, 2, 2.5, 3, 4 and 5")
    # The labels of a side are relative to its main camera, found for the internal and the external
    # cameras alike by the same grouping.
    if not any(MAIN_CAMERAS in line for line in added):
        fail(f"the camera application's patches never group the cameras of a side: no line adds {MAIN_CAMERAS}")


def fixtures(android_root: Path) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        copy = Path(temporary) / "lineage-24"
        for relative in (OVERLAY_CONFIG, SERIES_DIRECTORY, VERIFIER):
            source = android_root / relative
            target = copy / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if source.is_dir():
                shutil.copytree(source, target)
            else:
                shutil.copy2(source, target)
        config = copy / OVERLAY_CONFIG
        good = config.read_text(encoding="utf-8")
        check(copy)

        for label, old, new in (
            ("free zoom left on", f'<bool name="{ZOOM}">false</bool>', f'<bool name="{ZOOM}">true</bool>'),
            ("no zoom switch", f'    <bool name="{ZOOM}">false</bool>\n', ""),
            ("a zoom switch under another name", f'name="{ZOOM}"', 'name="config_zoomEnabled"'),
            ("no ignored camera", "        <item>2</item>\n", ""),
            ("aux cameras off", f'<bool name="{AUX_CAMERAS}">true</bool>', f'<bool name="{AUX_CAMERAS}">false</bool>'),
        ):
            if old not in good:
                fail(f"the fixture for {label} found nothing to change in the overlay")
            config.write_text(good.replace(old, new), encoding="utf-8")
            expect_failure(lambda: check(copy), label)
        config.write_text(good, encoding="utf-8")

        for patch in sorted((copy / SERIES_DIRECTORY).glob("*.patch")):
            text = patch.read_text(encoding="utf-8")
            if f'<bool name="{ZOOM}">true</bool>' not in text:
                continue
            patch.write_text(text.replace(f'<bool name="{ZOOM}">true</bool>',
                                          f'<bool name="{ZOOM}">false</bool>'), encoding="utf-8")
            expect_failure(lambda: check(copy), "a patch that turns free zoom off by default")
            patch.write_text(text, encoding="utf-8")
            if MAIN_CAMERAS in text:
                patch.write_text(text.replace(MAIN_CAMERAS, "mainCamerasOf("), encoding="utf-8")
                expect_failure(lambda: check(copy), "a patch that does not group the cameras of a side")
                patch.write_text(text, encoding="utf-8")
        check(copy)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    android_root = args.source_root.resolve() / "android/lineage-24"
    check(android_root)
    fixtures(android_root)
    print("android camera application contract passed")


if __name__ == "__main__":
    main()
