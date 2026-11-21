#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Make a release's Magisk-patched init_boot from the pinned official APK.

    magisk-boot.py patch --magisk-apk APK --disk system.img \\
        --layout partition-layout.json --output-dir DIR

runs on the x86-64 Linux builder, after the system disk is assembled. It reads
the APK, a local file, once, refuses it unless it is the pinned release
(metadata/magisk.env), and uses those same bytes to extract from, to run and to
deliver: the path is never reopened. It reads the stock init_boot partition from
the disk and runs Magisk's own boot_patch.sh from the APK twice, in private
directories, with the fixed patch inputs: Magisk's x86-64 magiskboot patches, and
only the APK's arm64 magiskinit, magisk and init-ld go into the ramdisk. The two
results must be identical, and the result must verify as the stock image plus exactly what Magisk
adds (magisk_image.py). DIR, which must not exist or be empty, then holds
init_boot-magisk.img and the unmodified APK under its official name, which is how a
release carries them as its root/ files; the pair appears together or not at all.

The script cannot name the data disk's userdata partition, which it asks the
guest's own magisk for; the tool therefore writes the missing PREINITDEVICE line
into .backup/.magisk itself when the script did not, with magiskboot, and
repacks.

    magisk-boot.py record --root-dir DIR --disk system.img \\
        --layout partition-layout.json

prints the manifest's `root` record for the files of such a DIR, for a producer
that writes its own manifest (publish-release.py builds the record itself). The
record is the producer's statement; verify-magisk-boot.py --release checks the
files against it.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import magisk_image  # noqa: E402

RUNS = 2
STEP_TIMEOUT_SECONDS = 300
# How much of a failing step's own output the refusal repeats.
TAIL_LINES = 40
TOOL_PATH = "/usr/bin:/bin"
PATCHED_NAME = Path(magisk_image.PATCHED_PATH).name
NEW_IMAGE = "new-boot.img"
STOCK_IMAGE = "init_boot.img"
SHELL = "/bin/sh"

# The files the script expects beside it, and where each comes from in the APK.
STAGED = (
    ("boot_patch.sh", magisk_image.SCRIPT_MEMBER, 0o755),
    ("util_functions.sh", magisk_image.UTILITY_MEMBER, 0o755),
    ("magiskboot", magisk_image.MAGISKBOOT_MEMBER, 0o755),
    ("magiskinit", magisk_image.MAGISKINIT_MEMBER, 0o755),
    ("magisk", magisk_image.MAGISK_MEMBER, 0o755),
    ("init-ld", magisk_image.INIT_LD_MEMBER, 0o644),
)


def error(message: str) -> SystemExit:
    return SystemExit(f"Magisk boot patcher: {message}")


def run_step(arguments: list[str], work: Path, env: dict[str, str], label: str) -> str:
    """Run one step in its own process group, which a timeout kills whole."""
    try:
        process = subprocess.Popen(
            arguments,
            cwd=work,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            start_new_session=True,
        )
    except OSError as failure:
        raise error(f"cannot run {label}: {failure}") from failure
    try:
        output, _ = process.communicate(timeout=STEP_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        raise error(f"{label} did not finish in {STEP_TIMEOUT_SECONDS} s") from None
    if process.returncode != 0:
        tail = "\n".join(output.splitlines()[-TAIL_LINES:])
        raise error(f"{label} failed ({process.returncode}):\n{tail}")
    return output


def patch_once(archive, stock: bytes, preinit: str, work: Path) -> bytes:
    """One run of Magisk's patcher in the private directory `work`."""
    for name, member, mode in STAGED:
        path = work / name
        path.write_bytes(magisk_image.apk_member(archive, member, error))
        path.chmod(mode)
    (work / STOCK_IMAGE).write_bytes(stock)
    env = {
        "PATH": TOOL_PATH,
        "HOME": str(work),
        "LC_ALL": "C",
        **dict(magisk_image.PATCH_FLAGS),
        "PREINITDEVICE": preinit,
    }
    run_step([SHELL, "boot_patch.sh", STOCK_IMAGE], work, env, "Magisk's boot_patch.sh")
    produced = work / NEW_IMAGE
    if produced.is_symlink() or not produced.is_file():
        raise error(f"boot_patch.sh produced no {NEW_IMAGE}")
    image = magisk_image.read_file(produced, len(stock), f"the {NEW_IMAGE} of boot_patch.sh", error)

    # The script reads the data disk's partition from the guest's own magisk,
    # which cannot run here: complete .backup/.magisk when it left the line out.
    _, ramdisk = magisk_image.boot_ramdisk(image, "patched init_boot", error)
    entries = magisk_image.parse_newc(
        magisk_image.decode_ramdisk(ramdisk, "patched", error), error
    )
    config = entries.get(magisk_image.CONFIG_ENTRY)
    if config is None:
        raise error(f"the patched ramdisk has no {magisk_image.CONFIG_ENTRY}")
    stock_sha1 = hashlib.sha1(stock).hexdigest()
    wanted = magisk_image.backup_config(preinit, stock_sha1)
    if config.payload == wanted:
        return image
    if config.payload != magisk_image.backup_config(preinit, stock_sha1, with_preinit=False):
        raise error(
            f"the patcher wrote an unexpected {magisk_image.CONFIG_ENTRY}: it has neither "
            "the fixed patch inputs with the pre-init partition nor without it"
        )
    (work / "config.new").write_bytes(wanted)
    run_step(
        [str(work / "magiskboot"), "cpio", "ramdisk.cpio",
         f"add 000 {magisk_image.CONFIG_ENTRY} config.new"],
        work, env, "magiskboot cpio",
    )
    run_step(
        [str(work / "magiskboot"), "repack", STOCK_IMAGE, "final-boot.img"],
        work, env, "magiskboot repack",
    )
    final = work / "final-boot.img"
    if final.is_symlink() or not final.is_file():
        raise error("magiskboot repack produced no image")
    return magisk_image.read_file(final, len(stock), "the image magiskboot repacked", error)


def write_new(path: Path, data: bytes) -> None:
    """Write a file that must not exist, and put it on disk."""
    with path.open("xb") as handle:
        handle.write(data)
        handle.flush()
        os.fsync(handle.fileno())


def patch(arguments: argparse.Namespace) -> None:
    if os.geteuid() == 0:
        # Magisk's script unmounts and moves system directories when it aborts
        # outside a booted system.
        raise error("the patcher must not run as root: run it as the build user")
    pin = magisk_image.read_pin(arguments.pin, error)
    apk = arguments.magisk_apk
    if not apk.is_absolute():
        raise error("--magisk-apk must be an absolute local path")
    snapshot = magisk_image.read_pinned_apk(apk, pin, error)
    for label, path in (("--disk", arguments.disk), ("--layout", arguments.layout)):
        if path.is_symlink() or not path.is_file():
            raise error(f"{label} must be a regular non-symlink file")
    output = arguments.output_dir
    if not output.is_absolute():
        raise error("--output-dir must be an absolute local path")
    if output.is_symlink() or (output.exists() and not output.is_dir()):
        raise error(f"--output-dir must be a real directory, not a link or a file: {output}")
    patched_path = output / PATCHED_NAME
    apk_copy = output / Path(magisk_image.apk_path(pin.version)).name
    for path in (patched_path, apk_copy):
        if path.exists() or path.is_symlink():
            raise error(f"{path.name} already exists: an existing file is never replaced")

    stock = magisk_image.read_partition(arguments.disk, arguments.layout, "init_boot", error)
    magisk_image.boot_ramdisk(stock, "the release's init_boot", error)
    preinit = magisk_image.preinit_device(arguments.layout, error)
    results: list[bytes] = []
    with magisk_image.open_apk(snapshot, error) as archive:
        for _ in range(RUNS):
            work = Path(tempfile.mkdtemp(prefix=".magisk-boot."))
            try:
                results.append(patch_once(archive, stock, preinit, work))
            finally:
                shutil.rmtree(work, ignore_errors=True)
        if any(result != results[0] for result in results):
            raise error(
                f"{RUNS} runs of the patcher on the same inputs gave different images: "
                "the result must be a function of the stock image, the APK and the flags"
            )
        magisk_image.verify_patched(results[0], stock, archive, preinit, error)

    # Both files are written in a private directory beside the output and published
    # together by renaming it: a failure on the way leaves nothing behind, so the
    # same command can be run again, and an existing directory must be empty.
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".magisk-root.", dir=output.parent))
    try:
        write_new(staging / PATCHED_NAME, results[0])
        write_new(staging / apk_copy.name, snapshot)
        for name in (PATCHED_NAME, apk_copy.name):
            os.chmod(staging / name, 0o644)
        os.chmod(staging, 0o755)
        try:
            os.rename(staging, output)
        except OSError as failure:
            raise error(
                f"cannot publish the root files to {output}, which must not exist or must be "
                f"an empty directory: {failure}"
            ) from failure
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    print(
        f"made the Magisk-patched init_boot of {pin.name} {pin.version}: "
        f"{patched_path} ({len(results[0])} bytes, SHA-256 {magisk_image.sha256_bytes(results[0])})"
    )


def record(arguments: argparse.Namespace) -> None:
    import release_common  # the one place the record is built

    for label, path in (("--disk", arguments.disk), ("--layout", arguments.layout)):
        if path.is_symlink() or not path.is_file():
            raise error(f"{label} must be a regular non-symlink file")
    if not arguments.root_dir.is_absolute() or not arguments.root_dir.is_dir():
        raise error("--root-dir must be an absolute local directory")
    document = release_common.root_record(
        arguments.root_dir, arguments.disk, arguments.layout, arguments.pin)
    print(json.dumps(document, indent=2, sort_keys=True))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    patch_parser = commands.add_parser(
        "patch", help="patch the release's init_boot with the pinned Magisk release"
    )
    patch_parser.add_argument("--magisk-apk", type=Path, required=True)
    patch_parser.add_argument("--disk", type=Path, required=True, help="the release's system.img")
    patch_parser.add_argument("--layout", type=Path, required=True, help="its partition-layout.json")
    patch_parser.add_argument("--output-dir", type=Path, required=True)
    patch_parser.add_argument("--pin", type=Path, default=magisk_image.PIN_PATH, help=argparse.SUPPRESS)
    record_parser = commands.add_parser(
        "record", help="print the manifest's root record for a producer's root/ directory"
    )
    record_parser.add_argument("--root-dir", type=Path, required=True)
    record_parser.add_argument("--disk", type=Path, required=True, help="the release's system.img")
    record_parser.add_argument("--layout", type=Path, required=True, help="its partition-layout.json")
    record_parser.add_argument("--pin", type=Path, default=None, help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    (patch if arguments.command == "patch" else record)(arguments)


if __name__ == "__main__":
    main()
