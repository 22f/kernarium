#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that the App Store configuration keeps the document pickers in place.

Andarium's two document pickers (Install APK in Shared/Settings.swift and the
file browser's Import in Shared/FileBrowserBar.swift) open the files in place
with SwiftUI's fileImporter in the App Store configuration, and present the
copy-mode picker (Shared/CopyDocumentPicker.swift) in every other
(Andarium/README.md#configurations). This reads the sources as the compiler
does with APP_STORE set and not set, and fails when the condition is taken
away, turned round or put anywhere else. The bundle check
(Andarium/scripts/check-app-store-bundle.py) says the same of a built
application; its names for the presenter are held to the sources here.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import re
import sys
from pathlib import Path

SHARED = Path("Andarium/Andarium/Shared")
CALL_SITES = ("Settings.swift", "FileBrowserBar.swift")
PRESENTER = "CopyDocumentPicker.swift"
# What only the copy-mode presenter may mention, in the App Store view of any
# file of the application.
PRESENTER_NAMES = ("CopyDocumentPicker", "documentCopyPicker", "PickedCopies", "asCopy: true")

CONDITION = re.compile(r"^\s*#(if|elseif|else|endif)\b(.*)$")


def active(text: str, app_store: bool) -> list[str]:
    """The lines the compiler sees when APP_STORE is `app_store`. A condition
    on anything else (FEEDBACK_MAIL, FOLLOW_X) is taken as holding, so that
    code under it is judged by APP_STORE alone."""
    stack: list[tuple[bool, bool]] = []  # (this branch is on, a branch of this #if was on)
    lines = []
    for line in text.splitlines():
        match = CONDITION.match(line)
        if match is None:
            if all(on for on, _taken in stack):
                lines.append(line)
            continue
        word, rest = match.group(1), match.group(2).split("//")[0].strip()
        if word == "if":
            on = evaluate(rest, app_store)
            stack.append((on, on))
        elif word == "elseif":
            _on, taken = stack.pop()
            on = not taken and evaluate(rest, app_store)
            stack.append((on, taken or on))
        elif word == "else":
            _on, taken = stack.pop()
            stack.append((not taken, True))
        else:
            stack.pop()
    if stack:
        raise ValueError("an #if without its #endif")
    return lines


def evaluate(condition: str, app_store: bool) -> bool:
    if condition == "APP_STORE":
        return app_store
    if condition == "!APP_STORE":
        return not app_store
    return True


def code(lines: list[str]) -> list[str]:
    """The lines that are code: no blank line and no comment."""
    return [line for line in lines if line.strip() and not line.strip().startswith("//")]


def count(lines: list[str], needle: str) -> int:
    return sum(line.count(needle) for line in code(lines))


def problems(call_sites: dict[str, str], presenter: str, others: dict[str, str]) -> list[str]:
    """What is wrong with the pickers of these sources: `call_sites` are the two
    files that present one, `presenter` is the copy-mode file and `others` the
    other files of the application."""
    found = []
    for name, text in call_sites.items():
        in_store, elsewhere = active(text, True), active(text, False)
        if count(in_store, ".fileImporter(") != 1:
            found.append(f"{name}: the App Store configuration must open the files in place, "
                         f"with one fileImporter (found {count(in_store, '.fileImporter(')})")
        if count(elsewhere, ".fileImporter(") != 0:
            found.append(f"{name}: a configuration other than the App Store's holds a fileImporter")
        if count(elsewhere, ".documentCopyPicker(") != 1:
            found.append(f"{name}: every other configuration must present the copy-mode picker, once "
                         f"(found {count(elsewhere, '.documentCopyPicker(')})")
        for needle in PRESENTER_NAMES:
            if count(in_store, needle):
                found.append(f"{name}: the App Store configuration mentions {needle}")
    if code(active(presenter, True)):
        found.append(f"{PRESENTER}: the copy-mode presenter has code in the App Store configuration")
    if not code(active(presenter, False)):
        found.append(f"{PRESENTER}: the copy-mode presenter is empty in the other configurations")
    for name, text in others.items():
        for needle in PRESENTER_NAMES:
            if count(active(text, True), needle):
                found.append(f"{name}: the App Store configuration mentions {needle}")
    return found


def load_checker(path: Path):
    spec = importlib.util.spec_from_file_location("check_app_store_bundle", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fragment_problems(checker, presenter: str) -> list[str]:
    """The bundle check looks for the presenter by mangled names: each must be a
    name of the presenter's file as Swift writes it, its length first."""
    found = []
    for fragment in checker.PICKER_COPY:
        match = re.fullmatch(rb"(\d+)([A-Za-z_][A-Za-z0-9_]*)", fragment)
        if match is None or int(match.group(1)) != len(match.group(2)):
            found.append(f"the bundle check's fragment {fragment!r} is not a Swift name with its length")
        elif match.group(2).decode() not in presenter:
            found.append(f"the bundle check looks for {match.group(2).decode()}, which the presenter does not define")
    for name, fragment in checker.PICKER_IN_PLACE.items():
        if b"12fileImporter11isPresented19allowedContentTypes" not in fragment:
            found.append(f"the bundle check's fragment for {name} is not SwiftUI's fileImporter")
    return found


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True, help="the repository")
    options = parser.parse_args()
    root: Path = options.root

    shared = root / SHARED
    call_sites = {name: (shared / name).read_text(encoding="utf-8") for name in CALL_SITES}
    presenter = (shared / PRESENTER).read_text(encoding="utf-8")
    others = {
        str(path.relative_to(root)): path.read_text(encoding="utf-8")
        for path in sorted((root / "Andarium/Andarium").rglob("*.swift"))
        if path.name not in (*CALL_SITES, PRESENTER) and path.relative_to(root / "Andarium/Andarium").parts[0] != "Preview"
    }
    failures = [f"the sources: {line}" for line in problems(call_sites, presenter, others)]
    checker = load_checker(root / "Andarium/scripts/check-app-store-bundle.py")
    failures.extend(fragment_problems(checker, presenter))

    # The check finds what a later edit could do to the condition: each change
    # below must fail it.
    stray = "        .fileImporter(isPresented: .constant(false), allowedContentTypes: [.item]) { _ in }\n"
    for site, text in call_sites.items():
        mutations = {
            "the condition taken away": text.replace("#if APP_STORE", "#if true", 1),
            "the branches turned round": text.replace("#if APP_STORE", "#if !APP_STORE", 1),
            "an importer in every build": text.replace("#if APP_STORE", stray + "#if APP_STORE", 1),
        }
        for label, mutated in mutations.items():
            if mutated == text:
                failures.append(f"{site}: the check's change '{label}' changed nothing")
            elif not problems({**call_sites, site: mutated}, presenter, others):
                failures.append(f"{site}: {label} passed the check")
    # A presenter outside its condition, and a mention of it elsewhere.
    if not problems(call_sites, presenter.replace("#if !APP_STORE", "#if true", 1), others):
        failures.append(f"{PRESENTER}: a presenter outside the condition passed the check")
    if not problems(call_sites, presenter, {**others, "Elsewhere.swift": "let picker = CopyDocumentPicker()\n"}):
        failures.append("a mention of the presenter outside the condition passed the check")

    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
