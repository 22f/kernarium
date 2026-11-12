#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Copy the developer's artwork into the application, where the tip sheet has it.

The Buy me tokens sheet's header shows its developer: an avatar and the GitHub
and X marks (Andarium/README.md#buy-me-tokens). The files are local, never
tracked (`Config/Local/`, named by `Config/Developer.local.xcconfig.example`),
so this build phase puts them in the product, in a `DeveloperArtwork` folder of
its resources, only where the feature is: the build names the tip identifiers
(TIP_JAR_NAMED), and the configuration is AppStore (the files are then
`Config/Local/`'s own) or the tests' arrangement states a developer name (the
files are then the drawn stand-ins of `Config/DeveloperExample/`). The avatar is
copied when its file exists; a mark when its handle is set and its file exists.
Any other build ends with no folder at all.

Run by Xcode with its build settings in the environment.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import os
import shutil
import sys
from pathlib import Path

# The file each piece comes from, and the name it has in the product.
PIECES = (
    ("developer-avatar.jpeg", "avatar", None),
    ("github-logo.svg", "github", "SHEET_DEVELOPER_GITHUB"),
    ("x-logo.svg", "x", "SHEET_DEVELOPER_X"),
)


def main() -> int:
    environment = os.environ
    try:
        root = Path(environment["SRCROOT"])
        product = Path(environment["TARGET_BUILD_DIR"]) / environment["UNLOCALIZED_RESOURCES_FOLDER_PATH"]
    except KeyError as missing:
        print(f"copy-developer-artwork.py: no {missing.args[0]} in the environment", file=sys.stderr)
        return 1
    destination = product / "DeveloperArtwork"
    shutil.rmtree(destination, ignore_errors=True)
    if not environment.get("TIP_JAR_NAMED"):
        return 0
    if environment.get("CONFIGURATION") == "AppStore":
        source = root / "Config" / "Local"
    elif environment.get("SHEET_DEVELOPER_NAME"):
        source = root / "Config" / "DeveloperExample"
    else:
        return 0
    copied = []
    for file_name, name, handle in PIECES:
        path = source / file_name
        if not path.is_file() or (handle is not None and not environment.get(handle)):
            continue
        destination.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, destination / name)
        copied.append(name)
    print(f"Developer artwork: {', '.join(copied) if copied else 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
