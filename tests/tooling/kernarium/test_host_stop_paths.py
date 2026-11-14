#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Every host-side end of a running Kernarium guest goes through one stop.

The application ends a running guest in two ways, the screen closing and Shut
down over a guest paused for space, and both come to
``EmulatorLauncher.stop()``, which has the guest write its storage back before
the host stops it (Kernarium/README.md, "Lifecycle"). This pins the call
sites: the runtime's stop is asked for by the delivery alone, the delivery by
the launcher alone, and the launcher's stop by those two callers. A new way to
end a guest, or a second path to the runtime's stop, fails here and has to
say what it does with the guest's storage.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import pathlib
import re
import unittest

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
SOURCES = REPOSITORY / "Kernarium" / "Kernarium"


def code(path: pathlib.Path) -> str:
    """The Swift source with its line comments removed."""
    return re.sub(r"//[^\n]*", "", path.read_text(encoding="utf-8"))


def files_matching(pattern: str) -> dict[str, int]:
    found: dict[str, int] = {}
    for path in sorted(SOURCES.glob("*.swift")):
        count = len(re.findall(pattern, code(path)))
        if count:
            found[path.name] = count
    return found


class HostStopPathsTests(unittest.TestCase):
    def test_the_sources_are_where_the_test_looks(self) -> None:
        self.assertTrue((SOURCES / "ContentView.swift").is_file())
        self.assertTrue((SOURCES / "GuestPersistence.swift").is_file())

    def test_the_runtimes_stop_is_asked_for_by_the_delivery_alone(self) -> None:
        self.assertEqual(
            files_matching(r"kernarium_runtime_loader_request_stop\b"),
            {"GuestPersistence.swift": 1},
        )

    def test_the_delivery_is_made_by_the_launcher_alone(self) -> None:
        self.assertEqual(
            files_matching(r"\bGuestPersistence\.shared\.deliverStop\("),
            {"ContentView.swift": 1},
        )

    def test_the_launchers_stop_has_two_callers(self) -> None:
        # The screen closing (ContentView.onDisappear) and Shut down over a
        # guest paused for space (GuestSpacePause.shutDown, through SpaceRun).
        self.assertEqual(
            files_matching(r"\b(?:EmulatorLauncher\.shared|launcher|run)\.stop\(\)"),
            {"ContentView.swift": 1, "GuestSpacePause.swift": 1},
        )

    def test_nothing_else_ends_the_process_or_the_guest(self) -> None:
        self.assertEqual(
            files_matching(r"\b(?:_?exit|abort|raise|kill|terminate)\("), {}
        )


if __name__ == "__main__":
    unittest.main()
