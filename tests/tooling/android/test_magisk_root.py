#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Magisk root files of an Android release.

A release can carry, beside its system disk, a copy of its own init_boot that
Magisk's patcher made and the official Magisk APK it was made from, both from the
one release pinned in android/lineage-24/metadata/magisk.env. These tests use a
synthetic cpio, a synthetic APK and stand-in tools (magiskboot, which is this
file in its --fake-magiskboot mode, and a boot_patch.sh that calls it as Magisk's
does): no Magisk file is in the repository.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import importlib.util
import json
import lzma
import os
import re
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / "android/lineage-24/tools"
THIS = Path(__file__).resolve()
PIN_KEYS = {
    "LINEAGE24_MAGISK_SCHEMA",
    "LINEAGE24_MAGISK_NAME",
    "LINEAGE24_MAGISK_VERSION",
    "LINEAGE24_MAGISK_VERSION_CODE",
    "LINEAGE24_MAGISK_APK_SHA256",
    "LINEAGE24_MAGISK_APK_SIZE",
}
TEST_VERSION = "v99.1"
TEST_VERSION_CODE = 99100


# ------------------------------------------------ synthetic formats (the test's) --


def pad4(length: int) -> bytes:
    return b"\0" * (-length % 4)


def newc_dump(entries: dict, *, order: list[str] | None = None, mtime: int = 0, tail: int = 0) -> bytes:
    """A newc archive of {name: Entry}, in Magisk's way (sorted, inodes from 300001,
    link count 1) unless `order` names the order."""
    out = bytearray()
    names = order if order is not None else sorted(entries)
    for number, name in enumerate(names, 300001):
        entry = entries[name]
        raw = name.encode() + b"\0"
        fields = (number, entry.mode, entry.uid, entry.gid, 1, mtime, len(entry.payload),
                  0, 0, entry.rdev[0], entry.rdev[1], len(raw), 0)
        out += b"070701" + b"".join(b"%08X" % value for value in fields)
        out += raw + pad4(110 + len(raw)) + entry.payload + pad4(len(entry.payload))
    raw = b"TRAILER!!!\0"
    out += b"070701" + b"".join(b"%08X" % value for value in (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(raw), 0))
    out += raw + pad4(110 + len(raw))
    return bytes(out) + b"\0" * tail


def lz4_literals(data: bytes) -> bytes:
    """An LZ4 block of literals only."""
    count = len(data)
    out = bytearray([min(count, 15) << 4])
    if count >= 15:
        remaining = count - 15
        while remaining >= 255:
            out.append(255)
            remaining -= 255
        out.append(remaining)
    return bytes(out) + data


def lz4_matches(data: bytes) -> bytes:
    """An LZ4 block with matches, by a naive greedy search: for small inputs, to
    exercise the decoder's overlapping copies and extended lengths."""
    out = bytearray()

    def extension(count: int) -> None:
        count -= 15
        while count >= 255:
            out.append(255)
            count -= 255
        out.append(count)

    def emit(literals: bytes, offset: int, length: int) -> None:
        match = length - 4 if length else 0
        out.append(min(len(literals), 15) << 4 | min(match, 15))
        if len(literals) >= 15:
            extension(len(literals))
        out.extend(literals)
        if length:
            out.extend(struct.pack("<H", offset))
            if match >= 15:
                extension(match)

    position = 0
    literal_start = 0
    while position + 4 <= len(data):
        best = (0, 0)
        for start in range(max(0, position - 65535), position):
            length = 0
            while position + length < len(data) and data[start + length] == data[position + length]:
                length += 1
            if length > best[0]:
                best = (length, position - start)
        if best[0] >= 4:
            emit(data[literal_start:position], best[1], best[0])
            position += best[0]
            literal_start = position
        else:
            position += 1
    emit(data[literal_start:], 0, 0)
    return bytes(out)


def lz4_legacy(data: bytes) -> bytes:
    out = bytearray(b"\x02\x21\x4c\x18")
    for start in range(0, len(data), 8 * 1024 * 1024):
        block = lz4_literals(data[start:start + 8 * 1024 * 1024])
        out += struct.pack("<I", len(block)) + block
    return bytes(out)


def boot_image(ramdisk: bytes, size: int = 0, os_version: int = 0x1234) -> bytes:
    header = bytearray(4096)
    header[0:8] = b"ANDROID!"
    struct.pack_into("<IIII", header, 8, 0, len(ramdisk), os_version, 1584)
    struct.pack_into("<I", header, 40, 4)
    image = bytes(header) + ramdisk + b"\0" * (-len(ramdisk) % 4096)
    return image + b"\0" * max(0, size - len(image))


def make_apk_bytes(path: Path, members: dict[str, bytes]) -> None:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, payload in members.items():
            archive.writestr(name, payload)


def pseudo_random(label: str, size: int) -> bytes:
    out = bytearray()
    counter = 0
    while len(out) < size:
        out += hashlib.sha256(f"{label}{counter}".encode()).digest()
        counter += 1
    return bytes(out[:size])


# ------------------------------------------------------ the stand-in magiskboot --


def fake_magiskboot(argv: list[str]) -> int:
    """The few magiskboot commands boot_patch.sh and magisk-boot.py use, on the
    synthetic images. The first argument is the tools directory; options before the
    command: --nondeterministic (the cpio carries a random time) and --raw-ramdisk
    (repack does not compress)."""
    sys.path.insert(0, argv.pop(0))
    import magisk_image  # noqa: PLC0415

    options: list[str] = []
    while argv and argv[0].startswith("--"):
        options.append(argv.pop(0))
    command, *rest = argv

    def error(text: str) -> SystemExit:
        return SystemExit(text)

    def load_cpio(path: str) -> dict:
        return magisk_image.parse_newc(Path(path).read_bytes(), error)

    if command == "unpack":
        _, ramdisk = magisk_image.boot_ramdisk(Path(rest[0]).read_bytes(), "image", error)
        Path("ramdisk.cpio").write_bytes(ramdisk)
        print("RAMDISK_FMT      [raw]")
        return 0
    if command == "sha1":
        print(hashlib.sha1(Path(rest[0]).read_bytes()).hexdigest())
        return 0
    if command == "compress=xz":
        Path(rest[1]).write_bytes(lzma.compress(Path(rest[0]).read_bytes(), format=lzma.FORMAT_XZ))
        return 0
    if command == "cpio":
        path = rest[0]
        entries = load_cpio(path)
        for text in rest[1:]:
            words = text.split(" ")
            if words[0] == "add":
                entries[words[2]] = magisk_image.Entry(
                    stat.S_IFREG | int(words[1], 8), 0, 0, (0, 0), Path(words[3]).read_bytes())
            elif words[0] == "mkdir":
                entries[words[2]] = magisk_image.Entry(stat.S_IFDIR | int(words[1], 8), 0, 0, (0, 0), b"")
            elif words[0] == "patch":
                pass
            elif words[0] == "backup":
                original = load_cpio(words[1])
                removals = sorted(name for name in entries if name not in original)
                for name, entry in original.items():
                    if name in entries and entries[name] != entry and stat.S_ISREG(entry.mode):
                        entries[f".backup/{name}.xz"] = magisk_image.Entry(
                            entry.mode, 0, 0, (0, 0), lzma.compress(entry.payload, format=lzma.FORMAT_XZ))
                entries[".backup/.rmlist"] = magisk_image.Entry(
                    stat.S_IFREG, 0, 0, (0, 0), b"".join(name.encode() + b"\0" for name in removals))
            else:
                print(f"! unknown cpio command {text}", file=sys.stderr)
                return 1
        mtime = int.from_bytes(os.urandom(3), "big") if "--nondeterministic" in options else 0
        Path(path).write_bytes(newc_dump(entries, mtime=mtime))
        return 0
    if command == "repack":
        original = Path(rest[0]).read_bytes()
        out = rest[1] if len(rest) > 1 else "new-boot.img"
        cpio = Path("ramdisk.cpio").read_bytes()
        ramdisk = cpio if "--raw-ramdisk" in options else lz4_legacy(cpio)
        header = bytearray(original[:4096])
        struct.pack_into("<I", header, 12, len(ramdisk))
        image = bytes(header) + ramdisk + b"\0" * (-len(ramdisk) % 4096)
        Path(out).write_bytes(image + b"\0" * max(0, len(original) - len(image)))
        return 0
    print(f"! unknown command {command}", file=sys.stderr)
    return 1


# The script follows Magisk's boot_patch.sh in its commands and their order. The
# real one asks the guest's magisk for the pre-init partition, which cannot run on
# the builder: this one leaves PREINITDEVICE out unless told to write it.
PATCH_SCRIPT = r"""#!/system/bin/sh
cd "$(dirname "$0")"
BOOTIMAGE="$1"
./magiskboot unpack "$BOOTIMAGE" || exit 1
cp -af ramdisk.cpio ramdisk.cpio.orig
SHA1=$(./magiskboot sha1 "$BOOTIMAGE")
echo "KEEPVERITY=$KEEPVERITY" > config
echo "KEEPFORCEENCRYPT=$KEEPFORCEENCRYPT" >> config
echo "RECOVERYMODE=$RECOVERYMODE" >> config
echo "VENDORBOOT=${VENDORBOOT:-false}" >> config
@PREINIT@
echo "SHA1=$SHA1" >> config
./magiskboot compress=xz magisk magisk.xz
./magiskboot compress=xz init-ld init-ld.xz
./magiskboot cpio ramdisk.cpio \
@ADD_INIT@
"mkdir 0750 overlay.d" \
"mkdir 0750 overlay.d/sbin" \
"add 0644 overlay.d/sbin/magisk.xz magisk.xz" \
@ADD_INIT_LD@
"patch" \
"backup ramdisk.cpio.orig" \
"mkdir 000 .backup" \
"add 000 .backup/.magisk config"
./magiskboot repack "$BOOTIMAGE"
@EXIT@
"""


def patch_script(*, preinit: bool = False, add_init: bool = True, add_init_ld: bool = True,
                 exit_status: int = 0, hang: bool = False) -> bytes:
    text = PATCH_SCRIPT
    text = text.replace("@PREINIT@", 'echo "PREINITDEVICE=$PREINITDEVICE" >> config' if preinit else ":")
    text = text.replace("@ADD_INIT@", '"add 0750 init magiskinit" \\' if add_init else '"patch" \\')
    text = text.replace(
        "@ADD_INIT_LD@",
        '"add 0644 overlay.d/sbin/init-ld.xz init-ld.xz" \\' if add_init_ld else '"patch" \\')
    text = text.replace("@EXIT@", ("sleep 5\n" if hang else "") + f"exit {exit_status}")
    return text.encode()


APK_NATIVE = {
    "lib/arm64-v8a/libmagiskinit.so": pseudo_random("init", 3000),
    "lib/arm64-v8a/libmagisk.so": pseudo_random("magisk", 9000),
    "lib/arm64-v8a/libinit-ld.so": pseudo_random("init-ld", 500),
}


def apk_members(script: bytes, *, options: tuple[str, ...] = (), omit: tuple[str, ...] = ()) -> dict[str, bytes]:
    wrapper = (
        f'#!/bin/sh\nexec "{sys.executable}" -B "{THIS}" --fake-magiskboot "{TOOLS}" '
        f'{" ".join(options)} "$@"\n'
    ).encode()
    members = {
        "assets/boot_patch.sh": script,
        "assets/util_functions.sh": b"# stand-in\n",
        "lib/x86_64/libmagiskboot.so": wrapper,
        "lib/x86_64/libmagisk.so": b"builder-only magisk",
        "AndroidManifest.xml": b"stand-in manifest",
        **APK_NATIVE,
    }
    for name in omit:
        del members[name]
    return members


def stock_entries() -> dict:
    Entry = magisk_image.Entry
    return {
        "init": Entry(stat.S_IFREG | 0o750, 0, 0, (0, 0), b"stock-init-binary" * 40),
        "dev": Entry(stat.S_IFDIR | 0o755, 0, 0, (0, 0), b""),
        "dev/null": Entry(stat.S_IFCHR | 0o666, 0, 0, (1, 3), b""),
        "system": Entry(stat.S_IFDIR | 0o755, 0, 0, (0, 0), b""),
        "system/bin": Entry(stat.S_IFDIR | 0o755, 0, 0, (0, 0), b""),
        "system/bin/toolbox": Entry(stat.S_IFREG | 0o755, 0, 2000, (0, 0), b"toolbox"),
        "sbin": Entry(stat.S_IFLNK | 0o777, 0, 0, (0, 0), b"/system/bin"),
        "first_stage_ramdisk": Entry(stat.S_IFDIR | 0o755, 0, 0, (0, 0), b""),
        "first_stage_ramdisk/system": Entry(stat.S_IFDIR | 0o755, 0, 0, (0, 0), b""),
        "first_stage_ramdisk/system/etc": Entry(stat.S_IFDIR | 0o755, 0, 0, (0, 0), b""),
        "first_stage_ramdisk/system/etc/fstab.uml": Entry(
            stat.S_IFREG | 0o644, 0, 0, (0, 0), b"fstab\n"),
    }


def reference_patched(stock: dict, preinit: str, stock_sha1: str) -> dict:
    """What Magisk's patch makes of the stock entries, by the test's own account."""
    Entry = magisk_image.Entry
    entries = dict(stock)
    entries["init"] = Entry(
        stat.S_IFREG | 0o750, 0, 0, (0, 0), APK_NATIVE["lib/arm64-v8a/libmagiskinit.so"])
    entries[".backup"] = Entry(stat.S_IFDIR, 0, 0, (0, 0), b"")
    entries["overlay.d"] = Entry(stat.S_IFDIR | 0o750, 0, 0, (0, 0), b"")
    entries["overlay.d/sbin"] = Entry(stat.S_IFDIR | 0o750, 0, 0, (0, 0), b"")
    for name, member in (("magisk.xz", "libmagisk.so"), ("init-ld.xz", "libinit-ld.so")):
        entries[f"overlay.d/sbin/{name}"] = Entry(
            stat.S_IFREG | 0o644, 0, 0, (0, 0),
            lzma.compress(APK_NATIVE[f"lib/arm64-v8a/{member}"], format=lzma.FORMAT_XZ))
    entries[".backup/init.xz"] = Entry(
        stock["init"].mode, 0, 0, (0, 0), lzma.compress(stock["init"].payload, format=lzma.FORMAT_XZ))
    entries[".backup/.rmlist"] = Entry(stat.S_IFREG, 0, 0, (0, 0), b"".join(
        name.encode() + b"\0" for name in (
            "overlay.d", "overlay.d/sbin", "overlay.d/sbin/init-ld.xz", "overlay.d/sbin/magisk.xz")))
    entries[".backup/.magisk"] = Entry(
        stat.S_IFREG, 0, 0, (0, 0), magisk_image.backup_config(preinit, stock_sha1))
    return entries


class Fixture:
    """A synthetic stock init_boot in a disk and layout, and a synthetic APK and pin."""

    def __init__(self, directory: Path, *, userdata: bool = True) -> None:
        self.directory = directory
        self.stock = stock_entries()
        raw = newc_dump(self.stock, order=list(self.stock), tail=196)
        self.stock_image = boot_image(raw, size=65536)
        self.preinit = "ubdb3"
        self.disk = directory / "system.img"
        self.layout = directory / "partition-layout.json"
        start = 8192
        recovery = 4096
        disk = bytes(start) + self.stock_image + bytes(recovery)
        self.disk.write_bytes(disk)
        names = ["misc", "metadata"] + (["userdata"] if userdata else [])
        self.layout.write_text(json.dumps({
            "schema_version": 2,
            "sector_size": 512,
            "system_disk": {
                "size_bytes": len(disk),
                "partitions": [
                    {"name": "init_boot", "start_bytes": start, "size_bytes": len(self.stock_image)},
                    {"name": "recovery", "start_bytes": start + len(self.stock_image), "size_bytes": recovery},
                ],
            },
            "data_disk": {"partitions": [{"name": name} for name in names]},
        }))
        self.stock_sha1 = hashlib.sha1(self.stock_image).hexdigest()

    def apk(self, name: str = "Magisk.apk", **options) -> Path:
        path = self.directory / name
        make_apk_bytes(path, apk_members(**options))
        return path

    def pin(self, apk: Path, name: str = "magisk.env") -> Path:
        path = self.directory / name
        path.write_text(
            "# SPDX-License-Identifier: GPL-2.0-or-later\n"
            "LINEAGE24_MAGISK_SCHEMA=1\n"
            "LINEAGE24_MAGISK_NAME='Magisk'\n"
            f"LINEAGE24_MAGISK_VERSION='{TEST_VERSION}'\n"
            f"LINEAGE24_MAGISK_VERSION_CODE={TEST_VERSION_CODE}\n"
            f"LINEAGE24_MAGISK_APK_SHA256='{hashlib.sha256(apk.read_bytes()).hexdigest()}'\n"
            f"LINEAGE24_MAGISK_APK_SIZE={apk.stat().st_size}\n")
        return path

    def patched_reference(self) -> bytes:
        entries = reference_patched(self.stock, self.preinit, self.stock_sha1)
        header = bytearray(self.stock_image[:4096])
        ramdisk = lz4_legacy(newc_dump(entries))
        struct.pack_into("<I", header, 12, len(ramdisk))
        image = bytes(header) + ramdisk + b"\0" * (-len(ramdisk) % 4096)
        return image + b"\0" * max(0, len(self.stock_image) - len(image))


def run_tool(script: str, *arguments: object) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-B", str(TOOLS / script), *map(str, arguments)],
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=120, check=False,
    )


# -------------------------------------------------------------------- tests --


class PinTests(unittest.TestCase):
    """The tracked pin names the release and its digest, and nothing about where a copy came from."""

    def setUp(self) -> None:
        self.path = ROOT / "android/lineage-24/metadata/magisk.env"
        self.pin = contract_checks.parse_assignments(self.path)

    def test_the_pin_has_exactly_its_fields(self) -> None:
        self.assertEqual(set(self.pin), PIN_KEYS)
        self.assertEqual(self.pin["LINEAGE24_MAGISK_SCHEMA"], "1")
        self.assertEqual(self.pin["LINEAGE24_MAGISK_NAME"], "Magisk")

    def test_the_version_and_digest_are_well_formed(self) -> None:
        self.assertRegex(self.pin["LINEAGE24_MAGISK_VERSION"], r"^v[0-9]+\.[0-9]+(\.[0-9]+)?$")
        self.assertGreater(int(self.pin["LINEAGE24_MAGISK_VERSION_CODE"]), 0)
        self.assertRegex(self.pin["LINEAGE24_MAGISK_APK_SHA256"], r"^[0-9a-f]{64}$")
        self.assertGreater(int(self.pin["LINEAGE24_MAGISK_APK_SIZE"]), 0)

    def test_the_pin_records_no_source_location(self) -> None:
        text = self.path.read_text(encoding="utf-8")
        self.assertIsNone(re.search(r"https?:|git@|\.com|\.org|/Users/|/home/|/Volumes/", text))

    def test_the_producer_files_are_classified(self) -> None:
        rows = release_inputs.read_rows(ROOT / release_inputs.LIST_RELATIVE)
        kinds = {path: kind for kind, path, _ in rows}
        self.assertEqual(kinds["android/lineage-24/metadata/magisk.env"], "input")
        # The producer and what it shares with the verifier decide the release's
        # bytes; the verifier's own command line does not.
        self.assertEqual(kinds["android/lineage-24/tools/magisk-boot.py"], "input")
        self.assertEqual(kinds["android/lineage-24/tools/magisk_image.py"], "input")
        self.assertEqual(kinds["android/lineage-24/tools/verify-magisk-boot.py"], "excluded")

    def test_the_tracked_pin_reads(self) -> None:
        pin = magisk_image.read_pin(self.path, SystemExit)
        self.assertEqual((pin.name, pin.version), ("Magisk", self.pin["LINEAGE24_MAGISK_VERSION"]))
        self.assertEqual(pin.apk_size, int(self.pin["LINEAGE24_MAGISK_APK_SIZE"]))

    def test_a_malformed_pin_is_refused(self) -> None:
        text = self.path.read_text(encoding="utf-8")
        for label, edit in (
            ("another name", lambda t: t.replace("'Magisk'", "'Other'")),
            ("a version without a v", lambda t: re.sub(r"='v([0-9.]+)'", r"='\1'", t, count=1)),
            ("a short digest", lambda t: re.sub(r"[0-9a-f]{64}", "abc", t)),
            ("a missing size", lambda t: re.sub(r"LINEAGE24_MAGISK_APK_SIZE=.*\n", "", t)),
            ("another schema", lambda t: t.replace("SCHEMA=1", "SCHEMA=2")),
            ("a zero size", lambda t: re.sub(r"(APK_SIZE)=[0-9]+", r"\1=0", t)),
        ):
            with self.subTest(label), tempfile.TemporaryDirectory() as directory:
                broken = Path(directory) / "magisk.env"
                broken.write_text(edit(text), encoding="utf-8")
                with self.assertRaises(SystemExit):
                    magisk_image.read_pin(broken, SystemExit)


class FormatTests(unittest.TestCase):
    """The decoders read what Magisk writes: lz4 legacy, xz, and newc with renumbered inodes."""

    def test_lz4_blocks_with_matches_decode(self) -> None:
        samples = (
            b"abcdabcdabcdabcd",
            b"a" * 300,
            b"xyz" * 1000 + b"tail",
            pseudo_random("lz4", 700) + pseudo_random("lz4", 700) + b"end",
            bytes(range(256)) * 8,
        )
        for data in samples:
            with self.subTest(length=len(data)):
                self.assertEqual(
                    magisk_image._lz4_block(lz4_matches(data), 1 << 20, SystemExit), data)

    def test_lz4_legacy_streams_decode_and_bad_ones_are_refused(self) -> None:
        data = b"hello ramdisk " * 5000
        stream = b"\x02\x21\x4c\x18"
        for start in range(0, len(data), 40000):
            block = lz4_matches(data[start:start + 40000])
            stream += struct.pack("<I", len(block)) + block
        self.assertEqual(magisk_image.lz4_legacy_decode(stream, SystemExit), data)
        self.assertEqual(magisk_image.lz4_legacy_decode(lz4_legacy(data), SystemExit), data)
        magic = b"\x02\x21\x4c\x18"
        for label, bad in (
            ("a missing magic", stream[1:]),
            ("a block that runs past the stream", stream[:-3]),
            ("a size field cut short", stream + b"\x01\x02"),
            ("a zero size", stream + b"\0\0\0\0"),
            ("an offset before the data", magic + struct.pack("<I", 4) + bytes([0x04, 0x61, 0x09, 0x00])),
            ("a zero offset", magic + struct.pack("<I", 4) + bytes([0x10, 0x61, 0x00, 0x00])),
            ("literals past the block", magic + struct.pack("<I", 2) + bytes([0xF0, 0x01])),
        ):
            with self.subTest(label), self.assertRaises(SystemExit):
                magisk_image.lz4_legacy_decode(bad, SystemExit)

    def test_xz_members_decode_and_bad_ones_are_refused(self) -> None:
        packed = lzma.compress(b"member", format=lzma.FORMAT_XZ)
        self.assertEqual(magisk_image.xz_decode(packed, SystemExit), b"member")
        for label, bad in (
            ("trailing data", packed + b"x"),
            ("a truncated stream", packed[:-4]),
            ("not xz", b"plain"),
        ):
            with self.subTest(label), self.assertRaises(SystemExit):
                magisk_image.xz_decode(bad, SystemExit)
        with self.assertRaises(SystemExit):
            magisk_image.xz_decode(
                lzma.compress(b"\0" * 5000, format=lzma.FORMAT_XZ), SystemExit, limit=100)

    def test_newc_keeps_the_fields_a_guest_sees_and_not_the_numbers(self) -> None:
        entries = stock_entries()
        parsed = magisk_image.parse_newc(newc_dump(entries, order=list(entries), tail=512), SystemExit)
        self.assertEqual(parsed, entries)
        self.assertEqual(
            magisk_image.parse_newc(newc_dump(entries, mtime=7), SystemExit),
            magisk_image.parse_newc(newc_dump(entries, mtime=9, order=sorted(entries)), SystemExit))
        for label, bad in (
            ("data after the trailer", newc_dump(entries) + b"x"),
            ("a truncated archive", newc_dump(entries)[:-40]),
            ("not newc", b"x" * 300),
            ("an unsafe name", newc_dump({"../x": entries["init"]})),
        ):
            with self.subTest(label), self.assertRaises(SystemExit):
                magisk_image.parse_newc(bad, SystemExit)
        single = newc_dump({"a": entries["init"]})
        with self.assertRaises(SystemExit):
            magisk_image.parse_newc(single[:-124] + single, SystemExit)


class VerifierTests(unittest.TestCase):
    """verify_patched accepts the patch of the stock image and refuses each departure from it."""

    def setUp(self) -> None:
        self.directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.fixture = Fixture(self.directory)
        self.apk_path = self.fixture.apk(script=patch_script())

    def patched(self, edit=None, *, preinit: str | None = None, compress=lz4_legacy,
                os_version: int | None = None) -> bytes:
        entries = reference_patched(
            self.fixture.stock, preinit or self.fixture.preinit, self.fixture.stock_sha1)
        if edit is not None:
            edit(entries)
        header = bytearray(self.fixture.stock_image[:4096])
        if os_version is not None:
            struct.pack_into("<I", header, 16, os_version)
        ramdisk = compress(newc_dump(entries))
        struct.pack_into("<I", header, 12, len(ramdisk))
        image = bytes(header) + ramdisk + b"\0" * (-len(ramdisk) % 4096)
        return image + b"\0" * max(0, len(self.fixture.stock_image) - len(image))

    def verify(self, patched: bytes, preinit: str | None = None) -> None:
        with zipfile.ZipFile(self.apk_path) as archive:
            magisk_image.verify_patched(
                patched, self.fixture.stock_image, archive, preinit or self.fixture.preinit, SystemExit)

    def test_the_patch_of_the_stock_image_verifies(self) -> None:
        self.verify(self.patched())
        self.assertEqual(self.patched(), self.fixture.patched_reference())

    def test_departures_from_the_patch_are_refused(self) -> None:
        Entry = magisk_image.Entry
        xz = lambda payload: lzma.compress(payload, format=lzma.FORMAT_XZ)  # noqa: E731
        config = magisk_image.backup_config("ubdb3", self.fixture.stock_sha1)

        def replace(name, **changes):
            def edit(entries):
                entry = entries[name]
                entries[name] = Entry(**{
                    "mode": entry.mode, "uid": entry.uid, "gid": entry.gid, "rdev": entry.rdev,
                    "payload": entry.payload, **changes})
            return edit

        def drop(name):
            return lambda entries: entries.pop(name)

        def add(name):
            return lambda entries: entries.__setitem__(
                name, Entry(stat.S_IFREG | 0o644, 0, 0, (0, 0), b"x"))

        for label, edit in (
            ("an /init that is not the APK's magiskinit", replace("init", payload=b"stock-init-binary")),
            ("an /init without the execute bits", replace("init", mode=stat.S_IFREG | 0o644)),
            ("an /init owned by a user", replace("init", uid=1000)),
            ("a changed stock entry", replace("system/bin/toolbox", payload=b"other")),
            ("a stock entry with another owner", replace("system/bin/toolbox", gid=0)),
            ("a stock entry with another mode", replace("system/bin/toolbox", mode=stat.S_IFREG | 0o700)),
            ("a changed link target", replace("sbin", payload=b"/system/xbin")),
            ("a removed stock entry", drop("system/bin/toolbox")),
            ("an unexpected added entry", add("system/bin/su")),
            ("a missing magisk.xz", drop("overlay.d/sbin/magisk.xz")),
            ("a missing init-ld.xz", drop("overlay.d/sbin/init-ld.xz")),
            ("a missing backup of /init", drop(".backup/init.xz")),
            ("a missing configuration", drop(".backup/.magisk")),
            ("a missing removal list", drop(".backup/.rmlist")),
            ("a missing .backup directory", drop(".backup")),
            ("a magisk.xz that is not the APK's", replace("overlay.d/sbin/magisk.xz", payload=xz(b"other"))),
            ("an init-ld.xz that is not the APK's", replace("overlay.d/sbin/init-ld.xz", payload=xz(b"other"))),
            ("a magisk.xz with trailing data", replace(
                "overlay.d/sbin/magisk.xz", payload=xz(APK_NATIVE["lib/arm64-v8a/libmagisk.so"]) + b"x")),
            ("a backup that is not the stock /init", replace(".backup/init.xz", payload=xz(b"other"))),
            ("an overlay directory with another mode", replace("overlay.d", mode=stat.S_IFDIR | 0o755)),
            ("a configuration with another flag", replace(
                ".backup/.magisk", payload=config.replace(b"KEEPVERITY=true", b"KEEPVERITY=false"))),
            ("a configuration with another SHA-1", replace(
                ".backup/.magisk", payload=magisk_image.backup_config("ubdb3", "0" * 40))),
            ("a configuration without the pre-init partition", replace(
                ".backup/.magisk", payload=magisk_image.backup_config(
                    "ubdb3", self.fixture.stock_sha1, with_preinit=False))),
            ("a removal list that misses an entry", replace(".backup/.rmlist", payload=b"overlay.d\0overlay.d/sbin\0")),
        ):
            with self.subTest(label), self.assertRaises(SystemExit):
                self.verify(self.patched(edit))

    def test_header_and_ramdisk_departures_are_refused(self) -> None:
        good = self.patched()
        with_data = bytearray(good)
        with_data[-1] = 1
        garbled = bytearray(good)
        garbled[4096 + 100:4096 + 104] = b"ZZZZ"
        for label, image in (
            ("a header that differs beyond the ramdisk size", self.patched(os_version=0x4321)),
            ("a raw ramdisk", self.patched(compress=lambda raw: raw)),
            ("data after the ramdisk", bytes(with_data)),
            ("a ramdisk that is neither raw nor lz4", self.patched(compress=lambda raw: b"\x1f\x8b" + raw)),
            ("a garbled ramdisk", bytes(garbled)),
        ):
            with self.subTest(label), self.assertRaises(SystemExit):
                self.verify(image)
        with self.assertRaises(SystemExit):
            self.verify(good, preinit="ubdb2")
        with self.assertRaises(SystemExit):
            self.verify(self.patched(preinit="ubdb2"))

    def test_another_stock_image_does_not_verify(self) -> None:
        other = bytearray(self.fixture.stock_image)
        other[5000] ^= 1
        with zipfile.ZipFile(self.apk_path) as archive, self.assertRaises(SystemExit):
            magisk_image.verify_patched(self.patched(), bytes(other), archive, "ubdb3", SystemExit)

    def test_an_apk_with_other_binaries_does_not_verify(self) -> None:
        other = self.directory / "other.apk"
        make_apk_bytes(other, apk_members(patch_script()) | {"lib/arm64-v8a/libmagiskinit.so": b"another build"})
        missing = self.directory / "missing.apk"
        make_apk_bytes(missing, apk_members(patch_script(), omit=("lib/arm64-v8a/libinit-ld.so",)))
        for path in (other, missing):
            with zipfile.ZipFile(path) as archive, self.assertRaises(SystemExit):
                magisk_image.verify_patched(
                    self.patched(), self.fixture.stock_image, archive, "ubdb3", SystemExit)

    def test_the_preinit_partition_follows_the_layout(self) -> None:
        self.assertEqual(magisk_image.preinit_device(self.fixture.layout, SystemExit), "ubdb3")
        other = Fixture(Path(self.enterContext(tempfile.TemporaryDirectory())), userdata=False)
        with self.assertRaises(SystemExit):
            magisk_image.preinit_device(other.layout, SystemExit)


class PatcherTests(unittest.TestCase):
    """magisk-boot.py runs the APK's own script and tools and refuses what does not verify."""

    def setUp(self) -> None:
        self.directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.fixture = Fixture(self.directory)
        self.output = self.directory / "out/root"

    def patch(self, apk: Path, *, pin: Path | None = None) -> subprocess.CompletedProcess:
        return run_tool(
            "magisk-boot.py", "patch", "--magisk-apk", apk, "--disk", self.fixture.disk,
            "--layout", self.fixture.layout, "--output-dir", self.output,
            "--pin", pin or self.fixture.pin(apk))

    def assert_refused(self, result: subprocess.CompletedProcess, pattern: str) -> None:
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertRegex(result.stderr, pattern)
        self.assertFalse((self.output / "init_boot-magisk.img").exists())

    def test_the_patch_is_the_stock_image_plus_what_magisk_adds(self) -> None:
        apk = self.fixture.apk(script=patch_script())
        result = self.patch(apk)
        self.assertEqual(result.returncode, 0, result.stderr)
        image = self.output / "init_boot-magisk.img"
        patched = image.read_bytes()
        self.assertEqual(patched, self.fixture.patched_reference())
        self.assertEqual(len(patched), len(self.fixture.stock_image))
        self.assertEqual((self.output / f"Magisk-{TEST_VERSION}.apk").read_bytes(), apk.read_bytes())
        self.assertEqual(sorted(path.name for path in self.output.iterdir()),
                         [f"Magisk-{TEST_VERSION}.apk", "init_boot-magisk.img"])
        # The verifier's own command line agrees, from the disk and from a stock file.
        verified = run_tool(
            "verify-magisk-boot.py", "--image", image, "--apk", apk, "--disk", self.fixture.disk,
            "--layout", self.fixture.layout, "--pin", self.fixture.pin(apk))
        self.assertEqual(verified.returncode, 0, verified.stderr)
        stock = self.directory / "stock.img"
        stock.write_bytes(self.fixture.stock_image)
        by_file = run_tool(
            "verify-magisk-boot.py", "--image", image, "--apk", apk, "--stock-image", stock,
            "--layout", self.fixture.layout)
        self.assertEqual(by_file.returncode, 0, by_file.stderr)
        # An image that is not the stock one's patch does not verify, nor does an APK that is not the pin.
        image.write_bytes(self.fixture.stock_image)
        refused = run_tool(
            "verify-magisk-boot.py", "--image", image, "--apk", apk, "--disk", self.fixture.disk,
            "--layout", self.fixture.layout)
        self.assertNotEqual(refused.returncode, 0)
        other = self.fixture.apk("other.apk", script=patch_script(exit_status=1))
        not_pinned = run_tool(
            "verify-magisk-boot.py", "--image", image, "--apk", other, "--disk", self.fixture.disk,
            "--layout", self.fixture.layout, "--pin", self.fixture.pin(apk))
        self.assertRegex(not_pinned.stderr, "is not the pinned Magisk")

    def test_a_script_that_writes_the_pre_init_line_gives_the_same_image(self) -> None:
        result = self.patch(self.fixture.apk(script=patch_script(preinit=True)))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            (self.output / "init_boot-magisk.img").read_bytes(), self.fixture.patched_reference())

    def test_an_apk_that_is_not_the_pin_is_refused(self) -> None:
        apk = self.fixture.apk(script=patch_script())
        pin = self.fixture.pin(apk)
        other = self.fixture.apk("other.apk", script=patch_script(exit_status=1))
        self.assert_refused(self.patch(other, pin=pin), "is not the pinned Magisk")
        pin.write_text(pin.read_text().replace(hashlib.sha256(apk.read_bytes()).hexdigest(), "0" * 64))
        self.assert_refused(self.patch(apk, pin=pin), "is not the pinned Magisk")

    def test_each_misbehaving_patcher_is_refused(self) -> None:
        for label, kwargs, pattern in (
            ("a script that fails", {"script": patch_script(exit_status=3)}, r"boot_patch\.sh failed \(3\)"),
            ("a script that leaves /init alone", {"script": patch_script(add_init=False)},
             "did not add|magiskinit"),
            ("a script that adds no init-ld", {"script": patch_script(add_init_ld=False)}, "did not add"),
            ("a tool that does not compress the ramdisk",
             {"script": patch_script(), "options": ("--raw-ramdisk",)}, "not lz4 legacy"),
            ("a tool whose output differs between runs",
             {"script": patch_script(), "options": ("--nondeterministic",)}, "different images"),
            ("an APK without the arm64 magiskinit",
             {"script": patch_script(), "omit": ("lib/arm64-v8a/libmagiskinit.so",)},
             "has no lib/arm64-v8a/libmagiskinit.so"),
            ("an APK without the builder's magiskboot",
             {"script": patch_script(), "omit": ("lib/x86_64/libmagiskboot.so",)},
             "has no lib/x86_64/libmagiskboot.so"),
        ):
            with self.subTest(label):
                self.assert_refused(self.patch(self.fixture.apk(**kwargs)), pattern)

    def test_a_script_that_hangs_is_stopped(self) -> None:
        spec = importlib.util.spec_from_file_location("magisk_boot_tool", TOOLS / "magisk-boot.py")
        tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tool)
        tool.STEP_TIMEOUT_SECONDS = 1
        apk = self.fixture.apk(script=patch_script(hang=True))
        work = Path(tempfile.mkdtemp(dir=self.directory))
        with zipfile.ZipFile(apk) as archive, self.assertRaisesRegex(SystemExit, "did not finish"):
            tool.patch_once(archive, self.fixture.stock_image, "ubdb3", work)

    def test_an_existing_file_is_never_replaced(self) -> None:
        apk = self.fixture.apk(script=patch_script())
        self.output.mkdir(parents=True)
        (self.output / "init_boot-magisk.img").write_bytes(b"earlier")
        result = self.patch(apk)
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stderr, "already exists")
        self.assertEqual((self.output / "init_boot-magisk.img").read_bytes(), b"earlier")

    def test_a_layout_without_userdata_is_refused(self) -> None:
        fixture = Fixture(Path(self.enterContext(tempfile.TemporaryDirectory())), userdata=False)
        apk = fixture.apk(script=patch_script())
        result = run_tool(
            "magisk-boot.py", "patch", "--magisk-apk", apk, "--disk", fixture.disk,
            "--layout", fixture.layout, "--output-dir", self.output, "--pin", fixture.pin(apk))
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stderr, "no single userdata partition")


class AssembleOptionTests(unittest.TestCase):
    """assemble-image.sh takes --magisk-apk, checks it before any work, and says what it makes."""

    def assemble(self, *arguments: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["bash", str(ROOT / "android/lineage-24/build/assemble-image.sh"), *arguments],
            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60, check=False)

    def test_the_usage_names_the_option_and_the_root_files(self) -> None:
        result = self.assemble("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--magisk-apk", result.stdout)
        self.assertIn("init_boot-magisk.img", result.stdout)

    def test_the_option_needs_an_absolute_path(self) -> None:
        required = ["--target-files", "/t.zip", "--kernel-artifact", "/k", "--mkbootfs", "/m",
                    "--mkbootimg", "/i", "--output-dir", "/o"]
        for label, arguments, pattern in (
            ("a relative path", ["--magisk-apk", "Magisk.apk"], "Magisk APK must be an absolute local path"),
            ("no path", ["--magisk-apk"], "--magisk-apk requires a path"),
        ):
            with self.subTest(label):
                result = self.assemble(*required, *arguments)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(pattern, result.stderr)


def main() -> None:
    global ROOT, TOOLS, contract_checks, magisk_image, release_inputs
    if len(sys.argv) > 1 and sys.argv[1] == "--fake-magiskboot":
        raise SystemExit(fake_magiskboot(sys.argv[2:]))
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=ROOT)
    arguments, rest = parser.parse_known_args()
    ROOT = arguments.source_root.resolve()
    TOOLS = ROOT / "android/lineage-24/tools"
    sys.path.insert(0, str(ROOT / "kernel/common/artifacts"))
    sys.path.insert(0, str(TOOLS))
    import contract_checks as checks  # noqa: PLC0415
    import magisk_image as image  # noqa: PLC0415
    import release_inputs as inputs  # noqa: PLC0415

    contract_checks, magisk_image, release_inputs = checks, image, inputs
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)


if __name__ == "__main__":
    main()
