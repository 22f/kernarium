#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""android/lineage-24/tools/stage-accessibility-artifact.py.

The stager takes only an artifact whose files match its description, whose
description matches what the source tree tracks (the pinned commits, the patch
series trees, the archive list, the builder and the toolchain), and which the
tracked descriptor accepts. Each input changed after the build refuses the old
artifact. It writes the APKs and the filegroups the product imports, removes
anything else the destination held, and leaves an unchanged file's time alone
so an unchanged artifact rebuilds nothing.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
STAGER = ROOT / "android/lineage-24/tools/stage-accessibility-artifact.py"
OVERLAY = ROOT / "android/lineage-24/overlay/accessibility/Android.bp"
ACCESSIBILITY = ROOT / "dependencies/andarium-accessibility"

specification = importlib.util.spec_from_file_location("accessibility_identity", ACCESSIBILITY / "identity.py")
identity = importlib.util.module_from_spec(specification)
sys.modules[specification.name] = identity
specification.loader.exec_module(identity)

PINS = {"talkback": "1" * 40, "espeak-ng": "2" * 40, "sonic": "3" * 40}
TREES = {"talkback": "4" * 40, "espeak-ng": "5" * 40}


def run(*arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-B", str(STAGER), *arguments],
        stdin=subprocess.DEVNULL, capture_output=True, text=True,
    )


def make_tree(root: Path) -> Path:
    """A source tree with the tracked inputs the identity reads."""
    tree = root / "tree"
    directory = tree / "dependencies/andarium-accessibility"
    directory.mkdir(parents=True)
    for name in ("identity.py", "toolchain.tsv"):
        shutil.copy(ACCESSIBILITY / name, directory / name)
    (directory / "archives.tsv").write_text("name\turl\tsha256\ngradle-1-bin.zip\thttps://example.invalid/g\t" + "a" * 64 + "\n")
    (tree / "dependencies/build-andarium-accessibility.sh").write_text("#!/bin/sh\nexit 0\n")
    (tree / "dependencies/manifest.tsv").write_text(
        "# header\nname\tversion\tproducts\trepository\tcommit\tsource_paths\tpatch_series\n"
        + "".join(f"{name}\t1\tandarium-accessibility\thttps://example.invalid/{name}.git\t{pin}\t.\t-\n"
                  for name, pin in PINS.items()))
    for name, series in TREES.items():
        patches = tree / "dependencies/patches" / name
        patches.mkdir(parents=True)
        (patches / "series.tsv").write_text(f"# header\ntree\t{series}\npatch\tOne.patch\n")
    (tree / "android/lineage-24/metadata").mkdir(parents=True)
    return tree


def make_apk(path: Path, package: str, extra: bytes = b"") -> None:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        archive.writestr("AndroidManifest.xml", b"\x03\x00" + package.encode("utf-16-le") + b"\0\0")
        archive.writestr("classes.dex", b"dex\n035\0" + extra)
        archive.writestr("META-INF/CERT.SF", b"signature")
    path.write_bytes(buffer.getvalue())


def make_artifact(root: Path, tree: Path, **changes) -> Path:
    artifact = root / "artifact"
    artifact.mkdir()
    wanted = identity.expected(tree)
    apks = {}
    for name, package in identity.APKS.items():
        make_apk(artifact / name, package)
        apks[name] = {
            "package": package,
            "sha256": identity.sha256_file(artifact / name),
            "entries_sha256": identity.entries_sha256(artifact / name),
        }
    sources = {
        name: {"pin": PINS[name], "head": "c" * 40, **({"tree": wanted[f"source.{name}.tree"]} if name in TREES else {})}
        for name in PINS
    }
    sources["sonic"]["head"] = wanted["source.sonic.head"]
    description = {
        "schema": identity.SCHEMA,
        "apks": apks,
        "sources": sources,
        "patch_series": {name: wanted[f"source.{name}.tree"] for name in TREES},
        "toolchain": {key.split(".", 1)[1]: value for key, value in wanted.items() if key.startswith("toolchain.")},
        "archives_sha256": wanted["archives.sha256"],
        "recipe": wanted["recipe.blob"],
    }
    description.update(changes)
    (artifact / "artifact.json").write_text(json.dumps(description), encoding="utf-8")
    return artifact


class StageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.aosp = self.root / "aosp"
        (self.aosp / ".repo").mkdir(parents=True)
        self.tree = make_tree(self.root)

    def tearDown(self) -> None:
        self.directory.cleanup()

    def stager(self, artifact: Path, *arguments: str) -> subprocess.CompletedProcess:
        return run("--artifact", str(artifact), "--repo-root", str(self.tree), *arguments)

    def accepted(self, **changes) -> Path:
        """An artifact built from the tree, accepted by the tracked descriptor."""
        artifact = make_artifact(self.root, self.tree, **changes)
        result = self.stager(artifact, "--accept")
        self.assertEqual(result.returncode, 0, result.stderr)
        return artifact

    def refused(self, artifact: Path, text: str, *arguments: str) -> None:
        result = self.stager(artifact, *(arguments or ("--check",)))
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(text, result.stderr)

    def test_an_accepted_artifact_is_staged_and_stale_files_go(self) -> None:
        artifact = self.accepted()
        destination = self.aosp / "vendor/kernarium/accessibility"
        destination.mkdir(parents=True)
        (destination / "Old.apk").write_bytes(b"stale")
        result = self.stager(artifact, "--aosp-source", str(self.aosp))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sorted(p.name for p in destination.iterdir()),
                         ["Android.bp", "TalkBack.apk", "artifact.json", "eSpeakNG.apk"])
        self.assertEqual((destination / "TalkBack.apk").read_bytes(), (artifact / "TalkBack.apk").read_bytes())

    def test_the_image_turns_no_accessibility_service_on_by_itself(self) -> None:
        # The README says the image enables nothing: no default of the overlay
        # or of a patch to the source writes either setting.
        for directory in (ROOT / "android/lineage-24/overlay", ROOT / "android/lineage-24/patches"):
            for path in directory.rglob("*"):
                if path.is_file() and path.suffix not in (".png", ".jar", ".apk"):
                    text = path.read_text(encoding="utf-8", errors="ignore")
                    for setting in ("enabled_accessibility_services", "accessibility_enabled"):
                        self.assertNotIn(setting, text, f"{path} sets {setting}")

    def test_the_filegroups_are_those_the_overlay_imports(self) -> None:
        artifact = self.accepted()
        self.assertEqual(self.stager(artifact, "--aosp-source", str(self.aosp)).returncode, 0)
        staged = (self.aosp / "vendor/kernarium/accessibility/Android.bp").read_text()
        for line in OVERLAY.read_text().splitlines():
            if "apk:" in line:
                group = line.split('"')[1].lstrip(":")
                self.assertIn(f'name: "{group}"', staged)

    def test_an_unchanged_file_keeps_its_time(self) -> None:
        artifact = self.accepted()
        self.assertEqual(self.stager(artifact, "--aosp-source", str(self.aosp)).returncode, 0)
        apk = self.aosp / "vendor/kernarium/accessibility/TalkBack.apk"
        os.utime(apk, (1_000_000, 1_000_000))
        self.assertEqual(self.stager(artifact, "--aosp-source", str(self.aosp)).returncode, 0)
        self.assertEqual(apk.stat().st_mtime, 1_000_000)

    # -- the files against the description ---------------------------------

    def test_a_changed_apk_is_refused(self) -> None:
        artifact = self.accepted()
        (artifact / "eSpeakNG.apk").write_bytes(b"changed")
        self.refused(artifact, "eSpeakNG.apk differs from artifact.json")

    def test_another_package_in_the_description_is_refused(self) -> None:
        artifact = self.accepted()
        description = json.loads((artifact / "artifact.json").read_text())
        description["apks"]["TalkBack.apk"]["package"] = "com.google.android.marvin.talkback"
        (artifact / "artifact.json").write_text(json.dumps(description))
        self.refused(artifact, "must be package com.android.talkback")

    def test_an_apk_of_another_package_is_refused_though_its_hash_matches(self) -> None:
        artifact = make_artifact(self.root, self.tree)
        make_apk(artifact / "TalkBack.apk", "com.example.other")
        description = json.loads((artifact / "artifact.json").read_text())
        description["apks"]["TalkBack.apk"]["sha256"] = identity.sha256_file(artifact / "TalkBack.apk")
        description["apks"]["TalkBack.apk"]["entries_sha256"] = identity.entries_sha256(artifact / "TalkBack.apk")
        (artifact / "artifact.json").write_text(json.dumps(description))
        self.refused(artifact, "does not name package com.android.talkback")

    def test_a_file_that_is_not_an_apk_is_refused_though_its_hash_matches(self) -> None:
        artifact = make_artifact(self.root, self.tree)
        (artifact / "TalkBack.apk").write_bytes(b"not a zip")
        description = json.loads((artifact / "artifact.json").read_text())
        description["apks"]["TalkBack.apk"]["sha256"] = identity.sha256_file(artifact / "TalkBack.apk")
        (artifact / "artifact.json").write_text(json.dumps(description))
        self.refused(artifact, "TalkBack.apk is not a zip archive")

    def test_changed_entries_are_refused(self) -> None:
        artifact = make_artifact(self.root, self.tree)
        description = json.loads((artifact / "artifact.json").read_text())
        description["apks"]["TalkBack.apk"]["entries_sha256"] = "0" * 64
        (artifact / "artifact.json").write_text(json.dumps(description))
        self.refused(artifact, "TalkBack.apk's entries differ")

    def test_a_payload_that_names_the_build_machine_is_refused(self) -> None:
        artifact = make_artifact(self.root, self.tree)
        make_apk(artifact / "TalkBack.apk", "com.android.talkback", identity.FORBIDDEN_ROOTS[1].encode() + b"checkout/jni.c\0")
        description = json.loads((artifact / "artifact.json").read_text())
        description["apks"]["TalkBack.apk"]["sha256"] = identity.sha256_file(artifact / "TalkBack.apk")
        description["apks"]["TalkBack.apk"]["entries_sha256"] = identity.entries_sha256(artifact / "TalkBack.apk")
        (artifact / "artifact.json").write_text(json.dumps(description))
        self.refused(artifact, f"holds a value of the machine that built it: classes.dex: {identity.FORBIDDEN_ROOTS[1]}")

    def target_files(self, artifact: Path, signed: bool = True, **replace: bytes) -> Path:
        """A target-files tree carrying the artifact's APKs, re-signed as the release does."""
        target = self.root / "target-files"
        for name, package in identity.APKS.items():
            directory = target / "PRODUCT/app" / Path(name).stem
            directory.mkdir(parents=True)
            if name in replace:
                make_apk(directory / name, package, replace[name])
                continue
            with zipfile.ZipFile(artifact / name) as built, zipfile.ZipFile(directory / name, "w") as copy:
                for info in built.infolist():
                    copy.writestr(info, built.read(info.filename))
                if signed:
                    copy.writestr("META-INF/RELEASE.RSA", b"another signature")
        return target

    def test_the_apks_of_the_target_files_are_the_artifacts_whatever_signs_them(self) -> None:
        artifact = self.accepted()
        target = self.target_files(artifact)
        result = self.stager(artifact, "--check", "--target-files", str(target))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_another_apk_in_the_target_files_is_refused(self) -> None:
        artifact = self.accepted()
        target = self.target_files(artifact, **{"TalkBack.apk": b"other classes"})
        self.refused(artifact, "the target files' TalkBack.apk is not the accessibility artifact's",
                     "--check", "--target-files", str(target))

    def test_an_apk_missing_from_the_target_files_is_refused(self) -> None:
        artifact = self.accepted()
        target = self.target_files(artifact)
        (target / "PRODUCT/app/eSpeakNG/eSpeakNG.apk").unlink()
        self.refused(artifact, "the target files carry no eSpeakNG.apk", "--check", "--target-files", str(target))

    def test_target_files_without_check_are_refused(self) -> None:
        artifact = self.accepted()
        self.refused(artifact, "--target-files goes with --check", "--accept", "--target-files", str(self.root))

    def test_a_description_without_its_sources_is_refused(self) -> None:
        artifact = make_artifact(self.root, self.tree, sources={"talkback": {}})
        self.refused(artifact, "artifact.json records no source espeak-ng")

    # -- the description against the tree: each input moved ----------------

    def test_each_tracked_input_that_moves_refuses_the_old_artifact(self) -> None:
        directory = self.tree / "dependencies/andarium-accessibility"
        manifest = (self.tree / "dependencies/manifest.tsv")
        original = manifest.read_text()
        changes = {
            "source.talkback.pin": lambda: manifest.write_text(original.replace(PINS["talkback"], "9" * 40)),
            "source.espeak-ng.pin": lambda: manifest.write_text(original.replace(PINS["espeak-ng"], "9" * 40)),
            "source.sonic.pin": lambda: manifest.write_text(original.replace(PINS["sonic"], "9" * 40)),
            "source.talkback.tree": lambda: (self.tree / "dependencies/patches/talkback/series.tsv").write_text(
                f"tree\t{'9' * 40}\n"),
            "source.espeak-ng.tree": lambda: (self.tree / "dependencies/patches/espeak-ng/series.tsv").write_text(
                f"tree\t{'9' * 40}\n"),
            "archives.sha256": lambda: (directory / "archives.tsv").write_text("name\turl\tsha256\n"),
            "recipe.blob": lambda: (self.tree / "dependencies/build-andarium-accessibility.sh").write_text("#!/bin/sh\n"),
            "toolchain.ndk": lambda: (directory / "toolchain.tsv").write_text(
                (directory / "toolchain.tsv").read_text().replace("ndk\t", "ndk\t9")),
            "toolchain.jdk": lambda: (directory / "toolchain.tsv").write_text(
                (directory / "toolchain.tsv").read_text().replace("jdk\t", "jdk\t9")),
        }
        artifact = self.accepted()
        for name, change in changes.items():
            with self.subTest(name):
                snapshot = {path: path.read_bytes() for path in self.tree.rglob("*") if path.is_file()}
                change()
                self.refused(artifact, f"{name}: the tree has")
                self.refused(artifact, f"{name}: the tree has", "--aosp-source", str(self.aosp))
                self.refused(artifact, f"{name}: the tree has", "--accept")
                for path, content in snapshot.items():
                    path.write_bytes(content)

    def test_an_artifact_built_from_other_sources_or_a_toolchain_is_refused(self) -> None:
        wanted = identity.expected(self.tree)
        for label, changes in {
            "source.talkback.tree": {"sources": {
                "talkback": {"pin": PINS["talkback"], "head": "c" * 40, "tree": "8" * 40},
                "espeak-ng": {"pin": PINS["espeak-ng"], "head": "c" * 40, "tree": TREES["espeak-ng"]},
                "sonic": {"pin": PINS["sonic"], "head": PINS["sonic"]}}},
            "archives.sha256": {"archives_sha256": "e" * 64},
            "recipe.blob": {"recipe": "f" * 40},
            "toolchain.cmake": {"toolchain": {
                key.split(".", 1)[1]: ("9" if key == "toolchain.cmake" else value)
                for key, value in wanted.items() if key.startswith("toolchain.")}},
        }.items():
            with self.subTest(label):
                shutil.rmtree(self.root / "artifact", ignore_errors=True)
                artifact = make_artifact(self.root, self.tree, **changes)
                self.refused(artifact, f"{label}: the tree has", "--accept")

    def test_a_patch_series_that_is_not_the_built_tree_is_refused(self) -> None:
        artifact = make_artifact(self.root, self.tree, patch_series={"talkback": "9" * 40, "espeak-ng": TREES["espeak-ng"]})
        self.refused(artifact, "records a patch series that is not the tree it was built from", "--accept")

    # -- the descriptor ----------------------------------------------------

    def test_nothing_is_staged_before_an_artifact_is_accepted(self) -> None:
        artifact = make_artifact(self.root, self.tree)
        descriptor = self.tree / identity.DESCRIPTOR
        self.refused(artifact, "no accepted accessibility artifact:")
        descriptor.write_text(f"{identity.DESCRIPTOR_PREFIX}STATE='pending'\n")
        self.refused(artifact, "no accepted accessibility artifact yet")
        self.refused(artifact, "no accepted accessibility artifact yet", "--aosp-source", str(self.aosp))
        self.assertFalse((self.aosp / "vendor").exists())

    def test_another_artifact_than_the_accepted_one_is_refused(self) -> None:
        self.accepted()
        shutil.rmtree(self.root / "artifact")
        other = make_artifact(self.root, self.tree, extra="a field that changes artifact.json's bytes")
        self.refused(other, "is not the accepted accessibility artifact")

    def test_an_edited_descriptor_is_refused(self) -> None:
        artifact = self.accepted()
        descriptor = self.tree / identity.DESCRIPTOR
        text = descriptor.read_text()
        key = identity.env_name("archives.sha256")
        descriptor.write_text(text.replace(f"{key}='", f"{key}='0"))
        self.refused(artifact, "differs from this artifact at ARCHIVES_SHA256")

    def test_acceptance_writes_the_descriptor_the_release_compares(self) -> None:
        artifact = self.accepted()
        values = identity.read_descriptor(self.tree / identity.DESCRIPTOR)
        self.assertEqual(values[identity.env_name("artifact.sha256")], identity.sha256_file(artifact / "artifact.json"))
        self.assertEqual(identity.descriptor_differences(values, identity.expected(self.tree)), [])
        (self.tree / "dependencies/andarium-accessibility/archives.tsv").write_text("name\turl\tsha256\n")
        names = [name for name, _, _ in identity.descriptor_differences(values, identity.expected(self.tree))]
        self.assertEqual(names, ["archives.sha256"])

    def test_the_tracked_descriptor_names_no_artifact_before_one_is_accepted(self) -> None:
        values = identity.read_descriptor(ROOT / identity.DESCRIPTOR)
        self.assertEqual(values[identity.DESCRIPTOR_PREFIX + "STATE"] in ("pending", "accepted"), True)
        if values[identity.DESCRIPTOR_PREFIX + "STATE"] == "accepted":
            self.assertEqual(identity.descriptor_differences(values, identity.expected(ROOT)), [])

    def test_a_workspace_without_repo_metadata_is_refused(self) -> None:
        artifact = self.accepted()
        self.refused(artifact, "AOSP Repo workspace", "--aosp-source", str(self.root))


if __name__ == "__main__":
    unittest.main(verbosity=2)
