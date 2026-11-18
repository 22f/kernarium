#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""dependencies/andarium-accessibility/identity.py.

The expected identity is what the tracked pins, series, archive list, builder
and toolchain say; the payload scan finds the values of the machine that built
an APK where a path begins and nowhere else; an APK's entries digest ignores
its signature; and a description built from prepared sources records them.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
import io
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
import patch_series  # noqa: E402  (its isolated environment commits as the caller, as a replay does)

specification = importlib.util.spec_from_file_location(
    "accessibility_identity", ROOT / "dependencies/andarium-accessibility/identity.py")
identity = importlib.util.module_from_spec(specification)
sys.modules[specification.name] = identity
specification.loader.exec_module(identity)


def apk(path: Path, **entries: bytes) -> Path:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for name, content in entries.items():
            archive.writestr(name.replace("__", "/"), content)
    path.write_bytes(buffer.getvalue())
    return path


class IdentityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)

    def tearDown(self) -> None:
        self.directory.cleanup()

    def test_the_tracked_tree_has_an_expected_identity(self) -> None:
        wanted = identity.expected(ROOT)
        self.assertEqual(wanted["source.sonic.head"], wanted["source.sonic.pin"])
        self.assertEqual(wanted["recipe.blob"], subprocess.run(
            ["git", "hash-object", str(ROOT / identity.BUILDER)], capture_output=True, text=True, check=True).stdout.strip())
        for item in ("jdk", "ndk", "build-tools", "platform-api", "platform-revision", "cmake"):
            self.assertIn(f"toolchain.{item}", wanted)

    def test_the_payload_scan_finds_a_machine_value_where_a_path_begins(self) -> None:
        roots = identity.FORBIDDEN_ROOTS
        for content in (*(root.encode() + b"someone/work/a.c" for root in roots),
                        b"x\0.build/dependency-build/y",
                        (roots[0] + "someone").encode("utf-16-le")):
            with self.subTest(content):
                self.assertTrue(identity.scan_payload(apk(self.root / "a.apk", **{"lib__a.so": content})))
        home = b"/home/" + b"someone/x"
        self.assertTrue(identity.scan_payload(apk(self.root / "b.apk", **{"classes.dex": home}), user="someone"))
        self.assertTrue(identity.scan_payload(apk(self.root / "c.apk", **{"classes.dex": b"/work/root/x"}), forbidden=("/work/root",)))
        self.assertEqual(identity.scan_payload(apk(self.root / "d.apk", **{"Users__x.txt": b"a"})), [])
        self.assertEqual(identity.scan_payload(apk(self.root / "e.apk", **{roots[0].lstrip("/") + "x.txt": b"a"})), [])
        self.assertTrue(identity.scan_payload(apk(self.root / "f.apk", **{"entry": b"a"}), forbidden=("entry",)))

    def test_the_payload_scan_leaves_android_paths_alone(self) -> None:
        clean = apk(self.root / "a.apk", **{"classes.dex": b"/data/local/tmp/ /sdcard/ /system/ /proc/self/ /usr/lib/ "
                                                      b"https://example.invalid/Users/ not/Users/"})
        self.assertEqual(identity.scan_payload(clean, user="someone"), [])

    def test_the_entries_digest_ignores_the_signature_and_nothing_else(self) -> None:
        unsigned = apk(self.root / "u.apk", **{"classes.dex": b"dex", "AndroidManifest.xml": b"m"})
        signed = apk(self.root / "s.apk", **{"classes.dex": b"dex", "AndroidManifest.xml": b"m",
                                              "META-INF__CERT.RSA": b"signature", "META-INF__MANIFEST.MF": b"x"})
        changed = apk(self.root / "c.apk", **{"classes.dex": b"dez", "AndroidManifest.xml": b"m"})
        self.assertEqual(identity.entries_sha256(unsigned), identity.entries_sha256(signed))
        self.assertNotEqual(identity.entries_sha256(unsigned), identity.entries_sha256(changed))

    def test_a_description_records_the_prepared_sources(self) -> None:
        sources = self.root / "sources"
        for name in identity.SOURCES:
            repository = sources / name
            repository.mkdir(parents=True)
            for command in (["init", "-q"], ["commit", "-q", "--allow-empty", "-m", name]):
                subprocess.run(["git", "-C", str(repository), *command], check=True, capture_output=True,
                               env=patch_series.isolated_environment())
        apks = self.root / "apks"
        apks.mkdir()
        for name, package in identity.APKS.items():
            apk(apks / name, **{"AndroidManifest.xml": package.encode("utf-16-le"), "classes.dex": b"d"})
        description = identity.describe(ROOT, sources, apks, {"jdk": "17"})
        head = subprocess.run(["git", "-C", str(sources / "talkback"), "rev-parse", "HEAD^{tree}"],
                              capture_output=True, text=True, check=True).stdout.strip()
        self.assertEqual(description["sources"]["talkback"]["tree"], head)
        self.assertEqual(description["sources"]["sonic"]["pin"], identity.manifest_pins(ROOT)["sonic"])
        self.assertEqual(description["patch_series"]["espeak-ng"], identity.series_tree(ROOT, "espeak-ng"))
        self.assertEqual(identity.recorded(description)["toolchain.jdk"], "17")

    def test_the_tool_makes_no_network_call(self) -> None:
        text = (ROOT / "dependencies/andarium-accessibility/identity.py").read_text()
        for word in ("urllib", "socket", "http.client", "requests"):
            self.assertNotIn(f"import {word}", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
