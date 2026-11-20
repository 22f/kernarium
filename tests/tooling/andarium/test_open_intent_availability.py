#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shortcuts offers "Open Android App" from iOS 27 only.

Before iOS 27 an app of Andarium's is no entity (`IPadAppHost.offersAddToHomeScreen`),
so the action would ask for an app that no query can give. The declarations
say where the action is offered, and the App Intents metadata the build
extracts says it to the system: the action and the automatic shortcut carry an
`LNPlatformNameIOS` introduced version of 27.0 beside the wildcard that keeps
them on the other platforms (a Mac and a Vision Pro of iOS 27 run the action by
name). This reads both: the sources for the two declarations' availability, and
the metadata of a built application when `--bundle` names one.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
from pathlib import Path
import re
import sys
import unittest

SOURCE = "Andarium/Andarium/Shared/AndroidAppIntents.swift"
DECLARATIONS = ("struct OpenAndroidAppIntent", "struct AndroidAppShortcuts")
ACTION = "OpenAndroidAppIntent"
FIRST_VERSION = (27, 0)
WILDCARD = "LNPlatformNameWildcard"
IOS = "LNPlatformNameIOS"


def version(text: str) -> tuple[int, ...]:
    return tuple(int(part) for part in text.split("."))


def availability_problem(annotations: dict | None) -> str | None:
    """Why `annotations` leaves the entry on iOS before 27, or None."""
    if not annotations:
        return "has no availability annotations"
    ios = annotations.get(IOS)
    if ios is None:
        return "names no iOS version, so every iOS offers it"
    introduced = ios.get("introducedVersion", "")
    if not re.fullmatch(r"\d+(\.\d+)*", introduced) or version(introduced) < FIRST_VERSION:
        return f"introduces it on iOS {introduced!r}, before 27.0"
    if WILDCARD not in annotations:
        return "drops the wildcard, which keeps it on the other platforms"
    return None


def metadata_problems(actions_data: dict) -> list[str]:
    problems = []
    action = actions_data.get("actions", {}).get(ACTION)
    if action is None:
        problems.append(f"the metadata has no {ACTION}")
    else:
        found = availability_problem(action.get("availabilityAnnotations"))
        if found:
            problems.append(f"action {ACTION} {found}")
    shortcuts = [
        entry for entry in actions_data.get("autoShortcuts", [])
        if entry.get("actionIdentifier") == ACTION
    ]
    if not shortcuts:
        problems.append(f"the metadata has no automatic shortcut for {ACTION}")
    for entry in shortcuts:
        found = availability_problem(entry.get("availabilityAnnotations"))
        if found:
            problems.append(f"the automatic shortcut for {ACTION} {found}")
    return problems


def source_problems(text: str) -> list[str]:
    problems = []
    for declaration in DECLARATIONS:
        match = re.search(rf"^((?:@\w[^\n]*\n|///[^\n]*\n)*)(?:nonisolated )?{declaration}\b", text, re.M)
        if match is None:
            problems.append(f"{SOURCE} has no {declaration}")
        elif "@available(iOS 27.0, *)" not in match.group(1):
            problems.append(f"{declaration} is not @available(iOS 27.0, *)")
    return problems


# What the build extracted before the declarations were gated, and after.
WILDCARD_ONLY = {WILDCARD: {"introducedVersion": "*"}}
FROM_27 = {IOS: {"introducedVersion": "27.0"}, WILDCARD: {"introducedVersion": "*"}}


def extraction(annotations: dict) -> dict:
    return {
        "actions": {ACTION: {"availabilityAnnotations": annotations}},
        "autoShortcuts": [{"actionIdentifier": ACTION, "availabilityAnnotations": annotations}],
    }


class Fixtures(unittest.TestCase):
    def test_a_wildcard_only_declaration_is_offered_on_ios_26(self) -> None:
        problems = metadata_problems(extraction(WILDCARD_ONLY))
        self.assertEqual(len(problems), 2, problems)
        self.assertTrue(all("names no iOS version" in problem for problem in problems), problems)

    def test_an_earlier_ios_version_is_offered_on_ios_26(self) -> None:
        early = {IOS: {"introducedVersion": "26.0"}, WILDCARD: {"introducedVersion": "*"}}
        self.assertEqual(len(metadata_problems(extraction(early))), 2)

    def test_the_action_and_the_shortcut_are_judged_each(self) -> None:
        data = extraction(FROM_27)
        data["autoShortcuts"][0]["availabilityAnnotations"] = WILDCARD_ONLY
        problems = metadata_problems(data)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("automatic shortcut", problems[0])

    def test_ios_27_and_the_wildcard_are_what_the_build_should_extract(self) -> None:
        self.assertEqual(metadata_problems(extraction(FROM_27)), [])

    def test_dropping_the_wildcard_takes_the_other_platforms_off(self) -> None:
        only_ios = {IOS: {"introducedVersion": "27.0"}}
        self.assertEqual(len(metadata_problems(extraction(only_ios))), 2)

    def test_a_missing_action_or_shortcut_is_reported(self) -> None:
        self.assertEqual(len(metadata_problems({})), 2)

    def test_the_declarations_are_checked_in_the_source(self) -> None:
        gated = (
            "/// Opens.\n@available(iOS 27.0, *)\nstruct OpenAndroidAppIntent: OpenIntent {}\n"
            "/// Tiles.\n@available(iOS 27.0, *)\nnonisolated struct AndroidAppShortcuts: AppShortcutsProvider {}\n"
        )
        self.assertEqual(source_problems(gated), [])
        loose = gated.replace("@available(iOS 27.0, *)\nstruct", "struct")
        self.assertEqual(len(source_problems(loose)), 1)
        self.assertEqual(len(source_problems("")), 2)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument(
        "--bundle", type=Path,
        help="a built Andarium.app: its extracted App Intents metadata is checked too")
    args, rest = parser.parse_known_args()
    checked = []
    checked += source_problems((args.root / SOURCE).read_text(encoding="utf-8"))
    if args.bundle is not None:
        path = args.bundle / "Metadata.appintents" / "extract.actionsdata"
        checked += metadata_problems(json.loads(path.read_text(encoding="utf-8")))
    if checked:
        sys.exit("test_open_intent_availability.py: " + "; ".join(checked))
    result = unittest.main(argv=[sys.argv[0], *rest], exit=False)
    return 0 if result.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
