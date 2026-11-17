#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The TalkBack the image builds is the package Andarium turns on and off.

Andarium's screen-reader switch names TalkBack's accessibility service by its
component. The series that builds TalkBack for the image gives the app the
package that component names, and the target-files verifier requires the
application at the path the product installs it to. A library an application
does not embed installs as a file of its own, which the product's generic
system rule keeps off the system partition, so it names the application's
partition too.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_accessibility_licenses import BUILD_PATCH as BUILD_PATCH_NAME
from test_accessibility_licenses import added_files, blocks, listed

REPOSITORY = Path(__file__).resolve().parents[3]
PATCHES = REPOSITORY / "dependencies/patches"
SCREEN_READER = REPOSITORY / "Andarium/AndariumCore/Sources/AndariumCore/ScreenReader.swift"
BUILD_PATCH = REPOSITORY / "dependencies/patches/talkback/Build-with-Soong.patch"
VERIFIER = REPOSITORY / "android/lineage-24/tools/verify-target-files.py"
SERVICE = "com.google.android.marvin.talkback.TalkBackService"


def build_files(project: str) -> dict[str, str]:
    """path -> text of each Android.bp the project's series leave.

    The Soong build adds the files; the patches after it change them.
    """
    names = [
        line.split("\t")[1]
        for line in (PATCHES / project / "series.tsv").read_text(encoding="utf-8").splitlines()
        if line.startswith("patch\t")
    ]
    files = {path: text for path, text in added_files(project).items() if path.endswith("Android.bp")}
    with tempfile.TemporaryDirectory() as directory:
        for path, text in files.items():
            (Path(directory) / path).parent.mkdir(parents=True, exist_ok=True)
            (Path(directory) / path).write_text(text, encoding="utf-8")
        environment = dict(os.environ, GIT_CEILING_DIRECTORIES=str(Path(directory).parent))
        for name in names[names.index(BUILD_PATCH_NAME) + 1:]:
            subprocess.run(
                ["git", "apply", "--include=*Android.bp", str(PATCHES / project / name)],
                cwd=directory, env=environment, check=True, capture_output=True)
        return {path: (Path(directory) / path).read_text(encoding="utf-8") for path in files}


class AccessibilityPackageTests(unittest.TestCase):
    def test_the_built_app_has_the_package_the_switch_recognises(self) -> None:
        patch = BUILD_PATCH.read_text(encoding="utf-8")
        manifest = patch[patch.index("+++ b/src/soong/AndroidManifest.xml"):]
        package = re.search(r'^\+\s+package="([^"]+)"', manifest, re.M)
        self.assertIsNotNone(package)
        self.assertIn(f'"{package.group(1)}/{SERVICE}"', SCREEN_READER.read_text(encoding="utf-8"))

    def test_a_library_an_app_does_not_embed_installs_on_the_apps_partition(self) -> None:
        checked = []
        for project in ("talkback", "espeak-ng"):
            files = build_files(project)
            libraries = blocks(files, "cc_library_shared")
            for app, (_, body) in blocks(files, "android_app").items():
                if "product_specific: true" not in body or "use_embedded_native_libs: true" in body:
                    continue
                for library in listed(body, "jni_libs"):
                    checked.append(library)
                    self.assertIn(
                        "product_specific: true", libraries[library][1],
                        f"{library} installs outside the product partition of {app}")
        self.assertIn("espeak_ng_ttsespeak", checked)

    def test_the_verifier_requires_the_apps_where_the_product_installs_them(self) -> None:
        text = VERIFIER.read_text(encoding="utf-8")
        for module in ("TalkBack", "eSpeakNG"):
            self.assertIn(f"PRODUCT/app/{module}/{module}\\.apk", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
