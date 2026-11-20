#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a built Andarium application that is about to be archived.

An archive of the Release configuration (Andarium/README.md#configurations) is
the build that is handed out. The build phase "Check embedded artifacts" runs
this script beside scripts/check-embedded-artifacts.py when the action is an
archive, and no other build is checked. The application must:

  - link to the developer's profile on X (Follow developer on X) only with a
    handle of the developer's own: the build writes the handle to Info.plist
    under AndariumFollowXHandle where the ignored local configuration names
    one. No key, or an empty one, is no link. A handle that is not a handle of
    X (1 to 15 letters, digits and underscores), and the placeholder that
    Config/Developer.local.xcconfig.example names, in any letter case, are
    refused: a copy of the example that nobody edited would otherwise ship a
    link to it.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import plistlib
import re
import sys
from pathlib import Path

# The Info.plist key the build writes the handle to
# (Andarium/Config/Andarium.xcconfig).
FOLLOW_X_KEY = "AndariumFollowXHandle"
# The shape of a handle of X, the same rules as FollowXProfile.isWellFormed.
FOLLOW_X_HANDLE = re.compile(r"[A-Za-z0-9_]{1,15}")
# What Config/Developer.local.xcconfig.example names, and the handle the tests
# and the screen captures state: no archive links to it.
FOLLOW_X_EXAMPLE = "example_handle"


def emit(text: str, error: bool = False) -> None:
    print(text, file=sys.stderr if error else sys.stdout, flush=True)


def follow_x_problems(info: dict) -> list[str]:
    """What is wrong with the profile link the build names; nothing for no link."""
    if FOLLOW_X_KEY not in info:
        return []
    handle = info[FOLLOW_X_KEY]
    if handle == "":
        return []
    if not isinstance(handle, str) or not FOLLOW_X_HANDLE.fullmatch(handle):
        shown = repr(handle)[:40]
        return [
            f"Info.plist: {FOLLOW_X_KEY} is {shown}, not a handle of X "
            "(1 to 15 letters, digits and underscores)"
        ]
    if handle.lower() == FOLLOW_X_EXAMPLE:
        return [
            f"Info.plist: {FOLLOW_X_KEY} is the example handle of "
            "Developer.local.xcconfig.example; name the developer's own, or none"
        ]
    return []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path, help="the built application bundle")
    options = parser.parse_args()
    app = options.app

    try:
        with (app / "Info.plist").open("rb") as handle:
            info = plistlib.load(handle)
    except (OSError, plistlib.InvalidFileException, ValueError):
        emit(f"{app}: has no readable Info.plist", error=True)
        return 1

    problems = [f"{app.name}: {problem}" for problem in follow_x_problems(info)]
    for problem in problems:
        emit(problem, error=True)
    if problems:
        return 1
    emit(f"{app.name}: ready to archive")
    return 0


if __name__ == "__main__":
    sys.exit(main())
