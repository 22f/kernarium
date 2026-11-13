#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A management-console command survives other vCPUs printing.

``sysrq`` and ``stack`` send what the kernel prints while they run to the
client that asked, so the console's write() walks a list of those clients
while any vCPU may print, and end with a final reply that names the command
and says whether it ran: ``sysrq s ok``, or an error, ``sysrq s disabled``
with SysRq off and ``sysrq a failed`` for a key no operation has. The host
asks for ``sysrq s`` before it suspends a guest and flushes its disks. Three
tasks pinned to vCPUs 1 to 3 of a 4-vCPU guest keep writing to /dev/kmsg, so a
vCPU other than the one running the command owns the console whenever the
command ends, and the runner asks for ``sysrq s`` from a fresh client socket,
as the host does, EXCHANGES times or for EXCHANGE_SECONDS. Every exchange must
end with the status ``sysrq s ok``, the guest must run on, and the console
must show no fault, oops or panic. A kernel that walks the list unlocked
faulted at address 0x10 in console_write() within the first thousand
exchanges. A second guest boots with SysRq off, as Android's init leaves it,
and every exchange must end with the error ``sysrq s disabled`` and run
nothing.

``sync`` is the command the host asks for to have the guest write its storage
back, and it needs no SysRq: it runs sync(2) in the guest's process context
and replies ``synced in N ms`` only after the call returned. Two guests boot
with SysRq off and an ext4 disk whose write-back is held off (no periodic
write-back, a day's expiry, a journal commit every hour), write a file of a
byte the rest of the image does not hold, and are killed outright, the way iOS
ends a process that stops running. The one that was not asked to sync must lose
the file (the control, which proves that the setup keeps it dirty and the
image shows a loss); the one that asked ``sync`` first must have it in the
image, and its reply must be a success. ``--request`` names another command
for the second guest: a kernel without ``sync`` answers ``Unknown command``,
and ``sysrq s`` is refused with SysRq off, so the guest loses the file the
same way and the run says so.

The contract is the management-console paragraph of kernel/common/README.md.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import shutil
import socket
import struct
import sys
import tempfile
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


CPUS = 4
EXCHANGES = 10000
DISABLED_EXCHANGES = 50
EXCHANGE_SECONDS = 30.0
REPLY_SECONDS = 20.0
DISK_BYTES = 64 << 20
PAYLOAD_BYTES = 4 << 20
# The payload's byte; the rest of a freshly formatted image holds hardly any.
PAYLOAD_BYTE = 0xA5
# Held off until the host asks: no periodic write-back, an expiry of a day,
# a journal commit every hour, and ext4's delayed allocation keeps the data
# dirty in the page cache.
DIRTY_SETUP = (
    "mkfs.ext4 -q -F /dev/ubdb && mkdir -p /mnt/disk && "
    "mount -t ext4 -o commit=3600 /dev/ubdb /mnt/disk && "
    "echo 0 > /proc/sys/vm/dirty_writeback_centisecs && "
    "echo 8640000 > /proc/sys/vm/dirty_expire_centisecs && sync"
)
WRITE_PAYLOAD = (
    f"head -c {PAYLOAD_BYTES} /dev/zero | tr '\\000' '\\{PAYLOAD_BYTE:03o}' "
    "> /mnt/disk/payload && cat /proc/meminfo | grep ^Dirty"
)
MCONSOLE_MAGIC = 0xCAFEBABE
MCONSOLE_VERSION = 2
MCONSOLE_DATA = 512
WRITERS = (
    "writers=; for c in 1 2 3; do taskset -c $c sh -c 'while :; do "
    "echo \"console clients $$ 0123456789abcdef0123456789abcdef\" "
    "> /dev/kmsg; done' </dev/null >/dev/null 2>&1 & writers=\"$writers $!\"; "
    "done; echo $writers > /tmp/console-writers"
)
STOP_WRITERS = (
    "kill $(cat /tmp/console-writers); wait $(cat /tmp/console-writers); "
    "echo stopped"
)
FAILURE = re.compile(
    r"WARNING:|BUG:|Oops|Kernel panic|Darwin UML panic|Kernel mode fault|"
    r"Kernel mode signal"
)


class GuestGone(RuntimeError):
    pass


def exchange(guest: Guest, directory: pathlib.Path, serial: int,
             command: str) -> tuple[bool, str]:
    """One request from a client socket of its own, read to its final
    reply. Returns whether the final reply is an error, and its text."""
    reply = directory / f"mc-clients-{serial}"
    reply.unlink(missing_ok=True)
    endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        endpoint.bind(str(reply))
        data = command.encode()
        try:
            endpoint.sendto(
                struct.pack("<III", MCONSOLE_MAGIC, MCONSOLE_VERSION, len(data))
                + data.ljust(MCONSOLE_DATA, b"\0"),
                str(directory / "mconsole"),
            )
        except (FileNotFoundError, ConnectionRefusedError) as error:
            guest.wait_for_exit(10.0)
            raise GuestGone(f"exchange {serial}: the console is gone") from error
        endpoint.setblocking(False)
        deadline = time.monotonic() + REPLY_SECONDS
        while True:
            try:
                answer = endpoint.recv(12 + MCONSOLE_DATA)
            except BlockingIOError:
                if guest.wait_for_exit(0.002):
                    raise GuestGone(f"exchange {serial}: the guest exited")
                if time.monotonic() > deadline:
                    raise RuntimeError(
                        f"exchange {serial}: no final reply in {REPLY_SECONDS:g} s"
                    )
                continue
            error, more, length = struct.unpack_from("<III", answer)
            if not more:
                # The reply's length counts the NUL that ends its text.
                text = answer[12:12 + min(length, MCONSOLE_DATA)]
                return bool(error), text.rstrip(b"\0").decode(errors="replace")
    finally:
        endpoint.close()
        reply.unlink(missing_ok=True)


def require_status(guest: Guest, directory: pathlib.Path, serial: int,
                   command: str, status: str, refused: bool) -> None:
    """The final reply to `command` is `status`, an error if `refused`."""
    error, text = exchange(guest, directory, serial, command)
    if error != refused or text != status:
        raise RuntimeError(
            f"exchange {serial}: {command!r} ended with "
            f"{'an error' if error else 'a success'} {text!r}, "
            f"not {'an error' if refused else 'a success'} {status!r}"
        )


def check_clean(guest: Guest) -> None:
    output = guest.output()
    failure = FAILURE.search(output)
    if failure is not None:
        raise RuntimeError(
            f"the guest printed {failure[0]!r}:\n"
            + output[max(0, failure.start() - 2000):][:6000]
        )


def gone_report(guest: Guest, gone: GuestGone, done: int) -> RuntimeError:
    """The console around the fault that took the guest away."""
    output = guest.output() + "\n" + guest.host_output()
    failure = FAILURE.search(output)
    start = max(0, failure.start() - 2000) if failure else -6000
    return RuntimeError(
        f"{gone} after {done} exchanges:\n" + output[start:][:6000]
    )


def run(emulator: pathlib.Path, rootfs: pathlib.Path, engine: str,
        private: pathlib.Path) -> None:
    with Guest(emulator, rootfs, "1G", "tmpfs", (f"ncpus={CPUS}",),
               init_shell=True, engine=engine) as guest:
        guest.boot_to_shell(180)
        directory = private / f"uml-kern{guest.process_id}"
        guest.check_command(WRITERS, 30)
        started = time.monotonic()
        done = 0
        try:
            while done < EXCHANGES and time.monotonic() - started < EXCHANGE_SECONDS:
                require_status(guest, directory, done + 1, "sysrq s",
                               "sysrq s ok", False)
                done += 1
            elapsed = time.monotonic() - started
            # A command that prints the kernel's stack ends the same way, and
            # a key without an operation is an error that names it.
            require_status(guest, directory, done + 1, "stack 1",
                           "stack ok", False)
            require_status(guest, directory, done + 2, "sysrq a",
                           "sysrq a failed", True)
        except GuestGone as gone:
            raise gone_report(guest, gone, done) from None
        guest.run_command(STOP_WRITERS, 60)
        check_clean(guest)
        print(f"{done} sysrq s exchanges in {elapsed:.1f} s while three vCPUs "
              "printed, each ending with sysrq s ok")
        if not guest.shutdown(60):
            raise RuntimeError("the guest did not power off")


def run_sysrq_off(emulator: pathlib.Path, rootfs: pathlib.Path, engine: str,
                  private: pathlib.Path) -> None:
    """A guest with SysRq off, as Android's init leaves it, refuses the
    operation and says so, although it prints nothing a client could see."""
    with Guest(emulator, rootfs, "1G", "tmpfs",
               (f"ncpus={CPUS}", "sysctl.kernel.sysrq=0"),
               init_shell=True, engine=engine) as guest:
        guest.boot_to_shell(180)
        directory = private / f"uml-kern{guest.process_id}"
        guest.check_command("cat /proc/sys/kernel/sysrq", 30, expected="0")
        done = 0
        try:
            while done < DISABLED_EXCHANGES:
                require_status(guest, directory, done + 1, "sysrq s",
                               "sysrq s disabled", True)
                done += 1
            # stack is no SysRq operation, and ends as it does with SysRq on.
            require_status(guest, directory, done + 1, "stack 1",
                           "stack ok", False)
        except GuestGone as gone:
            raise gone_report(guest, gone, done) from None
        # The refusal ran nothing: the console shows no sync.
        if "Emergency Sync" in guest.output():
            raise RuntimeError("a refused sysrq s synced the file systems")
        check_clean(guest)
        print(f"{done} sysrq s exchanges with SysRq off, each ending with "
              "sysrq s disabled")
        if not guest.shutdown(60):
            raise RuntimeError("the guest did not power off")


def dirty_then_kill(emulator: pathlib.Path, rootfs: pathlib.Path,
                    engine: str, private: pathlib.Path, label: str,
                    request: str | None) -> tuple[int, tuple[bool, str] | None]:
    """Boots a guest with SysRq off and an ext4 disk, dirties a file that
    only the guest's page cache holds, asks `request` if there is one, and
    kills the emulator outright. Returns how many of the image's bytes are the
    payload's byte, and the reply to the request."""
    image = private / f"disk-{label}.img"
    with open(image, "wb") as handle:
        handle.truncate(DISK_BYTES)
    answer = None
    with Guest(emulator, rootfs, "1G", "tmpfs",
               ("ncpus=2", f"ubd1={image}", "sysctl.kernel.sysrq=0"),
               init_shell=True, engine=engine) as guest:
        guest.boot_to_shell(180)
        directory = private / f"uml-kern{guest.process_id}"
        guest.check_command("cat /proc/sys/kernel/sysrq", 30, expected="0")
        guest.check_command(DIRTY_SETUP, 60)
        output = guest.check_command(WRITE_PAYLOAD, 60)
        dirty = re.search(r"Dirty:\s+(\d+) kB", output)
        if dirty is None or int(dirty[1]) < PAYLOAD_BYTES // 1024 // 2:
            raise RuntimeError(
                f"{label}: the payload is not dirty in the guest: {output!r}"
            )
        # Nothing of it reached the image yet, or the control proves nothing.
        if request is not None:
            try:
                answer = exchange(guest, directory, 1, request)
            except GuestGone as gone:
                raise gone_report(guest, gone, 0) from None
        guest.terminate()
        if not guest.wait_for_exit(30):
            raise RuntimeError(f"{label}: the emulator survived SIGKILL")
    return image.read_bytes().count(bytes([PAYLOAD_BYTE])), answer


def run_sync_reaches_the_image(emulator: pathlib.Path, rootfs: pathlib.Path,
                               engine: str, private: pathlib.Path,
                               request: str) -> None:
    """The control loses the dirty file at a hard kill; the guest that was
    asked to write back first has it in the image."""
    lost, _ = dirty_then_kill(emulator, rootfs, engine, private, "control",
                              None)
    if lost >= PAYLOAD_BYTES // 4:
        raise RuntimeError(
            f"the control reached the image without a request ({lost} "
            f"payload bytes of {PAYLOAD_BYTES}): the setup does not hold the "
            "file back, and the check below proves nothing"
        )
    kept, answer = dirty_then_kill(emulator, rootfs, engine, private,
                                   "request", request)
    assert answer is not None
    refused, text = answer
    if refused:
        raise RuntimeError(
            f"{request!r} was refused with {text!r}, and after a hard kill "
            f"the image holds {kept} of the payload's {PAYLOAD_BYTES} bytes "
            f"(the control held {lost}): the guest's writeback was not "
            "confirmed and its data was lost"
        )
    if request == "sync" and not re.fullmatch(r"synced in \d+ ms", text):
        raise RuntimeError(f"sync replied {text!r}, not 'synced in N ms'")
    if kept < PAYLOAD_BYTES:
        raise RuntimeError(
            f"{request!r} replied {text!r}, and after a hard kill the image "
            f"holds {kept} of the payload's {PAYLOAD_BYTES} bytes (the "
            f"control held {lost})"
        )
    print(f"{request!r} replied {text!r}; after a hard kill the image holds "
          f"{kept} payload bytes of {PAYLOAD_BYTES}, the control {lost}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--only-sync", action="store_true",
                        help="run only the kill-after-request scenario")
    parser.add_argument("--request", default="sync",
                        help="the command that must reach the image (default "
                        "sync); only sync passes on every kernel that has it")
    arguments = parser.parse_args()

    # Short, so the management console's directory fits sun_path inside it.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-mcc-"))
    os.environ["TMPDIR"] = str(private)
    tempfile.tempdir = str(private)
    try:
        for scenario in () if arguments.only_sync else (run, run_sysrq_off):
            scenario(arguments.emulator.resolve(), arguments.rootfs.resolve(),
                     arguments.engine, private)
        run_sync_reaches_the_image(arguments.emulator.resolve(),
                                   arguments.rootfs.resolve(),
                                   arguments.engine, private,
                                   arguments.request)
    finally:
        shutil.rmtree(private, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
