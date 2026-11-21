# SPDX-License-Identifier: GPL-2.0-or-later
"""The Magisk-patched init_boot of an Android release.

A release can carry, beside its system disk, a copy of its own init_boot that
Magisk's patcher made, and the official Magisk APK it was made from, as its
`root/` files. This module owns what the producer (magisk-boot.py), the release
verifier (release_common.py) and verify-magisk-boot.py share: the pin of the
Magisk release, the fixed patch inputs, the root record a manifest keeps, and the
check that a patched image is its stock image plus exactly what Magisk adds.

Magisk's patcher turns a stock ramdisk into one that starts Magisk's init in
front of the stock init: it replaces /init, keeps the stock one as
`.backup/init.xz`, adds `overlay.d/sbin/{magisk,init-ld}.xz` and a `.backup`
directory with the patch flags, the stock image's SHA-1 and the list of added
entries, and compresses the ramdisk as lz4 legacy. The check decodes both
ramdisks with the standard library and compares them entry by entry, so it runs
wherever Python does and needs none of Magisk's tools. Each caller passes its own
error type, as android_image.py's readers do.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import io
import json
import lzma
import re
import stat
import struct
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path

import android_image

_ARTIFACTS = str(Path(__file__).resolve().parents[3] / "kernel/common/artifacts")
if _ARTIFACTS not in sys.path:
    sys.path.insert(0, _ARTIFACTS)
import contract_checks  # noqa: E402

Error = android_image.Error

ANDROID_ROOT = Path(__file__).resolve().parents[1]
PIN_PATH = ANDROID_ROOT / "metadata/magisk.env"

ROOT_DIRECTORY = "root"
PATCHED_PATH = f"{ROOT_DIRECTORY}/init_boot-magisk.img"
PATCHED_PAYLOAD = ("magisk-init-boot", "boot-image-v4-kernelless")
APK_PAYLOAD = ("magisk-apk", "android-package")
MANIFEST = "release-manifest.json"
SYSTEM_DISK = "guest/system.img"
PARTITION_LAYOUT = "guest/partition-layout.json"

MAGISK_NAME = "Magisk"
VERSION_RE = re.compile(r"^v[0-9]+\.[0-9]+(\.[0-9]+)?$")
HEX40_RE = re.compile(r"^[0-9a-f]{40}$")
HEX64_RE = re.compile(r"^[0-9a-f]{64}$")

# The ABI of the guest, whose Magisk binaries go into the ramdisk, and of the
# builder, whose magiskboot runs the patcher. The APK carries both.
GUEST_ABI = "arm64-v8a"
BUILDER_ABI = "x86_64"
SCRIPT_MEMBER = "assets/boot_patch.sh"
UTILITY_MEMBER = "assets/util_functions.sh"
MAGISKBOOT_MEMBER = f"lib/{BUILDER_ABI}/libmagiskboot.so"
MAGISKINIT_MEMBER = f"lib/{GUEST_ABI}/libmagiskinit.so"
MAGISK_MEMBER = f"lib/{GUEST_ABI}/libmagisk.so"
INIT_LD_MEMBER = f"lib/{GUEST_ABI}/libinit-ld.so"
MAX_APK_MEMBER = 64 * 1024 * 1024

# What the patch is told, and so what the patched ramdisk's .backup/.magisk says:
# the verified boot and the forced encryption of the stock ramdisk stay as they
# are (the guest has neither), and the image is not a recovery or vendor boot.
PATCH_FLAGS = (
    ("KEEPVERITY", "true"),
    ("KEEPFORCEENCRYPT", "false"),
    ("RECOVERYMODE", "false"),
    ("VENDORBOOT", "false"),
)
# Magisk keeps its modules and settings on a partition it names at boot: the
# data disk's userdata, which first-stage init finds as the second block device
# of the guest. The position is read from the release's own layout.
PREINIT_DISK = "ubdb"
PREINIT_PARTITION = "userdata"

# What the patch adds to the stock ramdisk, with the mode of each entry.
ADDED_DIRECTORIES = {
    ".backup": stat.S_IFDIR | 0o000,
    "overlay.d": stat.S_IFDIR | 0o750,
    "overlay.d/sbin": stat.S_IFDIR | 0o750,
}
ADDED_FILES = {
    ".backup/.magisk": stat.S_IFREG | 0o000,
    ".backup/.rmlist": stat.S_IFREG | 0o000,
    "overlay.d/sbin/magisk.xz": stat.S_IFREG | 0o644,
    "overlay.d/sbin/init-ld.xz": stat.S_IFREG | 0o644,
}
BACKUP_INIT = ".backup/init.xz"
CONFIG_ENTRY = ".backup/.magisk"
REMOVAL_LIST_ENTRY = ".backup/.rmlist"
REMOVAL_LIST = (
    "overlay.d", "overlay.d/sbin", "overlay.d/sbin/init-ld.xz", "overlay.d/sbin/magisk.xz",
)

LZ4_LEGACY_MAGIC = b"\x02\x21\x4c\x18"
LZ4_LEGACY_BLOCK = 8 * 1024 * 1024
MAX_RAMDISK = 256 * 1024 * 1024
MAX_DECODED_MEMBER = 64 * 1024 * 1024


def apk_path(version: str) -> str:
    """The release path of the official APK of a Magisk version."""
    return f"{ROOT_DIRECTORY}/Magisk-{version}.apk"


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


# ------------------------------------------------------------------- the pin --


@dataclass(frozen=True)
class Pin:
    name: str
    version: str
    version_code: int
    apk_sha256: str
    apk_size: int


PIN_KEYS = {
    "LINEAGE24_MAGISK_SCHEMA",
    "LINEAGE24_MAGISK_NAME",
    "LINEAGE24_MAGISK_VERSION",
    "LINEAGE24_MAGISK_VERSION_CODE",
    "LINEAGE24_MAGISK_APK_SHA256",
    "LINEAGE24_MAGISK_APK_SIZE",
}


def read_pin(path: Path, error: Error) -> Pin:
    values = contract_checks.parse_assignments(path, error)
    if set(values) != PIN_KEYS:
        raise error("the Magisk pin does not have exactly its fields")
    if values["LINEAGE24_MAGISK_SCHEMA"] != "1":
        raise error("the Magisk pin is not schema 1")
    if values["LINEAGE24_MAGISK_NAME"] != MAGISK_NAME:
        raise error(f"the Magisk pin is not for {MAGISK_NAME}")
    version = values["LINEAGE24_MAGISK_VERSION"]
    if VERSION_RE.fullmatch(version) is None:
        raise error("the Magisk pin's version is malformed")
    digest = values["LINEAGE24_MAGISK_APK_SHA256"]
    if HEX64_RE.fullmatch(digest) is None:
        raise error("the Magisk pin's APK digest is not a SHA-256")
    code = int(values["LINEAGE24_MAGISK_VERSION_CODE"])
    size = int(values["LINEAGE24_MAGISK_APK_SIZE"])
    if code < 1 or size < 1:
        raise error("the Magisk pin's version code or APK size is zero")
    return Pin(MAGISK_NAME, version, code, digest, size)


def check_apk_is_pin(apk: Path, pin: Pin, error: Error) -> None:
    """The APK is the pinned Magisk release, by size and SHA-256."""
    if apk.is_symlink() or not apk.is_file():
        raise error(f"the Magisk APK must be a regular non-symlink file: {apk}")
    if apk.stat().st_size != pin.apk_size or sha256_file(apk) != pin.apk_sha256:
        raise error(
            f"the Magisk APK is not the pinned {pin.name} {pin.version} "
            "(its size or SHA-256 differs from metadata/magisk.env)"
        )


# --------------------------------------------------------------- the record --


def _expect_keys(value: object, keys: set[str], label: str, error: Error) -> dict:
    if not isinstance(value, dict) or set(value) != keys:
        raise error(f"{label} must be an object with exactly {sorted(keys)}")
    return value


def _hex(value: object, pattern: re.Pattern[str], label: str, error: Error) -> str:
    if not isinstance(value, str) or pattern.fullmatch(value) is None:
        raise error(f"{label} is not a lowercase hexadecimal digest")
    return value


def _size(value: object, label: str, error: Error) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 1:
        raise error(f"{label} is not a positive integer")
    return value


def validate_root_record(value: object, error: Error) -> dict:
    """Check the `root` record a release manifest keeps; returns it."""
    record = _expect_keys(value, {"magisk", "apk", "boot_image"}, "the root record", error)
    magisk = _expect_keys(record["magisk"], {"name", "version_name", "version_code"},
                          "the root record's magisk", error)
    if magisk["name"] != MAGISK_NAME:
        raise error(f"the root record is not for {MAGISK_NAME}")
    version = magisk["version_name"]
    if not isinstance(version, str) or VERSION_RE.fullmatch(version) is None:
        raise error("the root record's Magisk version is malformed")
    _size(magisk["version_code"], "the root record's Magisk version code", error)
    apk = _expect_keys(record["apk"], {"path", "sha256", "size"}, "the root record's apk", error)
    if apk["path"] != apk_path(version):
        raise error(f"the root record's APK path is not {apk_path(version)}")
    _hex(apk["sha256"], HEX64_RE, "the root record's APK SHA-256", error)
    _size(apk["size"], "the root record's APK size", error)
    patched = _expect_keys(
        record["boot_image"],
        {"path", "sha256", "size", "stock_init_boot_sha256", "stock_init_boot_sha1"},
        "the root record's boot_image", error,
    )
    if patched["path"] != PATCHED_PATH:
        raise error(f"the root record's patched image path is not {PATCHED_PATH}")
    _hex(patched["sha256"], HEX64_RE, "the root record's patched image SHA-256", error)
    _size(patched["size"], "the root record's patched image size", error)
    _hex(patched["stock_init_boot_sha256"], HEX64_RE, "the root record's stock init_boot SHA-256", error)
    _hex(patched["stock_init_boot_sha1"], HEX40_RE, "the root record's stock init_boot SHA-1", error)
    return record


def root_payload_specs(record: dict) -> dict[str, tuple[str, str]]:
    """The inventory role and format of each root file, by path."""
    return {
        record["boot_image"]["path"]: PATCHED_PAYLOAD,
        record["apk"]["path"]: APK_PAYLOAD,
    }


def root_files(record: dict) -> dict[str, dict]:
    """The recorded size and digest of each root file, by path."""
    return {
        record["boot_image"]["path"]: record["boot_image"],
        record["apk"]["path"]: record["apk"],
    }


def check_root_files(release_root: Path, record: dict, error: Error) -> None:
    """Every root file is a regular file of the recorded size and SHA-256."""
    for relative, entry in root_files(record).items():
        path = release_root / relative
        if path.is_symlink() or not path.is_file():
            raise error(f"release root/ is partial: {relative} is missing or unsafe")
        if path.stat().st_size != entry["size"] or sha256_file(path) != entry["sha256"]:
            raise error(f"release root file differs from its recorded size or SHA-256: {relative}")


# ------------------------------------------------------------ the disk side --


def read_partition(disk: Path, layout: Path, name: str, error: Error) -> bytes:
    """The bytes of a system disk partition, by the release's partition layout."""
    start, size = android_image.partition_range(layout, disk, name, error)
    with disk.open("rb") as handle:
        handle.seek(start)
        return android_image.read_exact(handle, size, f"partition {name}", error)


def preinit_device(layout: Path, error: Error) -> str:
    """The guest's name of the data disk's userdata partition, from the layout."""
    try:
        document = json.loads(layout.read_text(encoding="utf-8"))
        partitions = document["data_disk"]["partitions"]
        names = [entry["name"] for entry in partitions]
    except (OSError, UnicodeError, ValueError, KeyError, TypeError) as failure:
        raise error(f"cannot read the data disk partitions of the layout: {failure}") from failure
    if names.count(PREINIT_PARTITION) != 1:
        raise error(f"the data disk layout has no single {PREINIT_PARTITION} partition")
    return f"{PREINIT_DISK}{names.index(PREINIT_PARTITION) + 1}"


def backup_config(preinit: str, stock_sha1: str, *, with_preinit: bool = True) -> bytes:
    """The text of .backup/.magisk that the patch writes for these inputs."""
    lines = [f"{key}={value}" for key, value in PATCH_FLAGS]
    if with_preinit:
        lines.append(f"PREINITDEVICE={preinit}")
    lines.append(f"SHA1={stock_sha1}")
    return ("\n".join(lines) + "\n").encode("ascii")


# ------------------------------------------------------------- the formats --


def _lz4_block(block: bytes, limit: int, error: Error) -> bytes:
    """Decode one LZ4 block of at most `limit` bytes."""
    out = bytearray()
    index = 0
    end = len(block)
    while index < end:
        token = block[index]
        index += 1
        literals = token >> 4
        if literals == 15:
            while True:
                if index >= end:
                    raise error("truncated lz4 literal length")
                extra = block[index]
                index += 1
                literals += extra
                if extra != 255:
                    break
        if index + literals > end:
            raise error("truncated lz4 literals")
        out += block[index:index + literals]
        index += literals
        if index == end:
            break
        if index + 2 > end:
            raise error("truncated lz4 match offset")
        offset = block[index] | (block[index + 1] << 8)
        index += 2
        length = token & 15
        if length == 15:
            while True:
                if index >= end:
                    raise error("truncated lz4 match length")
                extra = block[index]
                index += 1
                length += extra
                if extra != 255:
                    break
        length += 4
        if offset == 0 or offset > len(out):
            raise error("lz4 match offset is outside the decoded data")
        if len(out) + length > limit:
            raise error("lz4 block decodes to more than its limit")
        start = len(out) - offset
        if offset >= length:
            out += out[start:start + length]
        else:
            pattern = bytes(out[start:])
            out += (pattern * (length // offset + 1))[:length]
        if len(out) > limit:
            raise error("lz4 block decodes to more than its limit")
    if len(out) > limit:
        raise error("lz4 block decodes to more than its limit")
    return bytes(out)


def lz4_legacy_decode(data: bytes, error: Error, limit: int = MAX_RAMDISK) -> bytes:
    """Decode a stream in the lz4 legacy frame: the magic, then blocks of a
    four-byte little-endian size and an LZ4 block of at most 8 MiB decoded."""
    if data[:4] != LZ4_LEGACY_MAGIC:
        raise error("ramdisk is not an lz4 legacy stream")
    index = 4
    out = bytearray()
    while index < len(data):
        if index + 4 > len(data):
            raise error("truncated lz4 legacy block size")
        (size,) = struct.unpack_from("<I", data, index)
        index += 4
        if size == 0 or index + size > len(data):
            raise error("lz4 legacy block exceeds the stream")
        out += _lz4_block(data[index:index + size], LZ4_LEGACY_BLOCK, error)
        index += size
        if len(out) > limit:
            raise error("ramdisk decodes to more than its limit")
    return bytes(out)


def xz_decode(data: bytes, error: Error, limit: int = MAX_DECODED_MEMBER) -> bytes:
    """Decode a whole xz stream, refusing trailing data and an oversized result."""
    decoder = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
    try:
        out = decoder.decompress(data, max_length=limit + 1)
    except lzma.LZMAError as failure:
        raise error(f"xz member does not decode: {failure}") from failure
    if len(out) > limit:
        raise error("xz member decodes to more than its limit")
    if not decoder.eof or decoder.unused_data:
        raise error("xz member is truncated or has trailing data")
    return out


@dataclass(frozen=True)
class Entry:
    mode: int
    uid: int
    gid: int
    rdev: tuple[int, int]
    payload: bytes


def parse_newc(data: bytes, error: Error) -> dict[str, Entry]:
    """Map each path of an uncompressed newc archive to its entry, with the
    fields a guest sees. The inode, link count and time are not kept: Magisk
    renumbers them when it writes the archive. Only identical empty directories
    may repeat, and only zero bytes may follow the trailer."""
    entries: dict[str, Entry] = {}
    index = 0
    while True:
        header = data[index:index + android_image.NEWC_HEADER_SIZE]
        if len(header) != android_image.NEWC_HEADER_SIZE:
            raise error("truncated newc header")
        if header[:6] not in (b"070701", b"070702"):
            raise error("archive is not an uncompressed newc archive")
        try:
            fields = [int(header[6 + 8 * n:14 + 8 * n], 16) for n in range(13)]
        except ValueError as failure:
            raise error(f"malformed newc header: {failure}") from failure
        mode, uid, gid, file_size = fields[1], fields[2], fields[3], fields[6]
        rdev = (fields[9], fields[10])
        name_size = fields[11]
        if file_size > android_image.MAX_NEWC_ENTRY_SIZE or not 2 <= name_size <= 4096:
            raise error("newc entry has an invalid size")
        index += android_image.NEWC_HEADER_SIZE
        raw = data[index:index + name_size]
        if len(raw) != name_size or raw[-1:] != b"\0":
            raise error("newc path is truncated or not NUL terminated")
        index = android_image.align4(index + name_size)
        if raw[:-1] == b"TRAILER!!!":
            if file_size != 0:
                raise error("newc trailer has data")
            if any(data[index:]):
                raise error("newc archive has nonzero data after its trailer")
            return entries
        name = android_image.safe_name(raw[:-1], error)
        payload = data[index:index + file_size]
        if len(payload) != file_size:
            raise error(f"truncated payload for {name}")
        index = android_image.align4(index + file_size)
        previous = entries.get(name)
        if previous is not None and not (
            stat.S_ISDIR(previous.mode) and stat.S_ISDIR(mode)
            and previous.mode == mode and not previous.payload and not payload
        ):
            raise error(f"ambiguous duplicate archive entry: {name}")
        entries[name] = Entry(mode, uid, gid, rdev, payload)


def boot_ramdisk(image: bytes, label: str, error: Error) -> tuple[android_image.BootImage, bytes]:
    """The ramdisk of a kernelless boot-image v4, with nothing after it but zeros."""
    info = android_image.parse_boot_image(io.BytesIO(image), 0, len(image), error)
    end = info.ramdisk_offset + info.ramdisk_size
    if any(image[end:]):
        raise error(f"{label} has data after its ramdisk")
    return info, image[info.ramdisk_offset:end]


def decode_ramdisk(ramdisk: bytes, label: str, error: Error) -> bytes:
    """The raw newc archive of a ramdisk that is raw or lz4 legacy."""
    if ramdisk[:6] in (b"070701", b"070702"):
        return ramdisk
    if ramdisk[:4] == LZ4_LEGACY_MAGIC:
        return lz4_legacy_decode(ramdisk, error)
    raise error(f"{label} ramdisk is neither a raw newc archive nor lz4 legacy")


# ------------------------------------------------------------- the checks --


def apk_member(archive: zipfile.ZipFile, name: str, error: Error) -> bytes:
    try:
        info = archive.getinfo(name)
    except KeyError:
        raise error(f"the Magisk APK has no {name}") from None
    if info.is_dir() or info.file_size <= 0 or info.file_size > MAX_APK_MEMBER:
        raise error(f"the Magisk APK member has an unsafe size: {name}")
    try:
        return archive.read(info)
    except (OSError, zipfile.BadZipFile, RuntimeError, ValueError) as failure:
        raise error(f"cannot read {name} of the Magisk APK: {failure}") from failure


def open_apk(apk: Path, error: Error) -> zipfile.ZipFile:
    try:
        return zipfile.ZipFile(apk)
    except (OSError, zipfile.BadZipFile) as failure:
        raise error(f"the Magisk APK is not a readable archive: {failure}") from failure


def verify_patched(
    patched: bytes,
    stock: bytes,
    apk: zipfile.ZipFile,
    preinit: str,
    error: Error,
) -> None:
    """The patched init_boot is its stock image plus what Magisk adds, and nothing else.

    `patched` and `stock` are whole images. Refused: a header that differs from
    the stock one beyond the ramdisk size; a ramdisk that is not raw or lz4 legacy;
    an entry the patch removed or an unexpected one it added; a stock entry that
    changed; an /init that is not the APK's arm64 magiskinit; a backup, overlay or
    configuration that is not what the fixed patch inputs give.
    """
    _, stock_ramdisk = boot_ramdisk(stock, "stock init_boot", error)
    _, patched_ramdisk = boot_ramdisk(patched, "patched init_boot", error)
    stock_header = bytearray(stock[:android_image.BOOT_PAGE_SIZE])
    patched_header = bytearray(patched[:android_image.BOOT_PAGE_SIZE])
    stock_header[12:16] = patched_header[12:16] = b"\0\0\0\0"
    if stock_header != patched_header:
        raise error("the patched boot image header differs from the stock one beyond its ramdisk size")
    if patched_ramdisk[:4] != LZ4_LEGACY_MAGIC:
        raise error("the patched ramdisk is not lz4 legacy")

    before = parse_newc(decode_ramdisk(stock_ramdisk, "stock", error), error)
    after = parse_newc(decode_ramdisk(patched_ramdisk, "patched", error), error)
    added = set(ADDED_DIRECTORIES) | set(ADDED_FILES) | {BACKUP_INIT}
    if set(before) & added:
        raise error("the stock ramdisk already holds entries the patch adds")
    stock_init = before.get("init")
    if stock_init is None or not stat.S_ISREG(stock_init.mode):
        raise error("the stock ramdisk has no regular /init")
    missing = sorted(set(before) - set(after))
    if missing:
        raise error(f"the patch removed stock entries: {missing[:4]}")
    unexpected = sorted(set(after) - set(before) - added)
    if unexpected:
        raise error(f"the patch added unexpected entries: {unexpected[:4]}")
    absent = sorted(added - set(after))
    if absent:
        raise error(f"the patch did not add entries it must: {absent[:4]}")
    for name, entry in before.items():
        if name != "init" and after[name] != entry:
            raise error(f"the patch changed the stock entry {name}")

    init = after["init"]
    if (
        init.payload != apk_member(apk, MAGISKINIT_MEMBER, error)
        or init.mode != stat.S_IFREG | 0o750
        or init.uid != 0 or init.gid != 0 or init.rdev != (0, 0)
    ):
        raise error("/init of the patched ramdisk is not the APK's arm64 magiskinit")
    for name, expected in {**ADDED_DIRECTORIES, **ADDED_FILES}.items():
        entry = after[name]
        if entry.mode != expected or entry.uid != 0 or entry.gid != 0 or entry.rdev != (0, 0):
            raise error(f"the patched ramdisk's {name} has the wrong mode or owner")
    backup = after[BACKUP_INIT]
    if backup.mode != stock_init.mode or backup.uid != 0 or backup.gid != 0:
        raise error(f"the patched ramdisk's {BACKUP_INIT} has the wrong mode or owner")
    if xz_decode(backup.payload, error) != stock_init.payload:
        raise error(f"{BACKUP_INIT} is not the stock /init")
    for name, member in (
        ("overlay.d/sbin/magisk.xz", MAGISK_MEMBER),
        ("overlay.d/sbin/init-ld.xz", INIT_LD_MEMBER),
    ):
        if xz_decode(after[name].payload, error) != apk_member(apk, member, error):
            raise error(f"{name} is not the APK's arm64 {member.rsplit('/', 1)[1]}")
    removal = after[REMOVAL_LIST_ENTRY].payload
    if removal != b"".join(entry.encode("ascii") + b"\0" for entry in REMOVAL_LIST):
        raise error("the patched ramdisk's removal list is not the list of the entries it added")
    expected_config = backup_config(preinit, hashlib.sha1(stock).hexdigest())
    if after[CONFIG_ENTRY].payload != expected_config:
        raise error(
            "the patched ramdisk's .backup/.magisk is not the fixed patch inputs, the data "
            "disk's pre-init partition and the SHA-1 of the stock image"
        )


def verify_release_root(release_root: Path, error: Error, pin: Pin | None = None) -> dict:
    """Verify the root files of a release directory against its own manifest.

    The manifest's root record is well formed, both files are the recorded
    ones, the recorded stock digests are those of the release's own init_boot
    partition, and the patched image verifies against that partition and the
    APK. With a pin, the record is the pinned Magisk release. Returns the record.
    """
    try:
        manifest = contract_checks.load_json_object(release_root / MANIFEST, error)
    except (OSError, ValueError) as failure:
        raise error(f"cannot read the release manifest: {failure}") from failure
    if "root" not in manifest:
        raise error("the release manifest has no root record")
    record = validate_root_record(manifest["root"], error)
    check_root_files(release_root, record, error)
    if pin is not None:
        magisk, apk = record["magisk"], record["apk"]
        if (magisk["version_name"], magisk["version_code"]) != (pin.version, pin.version_code):
            raise error("the root record is not the pinned Magisk version")
        if (apk["sha256"], apk["size"]) != (pin.apk_sha256, pin.apk_size):
            raise error("the Magisk APK digest is not the pinned one")
    disk = release_root / SYSTEM_DISK
    layout = release_root / PARTITION_LAYOUT
    stock = read_partition(disk, layout, "init_boot", error)
    patched = record["boot_image"]
    if (
        sha256_bytes(stock) != patched["stock_init_boot_sha256"]
        or hashlib.sha1(stock).hexdigest() != patched["stock_init_boot_sha1"]
    ):
        raise error("the root record's stock digests are not those of the release's init_boot")
    with open_apk(release_root / record["apk"]["path"], error) as apk:
        verify_patched(
            (release_root / patched["path"]).read_bytes(),
            stock,
            apk,
            preinit_device(layout, error),
            error,
        )
    return record
