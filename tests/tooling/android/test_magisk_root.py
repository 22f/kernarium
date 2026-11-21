#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Magisk root files of an Android release.

A release can carry, beside its system disk, a copy of its own init_boot that
Magisk's patcher made and the official Magisk APK it was made from, both from the
one release pinned in android/lineage-24/metadata/magisk.env. These tests use the
synthetic inputs of magisk_fixture.py: no Magisk file is in the repository.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import importlib.util
import json
import lzma
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest import mock
from pathlib import Path

import magisk_fixture
from magisk_fixture import (
    APK_NATIVE, TEST_VERSION, Fixture, lz4_legacy, lz4_matches, make_apk_bytes,
    newc_dump, patch_script, apk_members, pseudo_random, reference_patched, stock_entries,
)

ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / "android/lineage-24/tools"
PIN_KEYS = {
    "LINEAGE24_MAGISK_SCHEMA",
    "LINEAGE24_MAGISK_NAME",
    "LINEAGE24_MAGISK_VERSION",
    "LINEAGE24_MAGISK_VERSION_CODE",
    "LINEAGE24_MAGISK_APK_SHA256",
    "LINEAGE24_MAGISK_APK_SIZE",
}


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

    def test_record_prints_the_manifest_root_record_of_a_producers_files(self) -> None:
        apk = self.fixture.apk(script=patch_script())
        pin = self.fixture.pin(apk)
        self.assertEqual(self.patch(apk, pin=pin).returncode, 0)
        result = run_tool(
            "magisk-boot.py", "record", "--root-dir", self.output, "--disk", self.fixture.disk,
            "--layout", self.fixture.layout, "--pin", pin)
        self.assertEqual(result.returncode, 0, result.stderr)
        record = json.loads(result.stdout)
        patched = (self.output / "init_boot-magisk.img").read_bytes()
        stock = self.fixture.stock_image
        self.assertEqual(record["magisk"]["version_name"], TEST_VERSION)
        self.assertEqual(record["apk"], {
            "path": f"root/Magisk-{TEST_VERSION}.apk",
            "sha256": hashlib.sha256(apk.read_bytes()).hexdigest(),
            "size": apk.stat().st_size,
        })
        self.assertEqual(record["boot_image"], {
            "path": "root/init_boot-magisk.img",
            "sha256": hashlib.sha256(patched).hexdigest(),
            "size": len(patched),
            "stock_init_boot_sha256": hashlib.sha256(stock).hexdigest(),
            "stock_init_boot_sha1": hashlib.sha1(stock).hexdigest(),
        })
        # No patched image, or an APK that is not the pin, gives no record.
        (self.output / "init_boot-magisk.img").unlink()
        missing = run_tool(
            "magisk-boot.py", "record", "--root-dir", self.output, "--disk", self.fixture.disk,
            "--layout", self.fixture.layout, "--pin", pin)
        self.assertNotEqual(missing.returncode, 0)
        self.assertRegex(missing.stderr, "Magisk-patched init_boot is missing")
        other = self.fixture.apk("other.apk", script=patch_script(exit_status=1))
        wrong = self.directory / "wrong"
        wrong.mkdir()
        (wrong / f"Magisk-{TEST_VERSION}.apk").write_bytes(other.read_bytes())
        (wrong / "init_boot-magisk.img").write_bytes(patched)
        not_pinned = run_tool(
            "magisk-boot.py", "record", "--root-dir", wrong, "--disk", self.fixture.disk,
            "--layout", self.fixture.layout, "--pin", pin)
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

    def load_tool(self):
        spec = importlib.util.spec_from_file_location("magisk_boot_tool", TOOLS / "magisk-boot.py")
        tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tool)
        return tool

    def patch_in_process(self, tool, apk: Path, pin: Path) -> None:
        tool.patch(argparse.Namespace(
            pin=pin, magisk_apk=apk, disk=self.fixture.disk, layout=self.fixture.layout,
            output_dir=self.output))

    def test_the_apk_is_one_set_of_bytes_whatever_happens_to_the_path(self) -> None:
        # The path is checked against the pin, and then used for the extraction, the
        # execution and the delivered copy. A file that is replaced in between must
        # change none of them: the bytes checked are the bytes used.
        original = self.fixture.apk(script=patch_script())
        pin = self.fixture.pin(original)
        original_bytes = original.read_bytes()
        other = self.fixture.apk("other.apk", script=patch_script(exit_status=1))
        for label, name in (
            ("while the stock image is read, before the archive is opened", "read_partition"),
            ("after the patch, before the copy is delivered", "verify_patched"),
        ):
            with self.subTest(label):
                shutil.rmtree(self.output, ignore_errors=True)
                original.write_bytes(original_bytes)
                tool = self.load_tool()
                real = getattr(magisk_image, name)

                def swapping(*arguments, _real=real, **keywords):
                    result = _real(*arguments, **keywords)
                    original.write_bytes(other.read_bytes())
                    return result

                with mock.patch.object(magisk_image, name, swapping):
                    self.patch_in_process(tool, original, pin)
                self.assertEqual(
                    (self.output / f"Magisk-{TEST_VERSION}.apk").read_bytes(), original_bytes)
                self.assertEqual(
                    (self.output / "init_boot-magisk.img").read_bytes(),
                    self.fixture.patched_reference())

    def test_an_apk_that_is_a_link_is_refused(self) -> None:
        apk = self.fixture.apk(script=patch_script())
        link = self.directory / "link.apk"
        link.symlink_to(apk)
        self.assert_refused(self.patch(link, pin=self.fixture.pin(apk)), "regular non-symlink file")

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
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=ROOT)
    arguments, rest = parser.parse_known_args()
    ROOT = arguments.source_root.resolve()
    TOOLS = ROOT / "android/lineage-24/tools"
    magisk_fixture.configure(ROOT)
    import contract_checks as checks  # noqa: PLC0415
    import release_inputs as inputs  # noqa: PLC0415

    contract_checks, magisk_image, release_inputs = checks, magisk_fixture.magisk_image, inputs
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)


if __name__ == "__main__":
    main()
