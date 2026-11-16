#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The identity of the accessibility artifact, and the checks that bind it.

The artifact (TalkBack.apk, eSpeakNG.apk and artifact.json) is built from the
tracked pins, patch series, archive list, builder and toolchain list. This
module computes what those tracked inputs say the artifact must have been
built from (`expected`), reads the toolchain the build host has and refuses
any difference from toolchain.tsv (`observe_toolchain`), links a signed APK
back to the unsigned one (`entries_sha256`), and scans a payload for values of
the machine that built it (`scan_payload`). It makes no network call. The
builder writes the observed identity into artifact.json, the stager compares
it with `expected`, and scripts/check-embedded-artifacts.py compares the
accepted descriptor with it, so a moved pin, series, archive list, builder or
toolchain stops an old artifact at each of them.

    identity.py expected [--root DIR]
    identity.py tracked ITEM [--root DIR]
    identity.py toolchain --java-home DIR --android-home DIR [--root DIR]
    identity.py archives --require NAME... [--root DIR]
    identity.py describe --root DIR --sources DIR --apks DIR ...
    identity.py scan [--root-path DIR] [--forbid TEXT] APK...
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import getpass
import hashlib
import json
import os
import re
import subprocess
import sys
import zipfile
from pathlib import Path

DIRECTORY = "dependencies/andarium-accessibility"
MANIFEST = "dependencies/manifest.tsv"
BUILDER = "dependencies/build-andarium-accessibility.sh"
DESCRIPTOR = "android/lineage-24/metadata/accepted-accessibility.env"
SOURCES = ("talkback", "espeak-ng", "sonic")
# The sources that carry a patch series; sonic is built as pinned.
PATCHED = ("talkback", "espeak-ng")
# Each APK, its package and the source it is built from.
APKS = {
    "TalkBack.apk": "com.android.talkback",
    "eSpeakNG.apk": "com.reecedunn.espeak",
}
SCHEMA = 2
DESCRIPTOR_PREFIX = "LINEAGE24_ACCEPTED_ACCESSIBILITY_"
# Values of the build machine that no payload may hold. The first three are
# the macOS and temporary-file roots; each is matched where a path begins.
FORBIDDEN_ROOTS = ("/Users/", "/Volumes/", "/private/", "/tmp/", "/var/folders/")
FORBIDDEN_TEXT = (".build/dependency-build",)
HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")


class IdentityError(Exception):
    pass


def rows(path: Path) -> list[list[str]]:
    """The tab-separated rows of a tracked list, without comments."""
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise IdentityError(f"cannot read {path}: {error}") from error
    return [line.split("\t") for line in text.splitlines() if line and not line.startswith("#")]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def blob_id(path: Path) -> str:
    """The Git blob ID of a file, computed without Git."""
    content = path.read_bytes()
    return hashlib.sha1(b"blob %d\0" % len(content) + content).hexdigest()


def manifest_pins(root: Path) -> dict[str, str]:
    pins = {}
    for row in rows(root / MANIFEST)[1:]:
        if len(row) >= 5 and row[0] in SOURCES:
            pins[row[0]] = row[4]
    missing = [name for name in SOURCES if not HEX40.fullmatch(pins.get(name, ""))]
    if missing:
        raise IdentityError(f"{MANIFEST} has no pinned commit for {', '.join(missing)}")
    return pins


def series_tree(root: Path, name: str) -> str:
    for row in rows(root / "dependencies/patches" / name / "series.tsv"):
        if row[0] == "tree" and len(row) == 2 and HEX40.fullmatch(row[1]):
            return row[1]
    raise IdentityError(f"dependencies/patches/{name}/series.tsv records no tree")


def tracked_toolchain(root: Path) -> dict[str, str]:
    found = {}
    for row in rows(root / DIRECTORY / "toolchain.tsv"):
        if len(row) != 2 or not row[0] or not row[1]:
            raise IdentityError(f"{DIRECTORY}/toolchain.tsv: a row is item and value")
        if row[0] in found:
            raise IdentityError(f"{DIRECTORY}/toolchain.tsv: {row[0]} appears twice")
        found[row[0]] = row[1]
    return found


def expected(root: Path) -> dict[str, str]:
    """What the tracked inputs say a built artifact must record, by name."""
    pins = manifest_pins(root)
    flat = {}
    for name in SOURCES:
        flat[f"source.{name}.pin"] = pins[name]
        if name in PATCHED:
            flat[f"source.{name}.tree"] = series_tree(root, name)
        else:
            flat[f"source.{name}.head"] = pins[name]
    flat["archives.sha256"] = sha256_file(root / DIRECTORY / "archives.tsv")
    flat["recipe.blob"] = blob_id(root / BUILDER)
    for item, value in sorted(tracked_toolchain(root).items()):
        flat[f"toolchain.{item}"] = value
    return flat


def recorded(description: dict) -> dict[str, str]:
    """The same names, from what an artifact.json says it was built from."""
    flat = {}
    sources = description.get("sources")
    toolchain = description.get("toolchain")
    if not isinstance(sources, dict) or not isinstance(toolchain, dict):
        raise IdentityError("artifact.json records no sources and toolchain")
    for name in SOURCES:
        entry = sources.get(name)
        if not isinstance(entry, dict):
            raise IdentityError(f"artifact.json records no source {name}")
        flat[f"source.{name}.pin"] = str(entry.get("pin"))
        key = "tree" if name in PATCHED else "head"
        flat[f"source.{name}.{key}"] = str(entry.get(key))
    flat["archives.sha256"] = str(description.get("archives_sha256"))
    flat["recipe.blob"] = str(description.get("recipe"))
    for item, value in sorted(toolchain.items()):
        flat[f"toolchain.{item}"] = str(value)
    return flat


def differences(wanted: dict[str, str], found: dict[str, str]) -> list[tuple[str, str, str]]:
    """(name, wanted, found) for each name the two sides do not share."""
    return [
        (name, wanted.get(name, "(none)"), found.get(name, "(none)"))
        for name in sorted(set(wanted) | set(found))
        if wanted.get(name) != found.get(name)
    ]


# ------------------------------------------------------------- toolchain --


def properties(path: Path) -> dict[str, str]:
    values = {}
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return values
    for line in text.splitlines():
        key, separator, value = line.partition("=")
        if separator:
            values[key.strip()] = value.strip()
    return values


def observe_toolchain(java_home: Path, android_home: Path, tracked: dict[str, str]) -> tuple[dict[str, str], list[str]]:
    """The toolchain the build host has, and what differs from `tracked`.

    Each SDK component is read from the directory the tracked value names and
    from its own source.properties, so a directory renamed to another
    revision, or a newer neighbour, is never taken for it.
    """
    observed: dict[str, str] = {}
    problems: list[str] = []

    def look(item: str, found: str | None, missing: str) -> None:
        if found is None:
            problems.append(missing)
            return
        observed[item] = found
        if item in tracked and tracked[item] != found:
            problems.append(f"{item} is {found}, the tree pins {tracked[item]}")

    java = java_home / "bin/java"
    runtime = None
    try:
        result = subprocess.run(
            [str(java), "-XshowSettings:properties", "-version"], stdin=subprocess.DEVNULL,
            capture_output=True, text=True, timeout=60, check=False)
        match = re.search(r"^\s*java\.runtime\.version = (\S+)", result.stderr + result.stdout, re.M)
        runtime = match.group(1) if match else None
    except (OSError, subprocess.SubprocessError):
        pass
    look("jdk", runtime, "JAVA_HOME does not name a JDK that reports its runtime version")

    ndk = tracked.get("ndk", "")
    revision = properties(android_home / "ndk" / ndk / "source.properties").get("Pkg.Revision") if ndk else None
    look("ndk", revision, f"the SDK has no NDK {ndk}")
    tools = tracked.get("build-tools", "")
    revision = properties(android_home / "build-tools" / tools / "source.properties").get("Pkg.Revision") if tools else None
    look("build-tools", revision, f"the SDK has no build-tools {tools}")
    if tools and revision is not None and not os.access(android_home / "build-tools" / tools / "aapt2", os.X_OK):
        problems.append(f"build-tools {tools} has no aapt2")
    api = tracked.get("platform-api", "")
    platform = properties(android_home / "platforms" / f"android-{api}" / "source.properties") if api else {}
    look("platform-api", platform.get("AndroidVersion.ApiLevel"), f"the SDK has no platform android-{api}")
    look("platform-revision", platform.get("Pkg.Revision"), f"the SDK has no revision of platform android-{api}")
    cmake = tracked.get("cmake", "")
    revision = properties(android_home / "cmake" / cmake / "source.properties").get("Pkg.Revision") if cmake else None
    look("cmake", revision, f"the SDK has no CMake {cmake}")
    for item in tracked:
        if item not in observed and not any(item in problem for problem in problems):
            problems.append(f"the tree pins {item}, which nothing here reads")
    return observed, problems


# ------------------------------------------------------------------- apk --


def entries_sha256(apk: Path) -> str:
    """SHA-256 of the sorted `name crc32 size` lines of the non-META-INF
    entries: what a signature leaves alone, so a signed APK links back."""
    lines = []
    with zipfile.ZipFile(apk) as archive:
        for info in archive.infolist():
            if info.is_dir() or info.filename.startswith("META-INF/"):
                continue
            lines.append(f"{info.filename} {info.CRC:08x} {info.file_size}")
    return hashlib.sha256(("\n".join(sorted(lines)) + "\n").encode()).hexdigest()


def package_problem(apk: Path, package: str) -> str | None:
    """Why the file is not an APK of `package`, or None."""
    try:
        with zipfile.ZipFile(apk) as archive:
            names = set(archive.namelist())
            if "AndroidManifest.xml" not in names:
                return "has no AndroidManifest.xml"
            if not any(re.fullmatch(r"classes\d*\.dex", name) for name in names):
                return "has no classes.dex"
            manifest = archive.read("AndroidManifest.xml")
    except (OSError, zipfile.BadZipFile) as error:
        return f"is not a zip archive ({error})"
    if package.encode("utf-16-le") not in manifest and package.encode() not in manifest:
        return f"does not name package {package} in its manifest"
    return None


PATH_CHARACTERS = frozenset(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-")


def occurs(content: bytes, text: str, anchored: bool) -> str | None:
    """`text` in `content` as UTF-8 or UTF-16, where an anchored text must not
    continue a longer path."""
    for encoding, width in (("utf-8", 1), ("utf-16-le", 2)):
        needle = text.encode(encoding)
        start = 0
        while True:
            index = content.find(needle, start)
            if index < 0:
                break
            before = content[max(index - width, 0):index]
            if width == 2 and len(before) == 2 and before[1] == 0:
                before = before[:1]
            elif width == 2 and before:
                before = b"\xff"
            if not anchored or not before or before[0] not in PATH_CHARACTERS:
                return text if width == 1 else f"{text} (UTF-16)"
            start = index + 1
    return None


def scan_payload(apk: Path, forbidden: tuple[str, ...] = (), user: str | None = None) -> list[str]:
    """Where the file holds a value of the machine that built it.

    Entry names and the bytes of every entry (DEX strings, resources and the
    strings of each native library) are searched as UTF-8 and UTF-16. A root
    matches where a path begins; the other texts match anywhere.
    """
    needles = [(root, True) for root in FORBIDDEN_ROOTS]
    needles += [(text, False) for text in (*FORBIDDEN_TEXT, *forbidden) if text]
    if user and len(user) >= 3:
        needles.append((f"/{user}/", False))
    findings = []
    with zipfile.ZipFile(apk) as archive:
        for info in archive.infolist():
            name = info.filename.encode()
            for text, anchored in needles:
                if occurs(name, text, anchored):
                    findings.append(f"entry name {info.filename}")
            if info.is_dir():
                continue
            content = archive.read(info.filename)
            for text, anchored in needles:
                found = occurs(content, text, anchored)
                if found:
                    findings.append(f"{info.filename}: {found}")
    return sorted(set(findings))


# -------------------------------------------------------------- artifact --


def describe(root: Path, sources: Path, apks: Path, toolchain: dict[str, str], archives: Path | None = None) -> dict:
    """artifact.json for the APKs in `apks`, built from the prepared
    `sources`."""
    pins = manifest_pins(root)

    def git(name: str, *arguments: str) -> str:
        return subprocess.run(
            ["git", "-C", str(sources / name), *arguments], check=True, capture_output=True, text=True,
        ).stdout.strip()

    description: dict = {"schema": SCHEMA, "apks": {}, "sources": {}, "patch_series": {}, "toolchain": toolchain}
    for name, package in APKS.items():
        path = apks / name
        description["apks"][name] = {
            "package": package, "sha256": sha256_file(path), "entries_sha256": entries_sha256(path)}
    for name in SOURCES:
        entry = {"pin": pins[name], "head": git(name, "rev-parse", "HEAD")}
        if name in PATCHED:
            entry["tree"] = git(name, "rev-parse", "HEAD^{tree}")
            description["patch_series"][name] = series_tree(root, name)
        description["sources"][name] = entry
    description["archives_sha256"] = sha256_file(archives or root / DIRECTORY / "archives.tsv")
    description["recipe"] = blob_id(root / BUILDER)
    return description


# ------------------------------------------------------------ descriptor --


def env_name(key: str) -> str:
    return DESCRIPTOR_PREFIX + re.sub(r"[^A-Za-z0-9]", "_", key).upper()


def read_descriptor(path: Path) -> dict[str, str]:
    values = {}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw.strip() or raw.startswith("#"):
            continue
        match = re.fullmatch(r"([A-Z][A-Z0-9_]*)=(?:'([^']*)'|(\S*))", raw)
        if not match:
            raise IdentityError(f"{path}:{number}: not KEY=value")
        values[match.group(1)] = match.group(2) if match.group(2) is not None else match.group(3)
    return values


def descriptor_fields(description: dict, artifact_sha256: str) -> dict[str, str]:
    """The accepted descriptor's fields, by environment name, for a checked
    artifact.json."""
    fields = {
        DESCRIPTOR_PREFIX + "SCHEMA": "1",
        DESCRIPTOR_PREFIX + "STATE": "accepted",
        DESCRIPTOR_PREFIX + "ARTIFACT_SHA256": artifact_sha256,
    }
    for name, entry in sorted(description["apks"].items()):
        fields[env_name(f"apk.{name}.sha256")] = entry["sha256"]
        fields[env_name(f"apk.{name}.entries")] = entry["entries_sha256"]
    for key, value in recorded(description).items():
        fields[env_name(key)] = value
    return fields


def render_descriptor(fields: dict[str, str]) -> str:
    lines = [
        "# SPDX-License-Identifier: GPL-2.0-or-later",
        "# The accessibility artifact the LineageOS product carries, as accepted:",
        "# written by stage-accessibility-artifact.py --accept from an artifact that",
        "# matched the tracked inputs. A release input.",
    ]
    lines += [f"{name}='{value}'" for name, value in fields.items()]
    return "\n".join(lines) + "\n"


def descriptor_differences(values: dict[str, str], wanted: dict[str, str]) -> list[tuple[str, str, str]]:
    """How an accepted descriptor's identity fields differ from `wanted`
    (the names of `expected`)."""
    mapped = {env_name(key): key for key in wanted}
    found = {mapped.get(name, name[len(DESCRIPTOR_PREFIX):]): value for name, value in values.items()
             if name in mapped or re.match(DESCRIPTOR_PREFIX + "(SOURCE|ARCHIVES|RECIPE|TOOLCHAIN)_", name)}
    return differences(wanted, found)


# ------------------------------------------------------------------- cli --


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("expected")
    tracked_command = commands.add_parser("tracked")
    tracked_command.add_argument("item")
    toolchain_command = commands.add_parser("toolchain")
    toolchain_command.add_argument("--java-home", type=Path, required=True)
    toolchain_command.add_argument("--android-home", type=Path, required=True)
    toolchain_command.add_argument("--output", type=Path)
    archives_command = commands.add_parser("archives")
    archives_command.add_argument("--require", action="append", default=[])
    scan_command = commands.add_parser("scan")
    scan_command.add_argument("--forbid", action="append", default=[])
    scan_command.add_argument("apk", nargs="+", type=Path)
    describe_command = commands.add_parser("describe")
    describe_command.add_argument("--sources", type=Path, required=True)
    describe_command.add_argument("--apks", type=Path, required=True)
    describe_command.add_argument("--toolchain", type=Path, required=True)
    describe_command.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    root = arguments.root
    try:
        if arguments.command == "expected":
            print(json.dumps(expected(root), indent=2, sort_keys=True))
        elif arguments.command == "tracked":
            tracked = tracked_toolchain(root)
            if arguments.item not in tracked:
                raise IdentityError(f"{DIRECTORY}/toolchain.tsv pins no {arguments.item}")
            print(tracked[arguments.item])
        elif arguments.command == "toolchain":
            observed, problems = observe_toolchain(arguments.java_home, arguments.android_home, tracked_toolchain(root))
            if problems:
                for problem in problems:
                    print(f"toolchain: {problem}", file=sys.stderr)
                return 1
            if arguments.output:
                arguments.output.write_text(json.dumps(observed, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        elif arguments.command == "archives":
            names = {row[0] for row in rows(root / DIRECTORY / "archives.tsv")[1:]}
            absent = [name for name in arguments.require if name not in names]
            if absent:
                print(f"{DIRECTORY}/archives.tsv has no row for {', '.join(absent)}", file=sys.stderr)
                return 1
        elif arguments.command == "scan":
            failed = False
            for apk in arguments.apk:
                for finding in scan_payload(apk, tuple(arguments.forbid), getpass.getuser()):
                    print(f"{apk.name} holds a value of the build machine: {finding}", file=sys.stderr)
                    failed = True
            return 1 if failed else 0
        elif arguments.command == "describe":
            toolchain = json.loads(arguments.toolchain.read_text(encoding="utf-8"))
            description = describe(root, arguments.sources, arguments.apks, toolchain)
            arguments.output.write_text(json.dumps(description, indent=2) + "\n", encoding="utf-8")
    except IdentityError as error:
        print(f"identity.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
