#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The licence metadata of the two accessibility applications.

overlay/accessibility/Android.bp imports TalkBack and eSpeak NG prebuilt, so
the image's NOTICE reports what each module declares. Each import names its
own licence module (the Apache-2.0 of the build definition covers the build
definition, not the imported binary), and each module carries the kinds of
what the APK holds and the full text of every copyleft kind it names.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re
import unittest
from pathlib import Path

OVERLAY = Path(__file__).resolve().parents[3] / "android/lineage-24/overlay/accessibility"
BLUEPRINT = (OVERLAY / "Android.bp").read_text(encoding="utf-8")
TEXTS = {
    "GPL-3.0-or-later": ("licenses/GPL-3.0.txt", "GNU GENERAL PUBLIC LICENSE", "Version 3, 29 June 2007"),
    "LGPL-2.1-or-later": ("licenses/LGPL-2.1.txt", "GNU LESSER GENERAL PUBLIC LICENSE", "Version 2.1, February 1999"),
}


def blocks(kind: str) -> dict[str, str]:
    """name -> body of each top-level `kind { ... }` block."""
    found = {}
    for match in re.finditer(rf"^{kind} {{\n(.*?)^}}", BLUEPRINT, re.S | re.M):
        name = re.search(r'name: "([^"]+)"', match.group(1))
        found[name.group(1)] = match.group(1)
    return found


def listed(body: str, field: str) -> list[str]:
    match = re.search(rf"{field}: \[(.*?)\]", body, re.S)
    return re.findall(r'"([^"]+)"', match.group(1)) if match else []


class AccessibilityLicenseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.licenses = blocks("license")
        self.imports = blocks("android_app_import")

    def test_each_import_names_its_own_licence_module(self) -> None:
        self.assertEqual(sorted(self.imports), ["TalkBack", "eSpeakNG"])
        modules = {name: listed(body, "licenses") for name, body in self.imports.items()}
        self.assertEqual(modules["TalkBack"], ["kernarium_accessibility_talkback_license"])
        self.assertEqual(modules["eSpeakNG"], ["kernarium_accessibility_espeak_ng_license"])
        for names in modules.values():
            for name in names:
                self.assertIn(name, self.licenses)

    def test_the_kinds_are_what_each_apk_holds(self) -> None:
        talkback = listed(self.licenses["kernarium_accessibility_talkback_license"], "license_kinds")
        espeak = listed(self.licenses["kernarium_accessibility_espeak_ng_license"], "license_kinds")
        self.assertIn("SPDX-license-identifier-Apache-2.0", talkback)
        self.assertIn("SPDX-license-identifier-LGPL-2.1-or-later", talkback)  # BRLTTY and liblouis
        self.assertIn("SPDX-license-identifier-GPL-3.0-or-later", espeak)
        self.assertNotIn("SPDX-license-identifier-GPL-3.0-or-later", talkback)
        for kinds in (talkback, espeak):
            self.assertTrue(all(kind.startswith("SPDX-license-identifier-") for kind in kinds))

    def test_every_copyleft_kind_ships_its_full_text(self) -> None:
        for body in self.licenses.values():
            kinds = [kind.removeprefix("SPDX-license-identifier-") for kind in listed(body, "license_kinds")]
            texts = listed(body, "license_text")
            for kind, (path, *needles) in TEXTS.items():
                if kind in kinds:
                    self.assertIn(path, texts, f"{kind} has no licence text")
            for path in texts:
                content = (OVERLAY / path).read_text(encoding="utf-8")
                self.assertGreater(len(content), 5000, path)
                expected = [needles for tracked, *needles in TEXTS.values() if tracked == path]
                self.assertTrue(expected, f"{path} is not a text this test knows")
                for needle in expected[0]:
                    self.assertIn(needle, content[:400], path)

    def test_no_import_falls_back_to_the_build_definitions_licence(self) -> None:
        for name, body in self.imports.items():
            self.assertNotIn("Android-Apache-2.0", listed(body, "licenses"), name)


if __name__ == "__main__":
    unittest.main(verbosity=2)
