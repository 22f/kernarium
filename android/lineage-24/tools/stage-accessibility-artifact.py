#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stage the accessibility artifact in the AOSP workspace for the product.

The artifact is what dependencies/build-andarium-accessibility.sh writes on the
Mac: TalkBack.apk and eSpeakNG.apk, unsigned, and artifact.json naming each
APK and what they were built from. The tool accepts an artifact only when

  - every file matches artifact.json and each APK is an APK of its package,
    holding no value of the machine that built it;
  - what artifact.json says it was built from (the pinned commits, the patch
    series trees, the archive list, the builder and the toolchain) is what this
    tree tracks (dependencies/andarium-accessibility/identity.py); and
  - artifact.json is the one android/lineage-24/metadata/accepted-
    accessibility.env names.

--accept writes that descriptor from an artifact that passes the first two.
Staging writes vendor/kernarium/accessibility: the two APKs and the Android.bp
of the filegroups that overlay/accessibility/Android.bp imports. A file whose
bytes did not change keeps its time, so an unchanged artifact rebuilds
nothing. Anything else the directory held is removed.

    stage-accessibility-artifact.py --artifact DIR --aosp-source DIR
    stage-accessibility-artifact.py --artifact DIR --check
    stage-accessibility-artifact.py --artifact DIR --accept
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import json
import re
import shutil
import sys
from pathlib import Path, PurePosixPath

DESTINATION = PurePosixPath("vendor/kernarium/accessibility")
# The filegroup that names each of the artifact's APKs.
GROUPS = {
    "TalkBack.apk": "kernarium_accessibility_talkback",
    "eSpeakNG.apk": "kernarium_accessibility_espeak_ng",
}


def fail(message: str) -> None:
    raise SystemExit(f"stage-accessibility-artifact.py: {message}")


def load_identity(root: Path):
    path = root / "dependencies/andarium-accessibility/identity.py"
    if not path.is_file():
        fail(f"{path} is missing; --repo-root must be a source tree")
    specification = importlib.util.spec_from_file_location("accessibility_identity", path)
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


def checked(identity, root: Path, artifact: Path) -> tuple[dict, str]:
    """The artifact's description and artifact.json's SHA-256, once every
    file matches it and it matches the tree."""
    if not artifact.is_absolute() or not artifact.is_dir() or artifact.is_symlink():
        fail(f"the artifact must be an absolute, regular directory: {artifact}")
    description_path = artifact / "artifact.json"
    try:
        description = json.loads(description_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        fail(f"cannot read artifact.json: {error}")
    if not isinstance(description, dict) or description.get("schema") != identity.SCHEMA:
        fail(f"artifact.json is not schema {identity.SCHEMA}")
    apks = description.get("apks")
    if not isinstance(apks, dict) or set(apks) != set(identity.APKS):
        fail(f"artifact.json must describe exactly {', '.join(sorted(identity.APKS))}")
    for name, package in identity.APKS.items():
        entry = apks[name]
        path = artifact / name
        if not isinstance(entry, dict) or entry.get("package") != package:
            fail(f"{name} must be package {package}")
        for field in ("sha256", "entries_sha256"):
            if not re.fullmatch(r"[0-9a-f]{64}", str(entry.get(field))):
                fail(f"{name} has no {field} in artifact.json")
        if not path.is_file() or path.is_symlink():
            fail(f"{name} is missing from the artifact")
        if identity.sha256_file(path) != entry["sha256"]:
            fail(f"{name} differs from artifact.json")
        problem = identity.package_problem(path, package)
        if problem:
            fail(f"{name} {problem}")
        if identity.entries_sha256(path) != entry["entries_sha256"]:
            fail(f"{name}'s entries differ from artifact.json")
        findings = identity.scan_payload(path)
        if findings:
            fail(f"{name} holds a value of the machine that built it: {findings[0]}")
    try:
        found = identity.recorded(description)
        wanted = identity.expected(root)
    except identity.IdentityError as error:
        fail(str(error))
    differing = identity.differences(wanted, found)
    if differing:
        fail("the artifact was not built from this tree:\n" + "\n".join(
            f"  {name}: the tree has {tracked}, the artifact records {built}"
            for name, tracked, built in differing))
    series = description.get("patch_series")
    if not isinstance(series, dict) or any(
        series.get(name) != found.get(f"source.{name}.tree") for name in identity.PATCHED
    ):
        fail("artifact.json records a patch series that is not the tree it was built from")
    return description, identity.sha256_file(description_path)


def accepted(identity, root: Path, description: dict, digest: str) -> None:
    """Refuses unless the tracked descriptor accepts exactly this artifact."""
    path = root / identity.DESCRIPTOR
    try:
        values = identity.read_descriptor(path)
    except (OSError, identity.IdentityError) as error:
        fail(f"no accepted accessibility artifact: {error}")
    state = values.get(identity.DESCRIPTOR_PREFIX + "STATE")
    if state != "accepted":
        fail(f"no accepted accessibility artifact yet ({identity.DESCRIPTOR} is {state}); "
             "build one and accept it with --accept")
    if values.get(identity.DESCRIPTOR_PREFIX + "ARTIFACT_SHA256") != digest:
        fail("artifact.json is not the accepted accessibility artifact "
             f"(SHA-256 {digest}, accepted {values.get(identity.DESCRIPTOR_PREFIX + 'ARTIFACT_SHA256')})")
    fields = identity.descriptor_fields(description, digest)
    if values != fields:
        names = sorted(name[len(identity.DESCRIPTOR_PREFIX):]
                       for name in set(values) | set(fields) if values.get(name) != fields.get(name))
        fail(f"{identity.DESCRIPTOR} differs from this artifact at {', '.join(names)}")


def blueprint() -> bytes:
    groups = "".join(
        f'\nfilegroup {{\n    name: "{group}",\n    srcs: ["{name}"],\n}}\n' for name, group in sorted(GROUPS.items())
    )
    return (
        "// Generated by android/lineage-24/tools/stage-accessibility-artifact.py from the\n"
        "// accessibility artifact; overlay/accessibility/Android.bp imports these.\n" + groups
    ).encode()


def write(path: Path, content: bytes) -> None:
    """Writes `content` unless `path` already holds it, which keeps its time."""
    if path.is_file() and not path.is_symlink() and path.read_bytes() == content:
        return
    temporary = path.with_name(f".{path.name}.partial")
    temporary.write_bytes(content)
    temporary.replace(path)


def stage(identity, artifact: Path, aosp_source: Path) -> None:
    if not aosp_source.is_absolute() or not (aosp_source / ".repo").is_dir():
        fail(f"--aosp-source must be an absolute AOSP Repo workspace: {aosp_source}")
    destination = aosp_source / DESTINATION
    if destination.is_symlink():
        fail(f"{DESTINATION} must not be a symbolic link")
    destination.mkdir(parents=True, exist_ok=True)
    wanted = {"Android.bp": blueprint(), "artifact.json": (artifact / "artifact.json").read_bytes()}
    for name in identity.APKS:
        wanted[name] = (artifact / name).read_bytes()
    for entry in destination.iterdir():
        if entry.name not in wanted:
            shutil.rmtree(entry) if entry.is_dir() and not entry.is_symlink() else entry.unlink()
    for name, content in wanted.items():
        write(destination / name, content)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--aosp-source", type=Path)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3],
                        help="the source tree whose tracked inputs the artifact must match")
    parser.add_argument("--check", action="store_true", help="check the artifact and stage nothing")
    parser.add_argument("--accept", action="store_true",
                        help="record a checked artifact in the tracked descriptor")
    args = parser.parse_args()
    if [args.check, args.accept, args.aosp_source is not None].count(True) != 1:
        fail("give one of --aosp-source, --check or --accept")
    identity = load_identity(args.repo_root.resolve())
    description, digest = checked(identity, args.repo_root.resolve(), args.artifact)
    if args.accept:
        path = args.repo_root.resolve() / identity.DESCRIPTOR
        path.write_text(identity.render_descriptor(identity.descriptor_fields(description, digest)), encoding="utf-8")
        print(f"accepted the accessibility artifact in {identity.DESCRIPTOR}")
        return 0
    accepted(identity, args.repo_root.resolve(), description, digest)
    if args.check:
        print(f"accessibility artifact checks: {', '.join(sorted(description['apks']))}")
        return 0
    stage(identity, args.artifact, args.aosp_source)
    print(f"staged the accessibility artifact in {DESTINATION}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
