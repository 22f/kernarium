#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The TalkBack the image builds is the package Andarium turns on and off.

Andarium's screen-reader switch names TalkBack's accessibility service by its
component. The series that builds TalkBack for the image gives the app the
package that component names, and the target-files verifier requires the
application at the path the product installs it to. A library an application
does not embed installs as a file of its own, which the product's generic
system rule keeps off the system partition, so it names the application's
partition too. Neither application requests a network or Google service
permission.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
import os
import re
import subprocess
import sys
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


# The permissions the TalkBack series takes out of its manifests, and the manifests they leave.
NETWORK_PERMISSIONS = (
    "android.permission.INTERNET",
    "android.permission.ACCESS_NETWORK_STATE",
    "com.google.android.providers.gsf.permission.READ_GSERVICES",
    "com.google.android.apps.aicore.service.BIND_SERVICE",
)
TALKBACK_MANIFEST = "talkback/src/main/AndroidManifest.xml"
BRAILLE_DISPLAY_MANIFEST = "braille/brailledisplay/src/phone/AndroidManifest.xml"
BRAILLE_KEYBOARD_MANIFEST = "braille/brailleime/src/phone/AndroidManifest.xml"
# A signature guard for receivers that the installed package (com.android.talkback) never declares.
UNDECLARED_GUARD_PERMISSION = (
    "com.google.android.marvin.talkback.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION"
)
PERMISSION_REQUEST = re.compile(r'<uses-permission[^>]*android:name="([^"]+)"')
# An element that carries its name on the next line.
PERMISSION_ELEMENT_OPEN = re.compile(r"<uses-permission\s*$")
PERMISSION_NAME = re.compile(r'^\s*android:name="([^"]+)"')


def requests_changed(project: str) -> tuple[set[tuple[str, str]], set[tuple[str, str]]]:
    """(path, permission) of every <uses-permission> a project's series adds and removes."""
    added: set[tuple[str, str]] = set()
    removed: set[tuple[str, str]] = set()
    for line in (PATCHES / project / "series.tsv").read_text(encoding="utf-8").splitlines():
        if not line.startswith("patch\t"):
            continue
        path = ""
        opened = ""
        for text in (PATCHES / project / line.split("\t")[1]).read_text(encoding="utf-8").splitlines():
            if text.startswith("+++ b/"):
                path = text[len("+++ b/"):]
            if not text or text[0] not in "+-" or not path.endswith("AndroidManifest.xml"):
                opened = ""
                continue
            request = PERMISSION_REQUEST.search(text[1:])
            name = PERMISSION_NAME.search(text[1:]) if opened == text[0] else None
            if request or name:
                (added if text[0] == "+" else removed).add((path, (request or name).group(1)))
            opened = text[0] if PERMISSION_ELEMENT_OPEN.search(text[1:]) else ""
    return added, removed


def load_verifier():
    """Import the target-files verifier, whose hyphenated file name is not a module name."""
    spec = importlib.util.spec_from_file_location("verify_target_files", VERIFIER)
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(VERIFIER.parent))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.remove(str(VERIFIER.parent))
    return module


class AccessibilityPackageTests(unittest.TestCase):
    def test_the_series_leave_the_network_and_google_service_permissions_out(self) -> None:
        added, removed = requests_changed("talkback")
        for permission in NETWORK_PERMISSIONS:
            self.assertIn((TALKBACK_MANIFEST, permission), removed)
        self.assertIn((BRAILLE_DISPLAY_MANIFEST, "android.permission.INTERNET"), removed)
        self.assertIn((BRAILLE_KEYBOARD_MANIFEST, UNDECLARED_GUARD_PERMISSION), removed)
        self.assertEqual({permission for _, permission in added} & set(NETWORK_PERMISSIONS), set())
        self.assertEqual(requests_changed("espeak-ng")[0], set())

    def test_the_verifier_refuses_every_network_and_google_service_permission(self) -> None:
        verifier = load_verifier()
        refuse = verifier.accessibility_refused_permission
        # Named in the list, so the list alone catches them.
        for permission in verifier.ACCESSIBILITY_REFUSED_PERMISSIONS:
            self.assertTrue(refuse(permission), permission)
        # Caught by the name rule alone: the list does not name them.
        by_name = tuple(f"android.permission.{name}" for name in (
            "OVERRIDE_WIFI_CONFIG", "MANAGE_WIFI_NETWORK_SELECTION", "NETWORK_FACTORY",
            "NETWORK_SIGNAL_STRENGTH_WAKEUP", "CONNECTIVITY_CONSTRAINED",
            "WIFI_ACCESS_COEX_UNSAFE_CHANNELS", "ACCESS_WIMAX_SCAN", "INTERNET_PRIVILEGED"))
        for permission in by_name:
            self.assertNotIn(permission, verifier.ACCESSIBILITY_REFUSED_PERMISSIONS)
            self.assertTrue(refuse(permission), permission)
        # Google's namespace, with no exemption for the package TalkBack is built from.
        for permission in (
            UNDECLARED_GUARD_PERMISSION,
            "com.google.android.marvin.talkback.OTHER",
            "com.google.android.providers.gsf.permission.READ_GSERVICES",
            "com.google.android.apps.aicore.service.BIND_SERVICE",
            "com.android.vending.BILLING",
        ):
            self.assertTrue(refuse(permission), permission)
        # Bluetooth, USB and the rest of what TalkBack and eSpeak NG keep are not network access.
        for permission in tuple(f"android.permission.{name}" for name in (
            "USE_FINGERPRINT", "READ_PHONE_STATE", "VIBRATE", "WAKE_LOCK", "MODIFY_AUDIO_SETTINGS",
            "RECORD_AUDIO", "QUERY_ALL_PACKAGES", "RECEIVE_BOOT_COMPLETED", "FOREGROUND_SERVICE",
            "POST_NOTIFICATIONS", "ACCESS_FINE_LOCATION", "ACCESS_COARSE_LOCATION", "BLUETOOTH",
            "BLUETOOTH_ADMIN", "BLUETOOTH_CONNECT", "BLUETOOTH_SCAN", "MANAGE_USB")):
            self.assertFalse(refuse(permission), permission)

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
