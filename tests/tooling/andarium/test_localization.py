#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Andarium/scripts/localization.py checks the String Catalogs and syncs them with the code.

The real tree must pass its own check. A small fixture tree (one catalog of
each kind, the project's permission text, one What's New note) passes it, and
each defect the script's header lists is made in the fixture alone and must
be found. `sync` runs Apple's `xcstringstool` on a hand-written
`.stringsdata`, so that part needs a Mac and skips elsewhere.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import copy
import importlib.util
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPOSITORY / "Andarium/scripts/localization.py"
BOUND_SECONDS = 120

NOTE = "Browse the guest’s storage."
PERMISSION = "Andarium records from the microphone only when you turn on Microphone."
LANGS = ("ja", "zh-Hans", "zh-Hant")


def load_script():
    spec = importlib.util.spec_from_file_location("localization", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules["localization"] = module
    spec.loader.exec_module(module)
    return module


def unit(value: str, state: str = "translated") -> dict:
    return {"stringUnit": {"state": state, "value": value}}


def entry(**values: str) -> dict:
    return {"localizations": {lang: unit(values[lang.replace("-", "_")]) for lang in LANGS}}


def catalog(strings: dict) -> dict:
    return {"sourceLanguage": "en", "strings": strings, "version": "1.0"}


class Tree:
    """A fixture repository: the paths the script reads, with one valid entry in each."""

    def __init__(self, root: pathlib.Path, script) -> None:
        self.root = root
        self.script = script
        self.files = {
            "Localizable": catalog({
                "Guest system": entry(ja="ゲストシステム", zh_Hans="客户机系统", zh_Hant="客體系統"),
                "%@ of %@": entry(ja="%2$@中 %1$@", zh_Hans="%1$@，共 %2$@", zh_Hant="%1$@，共 %2$@"),
                "%lld snapshots": {"localizations": {
                    "en": {"variations": {"plural": {"one": unit("%lld snapshot"), "other": unit("%lld snapshots")}}},
                    **{lang: {"variations": {"plural": {"other": unit("%lld X")}}} for lang in LANGS},
                }},
                "Retina": {"shouldTranslate": False},
            }),
            "AndariumCore": catalog({"Paused": entry(ja="一時停止", zh_Hans="已暂停", zh_Hant="已暫停")}),
            "InfoPlist": catalog({"NSMicrophoneUsageDescription": {"localizations": {
                "en": unit(PERMISSION), **{lang: unit(f"{lang}: 麦克风") for lang in LANGS}}}}),
            "WhatsNew": catalog({NOTE: {"extractionState": "manual", **entry(
                ja="ストレージを参照します。", zh_Hans="浏览存储空间。", zh_Hant="瀏覽儲存空間。")}}),
        }
        self.english_only = {
            "identical": [{"catalog": "Localizable", "key": "Retina", "reason": "a name"}],
            "english": [{"text": "JIT", "file": "Andarium/Andarium/Shared/Dev.swift", "reason": "development only"}],
        }
        self.sources = {"Andarium/Andarium/Shared/Dev.swift": 'Text(verbatim: "JIT")\n'}
        self.whats_new = [{"entries": [{"text": NOTE}]}]
        self.project = (
            '{ "targets": [ {\n  "name": "Andarium",\n  "id": "A1",\n  "settings": {\n'
            f'    "INFOPLIST_KEY_NSMicrophoneUsageDescription": "{PERMISSION}"\n  }} }},\n'
            '  { "name": "AndariumPreview",\n    "id": "A2", "settings": {} } ] }\n'
        )

    def write(self) -> pathlib.Path:
        for name, relative in self.script.CATALOGS.items():
            self.put(relative, json.dumps(self.files[name], ensure_ascii=False, indent=2))
        self.put(self.script.ENGLISH_ONLY, json.dumps(self.english_only, indent=2))
        self.put(self.script.WHATS_NEW, json.dumps(self.whats_new, ensure_ascii=False))
        self.put(self.script.PROJECT, self.project)
        for relative, text in self.sources.items():
            self.put(relative, text)
        return self.root

    def put(self, relative: str, text: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def string(self, catalog_name: str, key: str) -> dict:
        return self.files[catalog_name]["strings"][key]


class CheckTests(unittest.TestCase):
    def setUp(self) -> None:
        self.script = load_script()
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.tree = Tree(pathlib.Path(scratch.name), self.script)

    def problems(self) -> list[str]:
        return self.script.check(self.tree.write())

    def assertFinds(self, text: str) -> None:
        found = self.problems()
        self.assertTrue(any(text in problem for problem in found), f"{text!r} not reported; got {found}")

    def test_the_real_tree_passes_its_own_check(self) -> None:
        run = subprocess.run(
            [sys.executable, "-B", str(SCRIPT), "check", "--root", str(REPOSITORY)],
            capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=BOUND_SECONDS, check=False)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn("the String Catalogs are complete", run.stdout)

    def test_the_fixture_passes(self) -> None:
        self.assertEqual(self.problems(), [])

    def test_a_missing_language(self) -> None:
        del self.tree.string("Localizable", "Guest system")["localizations"]["ja"]
        self.assertFinds("has no ja translation")

    def test_a_value_not_marked_translated(self) -> None:
        self.tree.string("AndariumCore", "Paused")["localizations"]["zh-Hant"] = unit("已暫停", "needs_review")
        self.assertFinds("not 'translated'")

    def test_a_new_value(self) -> None:
        self.tree.string("Localizable", "Guest system")["localizations"]["zh-Hans"] = unit("客户机系统", "new")
        self.assertFinds("not 'translated'")

    def test_an_empty_value(self) -> None:
        self.tree.string("Localizable", "Guest system")["localizations"]["ja"] = unit(" ")
        self.assertFinds("is empty")

    def test_a_stale_key(self) -> None:
        self.tree.string("Localizable", "Guest system")["extractionState"] = "stale"
        self.assertFinds("is stale")

    def test_a_missing_format_specifier(self) -> None:
        self.tree.string("Localizable", "%@ of %@")["localizations"]["ja"] = unit("%1$@中")
        self.assertFinds("are not those of the source")

    def test_reordered_specifiers_need_positions(self) -> None:
        self.tree.string("Localizable", "%@ of %@")["localizations"]["zh-Hant"] = unit("%2$@ %@")
        self.assertFinds("mixes positional and plain")

    def test_positions_that_skip_a_number(self) -> None:
        self.tree.string("Localizable", "%@ of %@")["localizations"]["ja"] = unit("%1$@ %3$@")
        self.assertFinds("are not 1 to 2")

    def test_a_plural_category_a_language_does_not_have(self) -> None:
        plural = self.tree.string("Localizable", "%lld snapshots")["localizations"]["ja"]["variations"]["plural"]
        plural["one"] = unit("%lld X")
        self.assertFinds("plural category 'one', which ja does not have")

    def test_a_plural_without_other(self) -> None:
        plural = self.tree.string("Localizable", "%lld snapshots")["localizations"]["ja"]["variations"]["plural"]
        plural["many"] = plural.pop("other")
        self.assertFinds("plural variations without 'other'")

    def test_a_google_name_in_a_value(self) -> None:
        self.tree.string("Localizable", "Guest system")["localizations"]["zh-Hans"] = unit("Google 客户机系统")
        self.assertFinds("names a Google product")

    def test_a_google_name_in_a_key(self) -> None:
        self.tree.files["Localizable"]["strings"]["Open Google Play"] = entry(ja="開く", zh_Hans="打开", zh_Hant="開啟")
        self.assertFinds("names a Google product")

    def test_english_left_untranslated(self) -> None:
        self.tree.files["Localizable"]["strings"]["Erase guest"] = entry(
            ja="Erase guest", zh_Hans="抹掉客户机", zh_Hant="清除客體")
        self.assertFinds("is the English text")

    def test_an_untranslated_key_off_the_list(self) -> None:
        self.tree.files["Localizable"]["strings"]["Metal"] = {"shouldTranslate": False}
        self.assertFinds("is not translated but is not on the list")

    def test_a_list_entry_for_a_key_that_is_gone(self) -> None:
        self.tree.english_only["identical"].append({"catalog": "Localizable", "key": "Gone", "reason": "none"})
        self.assertFinds("names 'Gone'")

    def test_an_english_text_no_longer_in_its_file(self) -> None:
        self.tree.sources["Andarium/Andarium/Shared/Dev.swift"] = "// nothing\n"
        self.assertFinds("is no longer in")

    def test_an_english_text_that_a_catalog_translates(self) -> None:
        self.tree.files["Localizable"]["strings"]["JIT"] = entry(ja="JIT", zh_Hans="即时编译", zh_Hant="即時編譯")
        self.assertFinds("a catalog translates it")

    def test_a_permission_text_without_an_entry(self) -> None:
        del self.tree.files["InfoPlist"]["strings"]["NSMicrophoneUsageDescription"]
        self.assertFinds("has no entry for NSMicrophoneUsageDescription")

    def test_a_permission_entry_that_differs_from_the_project(self) -> None:
        self.tree.string("InfoPlist", "NSMicrophoneUsageDescription")["localizations"]["en"] = unit("Something else.")
        self.assertFinds("differs from the project's text")

    def test_a_permission_text_of_another_target_is_not_expected(self) -> None:
        self.tree.project = self.tree.project.replace(
            '"settings": {} } ] }',
            f'"settings": {{ "INFOPLIST_KEY_NSCameraUsageDescription": "Camera." }} }} ] }}')
        self.assertEqual(self.problems(), [])

    def test_a_whats_new_note_without_an_entry(self) -> None:
        self.tree.whats_new[0]["entries"].append({"title": "More", "points": ["A new point"]})
        self.assertFinds("has no entry for the note 'A new point'")

    def test_a_whats_new_entry_that_is_no_note(self) -> None:
        self.tree.files["WhatsNew"]["strings"]["Removed note"] = entry(ja="a", zh_Hans="b", zh_Hant="c")
        self.assertFinds("is no note of")

    def test_a_catalog_in_another_source_language(self) -> None:
        self.tree.files["AndariumCore"]["sourceLanguage"] = "de"
        self.assertFinds("the source language is 'de'")

    def test_a_missing_catalog(self) -> None:
        self.tree.write()
        (self.tree.root / self.script.CATALOGS["WhatsNew"]).unlink()
        self.assertTrue(any("is missing" in problem for problem in self.script.check(self.tree.root)))


@unittest.skipUnless(shutil.which("xcrun"), "xcstringstool is part of Xcode")
class SyncTests(unittest.TestCase):
    def setUp(self) -> None:
        self.script = load_script()
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.scratch = pathlib.Path(scratch.name)
        self.tree = Tree(self.scratch / "tree", self.script)
        self.source = "Andarium/Andarium/Shared/Home.swift"
        self.tree.sources[self.source] = "// strings are extracted from here\n"
        self.root = self.tree.write()
        self.data = self.scratch / "build" / "Home.stringsdata"

    def extract(self, *keys: str) -> None:
        self.data.parent.mkdir(parents=True, exist_ok=True)
        body = {
            "source": str(self.root / self.source),
            "tables": {"Localizable": [{"comment": "", "key": key, "location": {"startingColumn": 1, "startingLine": 1}}
                                       for key in keys]},
            "version": 1,
        }
        self.data.write_text(json.dumps(body), encoding="utf-8")

    def sync(self, *extra: str) -> list[str]:
        return self.script.sync(self.root, [self.data.parent], "--check" in extra)

    def catalog(self) -> dict:
        return json.loads((self.root / self.script.CATALOGS["Localizable"]).read_text(encoding="utf-8"))["strings"]

    def test_a_new_string_is_added_untranslated_and_the_check_then_fails(self) -> None:
        self.extract("Guest system", "%@ of %@", "%lld snapshots", "Retina", "Erase guest")
        problems = self.sync("--check")
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("1 to add ['Erase guest']", problems[0])
        self.assertNotIn("Erase guest", self.catalog(), "--check must change nothing")
        self.assertEqual(self.sync(), [])
        self.assertNotIn("localizations", self.catalog()["Erase guest"])
        self.assertEqual(self.sync("--check"), [])
        self.assertTrue(any("has no ja translation" in problem for problem in self.script.check(self.root)))

    def test_a_string_the_code_dropped_becomes_stale(self) -> None:
        self.extract("Guest system", "%@ of %@", "Retina")
        self.assertEqual(self.sync(), [])
        self.assertEqual(self.catalog()["%lld snapshots"].get("extractionState"), "stale")
        self.assertTrue(any("is stale" in problem for problem in self.script.check(self.root)))

    def test_a_tree_in_step_has_nothing_to_change(self) -> None:
        self.extract("Guest system", "%@ of %@", "%lld snapshots", "Retina")
        self.assertEqual(self.sync("--check"), [])

    def test_a_build_without_extracted_strings(self) -> None:
        empty = self.scratch / "empty"
        empty.mkdir()
        problems = self.script.sync(self.root, [empty], True)
        self.assertEqual(len(problems), 1)
        self.assertIn("no .stringsdata", problems[0])


if __name__ == "__main__":
    unittest.main()
