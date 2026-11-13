#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify the management console's sync on an Android release.

Android's init leaves magic SysRq off, so the host's ``sysrq s`` request is
refused there and a hard end of the process loses what the guest still holds
dirty. The ``sync`` command needs no SysRq: it runs sync(2) in the guest's
process context and replies ``synced in N ms`` only after the call returned
(the management-console paragraph of kernel/common/README.md). This runner
shows it on the release's own kernel and file system:

- the guest's SysRq mask is 0, as the launcher leaves it;
- a file of a byte the data disk does not otherwise hold is written under
  /data/local/tmp (unencrypted) while the guest's periodic write-back is held
  off, and it is dirty in the guest's page cache but not in the host's image
  (the control: a request that has not been made reaches nothing);
- ``sync`` replies with a success, and the image on the host then holds the
  file;
- the emulator is killed outright, the way iOS ends a process that never runs
  again, and the image still holds it.

``--request`` names another command for the same run: a kernel without ``sync``
answers ``Unknown command`` and ``sysrq s`` is refused with SysRq off, so the
file is not in the image and the run fails saying so. Every wait has a
deadline, and the guest is stopped through its own process on any exit.

The runner is opt-in: it needs the macos-android17 executable, an installed
release, and an adb client. It boots like run_android_video_suspend.py, whose
start-up it uses.
usage: run_android_sync.py --emulator uml-darwin-android17 [--release DIR]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import importlib.util
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]

SPEC = importlib.util.spec_from_file_location(
    "run_android_video_suspend", HERE / "run_android_video_suspend.py")
assert SPEC is not None and SPEC.loader is not None
base = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = base
SPEC.loader.exec_module(base)
boot_android = base.boot_android
Failure = base.Failure

PAYLOAD_BYTES = 8 << 20
PAYLOAD_BYTE = 0xA5
PAYLOAD = "/data/local/tmp/sync-payload"
# The write-back held off: no periodic write-back and an expiry of a day.
HOLD_BACK = ("echo 0 > /proc/sys/vm/dirty_writeback_centisecs; "
             "echo 8640000 > /proc/sys/vm/dirty_expire_centisecs")
WRITE = (f"head -c {PAYLOAD_BYTES} /dev/zero | tr '\\000' '\\{PAYLOAD_BYTE:03o}' "
         f"> {PAYLOAD}")
SYNC_REPLY = re.compile(r"synced in \d+ ms")
CHUNK = 64 << 20


def image_payload(image: pathlib.Path) -> int:
    """How many bytes of the image on the host are the payload's byte."""
    count = 0
    marker = bytes([PAYLOAD_BYTE])
    with open(image, "rb") as handle:
        while chunk := handle.read(CHUNK):
            count += chunk.count(marker)
    return count


def su(guest: "base.Guest", command: str) -> str:
    return guest.shell(f"su 0 sh -c '{command}'")


def run_checks(guest: "base.Guest", image: pathlib.Path, request: str) -> None:
    mask = guest.shell("cat /proc/sys/kernel/sysrq").strip()
    if mask != "0":
        raise Failure(f"the guest's SysRq mask is {mask!r}, not 0: the run proves nothing about a guest "
                      "that needs no SysRq")
    su(guest, HOLD_BACK)
    # What the guest wrote while booting is not the payload's byte, and the control is taken first.
    before_write = image_payload(image)
    su(guest, WRITE)
    dirty = re.search(r"Dirty:\s+(\d+) kB", guest.shell("grep ^Dirty /proc/meminfo"))
    if dirty is None or int(dirty[1]) < PAYLOAD_BYTES // 1024 // 2:
        raise Failure(f"the payload is not dirty in the guest: {dirty and dirty[0]!r}")
    time.sleep(3)
    control = image_payload(image)
    if control - before_write >= PAYLOAD_BYTES // 4:
        raise Failure(f"the image holds {control - before_write} payload bytes before any request: the data "
                      "disk's file system does not hold the file back, and the check below proves nothing")
    error, text = guest.console.exchange(request, 60)
    after = image_payload(image)
    if error:
        raise Failure(f"{request!r} was refused with {text!r}; the image holds {after - before_write} of the "
                      f"payload's {PAYLOAD_BYTES} bytes (the control held {control - before_write})")
    if request == "sync" and not SYNC_REPLY.fullmatch(text):
        raise Failure(f"sync replied {text!r}, not 'synced in N ms'")
    if after - before_write < PAYLOAD_BYTES:
        raise Failure(f"{request!r} replied {text!r} and the image holds {after - before_write} of the "
                      f"payload's {PAYLOAD_BYTES} bytes (the control held {control - before_write})")
    guest.process.kill()
    guest.process.wait(timeout=30)
    killed = image_payload(image)
    if killed - before_write < PAYLOAD_BYTES:
        raise Failure(f"after a hard kill the image holds {killed - before_write} payload bytes of "
                      f"{PAYLOAD_BYTES}")
    print(f"{request!r} replied {text!r}; the image held {control - before_write} payload bytes before it, "
          f"{after - before_write} after it and {killed - before_write} after a hard kill (of {PAYLOAD_BYTES})")


def main() -> int:
    parser = base.build_parser()
    parser.description = __doc__
    parser.add_argument("--request", default="sync",
                        help="the command that must reach the image (default sync)")
    options = parser.parse_args()
    if options.cpus <= 0 or options.boot_timeout <= 0:
        print("counts and timeouts must be positive", file=sys.stderr)
        return 2
    adb = boot_android.resolve_executable(options.adb, sdk_fallback=True)
    # Short, so the management console's socket path stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-asy-"))
    instance = options.instance or private / "instance"
    guest: "base.Guest | None" = None
    try:
        guest = base.start_guest(options, str(adb), private)
        run_checks(guest, instance / boot_android.android_instance.DATA_DISK, options.request)
        print("PASS")
        return 0
    except (Failure, boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        log = private / "emulator.log"
        if log.is_file():
            print("launcher log tail:\n" + "\n".join(log.read_text(errors="replace").splitlines()[-30:]),
                  file=sys.stderr)
        return 1
    finally:
        if guest is not None:
            boot_android.disconnect_adb(guest.adb, guest.serial)
            boot_android.stop_guest(guest.process)
        shutil.rmtree(private, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
