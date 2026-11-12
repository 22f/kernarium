#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check which developer artwork the build phase copies into the application.

The tip sheet's header shows an avatar and two profile marks. They are local
files, so the phase copies them only where the build has Buy me tokens, and
only the pieces whose value is set: the App Store configuration takes the
maker's own folder, the tests' arrangement the drawn stand-ins, any other
build none. Every file and value here is made up.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

FILES = {"developer-avatar.jpeg": "avatar", "github-logo.svg": "github", "x-logo.svg": "x"}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", type=Path, required=True)
    options = parser.parse_args()
    failures: list[str] = []

    with tempfile.TemporaryDirectory(prefix="andarium-developer-artwork-") as directory:
        root = Path(directory)
        source = root / "source"
        for folder, prefix in (("Local", b"local "), ("DeveloperExample", b"example ")):
            (source / "Config" / folder).mkdir(parents=True)
            for name in FILES:
                (source / "Config" / folder / name).write_bytes(prefix + name.encode())

        def run(label: str, expected: dict[str, str] | None, *, remove: tuple[str, ...] = (), stale: bool = False,
                **environment: str) -> None:
            """`expected` maps a piece to the folder it must come from, or is None for no folder."""
            product = root / label
            product.mkdir()
            if stale:
                (product / "DeveloperArtwork").mkdir()
                (product / "DeveloperArtwork" / "old").write_bytes(b"stale")
            for folder in ("Local", "DeveloperExample"):
                for name in remove:
                    (source / "Config" / folder / name).rename(source / "Config" / folder / (name + ".held"))
            try:
                result = subprocess.run(
                    [sys.executable, "-B", str(options.script)], check=False, stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60,
                    env={**os.environ, "SRCROOT": str(source), "TARGET_BUILD_DIR": str(product),
                         "UNLOCALIZED_RESOURCES_FOLDER_PATH": ".", **environment})
            finally:
                for folder in ("Local", "DeveloperExample"):
                    for name in remove:
                        (source / "Config" / folder / (name + ".held")).rename(source / "Config" / folder / name)
            folder = product / "DeveloperArtwork"
            if result.returncode != 0:
                failures.append(f"{label}: the phase failed:\n{result.stdout}")
            elif expected is None:
                if folder.exists():
                    failures.append(f"{label}: a folder where there should be none: {sorted(p.name for p in folder.iterdir())}")
            else:
                found = {path.name: path.read_bytes() for path in folder.iterdir()} if folder.is_dir() else {}
                if set(found) != set(expected):
                    failures.append(f"{label}: has {sorted(found)}, wanted {sorted(expected)}")
                for piece, from_folder in expected.items():
                    name = next(name for name, value in FILES.items() if value == piece)
                    if found.get(piece) != (b"local " if from_folder == "Local" else b"example ") + name.encode():
                        failures.append(f"{label}: {piece} is not the file of {from_folder}")

        tip = {"TIP_JAR_NAMED": "named"}
        everything = {"SHEET_DEVELOPER_NAME": "n", "SHEET_DEVELOPER_GITHUB": "g", "SHEET_DEVELOPER_X": "x"}
        # The App Store configuration with the feature takes the maker's folder, each piece by its value.
        run("appstore-all", {"avatar": "Local", "github": "Local", "x": "Local"}, CONFIGURATION="AppStore", **tip, **everything)
        run("appstore-no-values", {"avatar": "Local"}, CONFIGURATION="AppStore", **tip)
        run("appstore-github", {"avatar": "Local", "github": "Local"}, CONFIGURATION="AppStore", **tip,
            SHEET_DEVELOPER_GITHUB="g")
        run("appstore-x-only", {"avatar": "Local", "x": "Local"}, CONFIGURATION="AppStore", **tip,
            SHEET_DEVELOPER_X="x")
        # A file the maker does not have is skipped.
        run("appstore-no-avatar", {"github": "Local", "x": "Local"}, remove=("developer-avatar.jpeg",),
            CONFIGURATION="AppStore", **tip, **everything)
        run("appstore-no-files", {}, remove=tuple(FILES), CONFIGURATION="AppStore", **tip, **everything)
        # Without the feature, nothing, and a folder of an earlier build is removed.
        run("appstore-no-tip", None, CONFIGURATION="AppStore", **everything)
        run("appstore-no-tip-stale", None, stale=True, CONFIGURATION="AppStore", **everything)
        # The arrangement states a name and takes the drawn stand-ins; no name, no folder.
        run("debug-arranged", {"avatar": "DeveloperExample", "github": "DeveloperExample", "x": "DeveloperExample"},
            CONFIGURATION="Debug", **tip, **everything)
        run("debug-name-only", {"avatar": "DeveloperExample"}, CONFIGURATION="Debug", **tip,
            SHEET_DEVELOPER_NAME="n")
        run("debug-tip-only", None, CONFIGURATION="Debug", **tip)
        run("debug-no-tip", None, CONFIGURATION="Debug", **everything)
        run("release-tip-stale", None, stale=True, CONFIGURATION="Release", **tip)
        # A phase without its environment fails rather than guessing.
        result = subprocess.run(
            [sys.executable, "-B", str(options.script)], check=False, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60,
            env={key: value for key, value in os.environ.items()
                 if key not in ("SRCROOT", "TARGET_BUILD_DIR", "UNLOCALIZED_RESOURCES_FOLDER_PATH")})
        if result.returncode == 0:
            failures.append("a phase without its environment passed")

    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
