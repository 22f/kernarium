#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The TalkBack the image builds is the package Andarium turns on and off.

Andarium's screen-reader switch names TalkBack's accessibility service by its
component. The series that builds TalkBack for the image gives the app the
package that component names, and the target-files verifier requires the
application at the path the product installs it to.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re
import unittest
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[3]
SCREEN_READER = REPOSITORY / "Andarium/AndariumCore/Sources/AndariumCore/ScreenReader.swift"
BUILD_PATCH = REPOSITORY / "dependencies/patches/talkback/Build-with-Soong.patch"
VERIFIER = REPOSITORY / "android/lineage-24/tools/verify-target-files.py"
SERVICE = "com.google.android.marvin.talkback.TalkBackService"


class AccessibilityPackageTests(unittest.TestCase):
    def test_the_built_app_has_the_package_the_switch_recognises(self) -> None:
        patch = BUILD_PATCH.read_text(encoding="utf-8")
        manifest = patch[patch.index("+++ b/src/soong/AndroidManifest.xml"):]
        package = re.search(r'^\+\s+package="([^"]+)"', manifest, re.M)
        self.assertIsNotNone(package)
        self.assertIn(f'"{package.group(1)}/{SERVICE}"', SCREEN_READER.read_text(encoding="utf-8"))

    def test_the_verifier_requires_the_apps_where_the_product_installs_them(self) -> None:
        text = VERIFIER.read_text(encoding="utf-8")
        for module in ("TalkBack", "eSpeakNG"):
            self.assertIn(f"PRODUCT/app/{module}/{module}\\.apk", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
