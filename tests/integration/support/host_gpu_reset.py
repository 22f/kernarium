# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher's log after a guest suspend that the host's GPU device did not survive.

When the host resets its virtio-gpu device across a suspend, every context and
resource the guest holds is gone: the guest kernel's console shows the host
refusing its commands as
`[drm:virtio_gpu_dequeue_ctrl_func] *ERROR* response 0x1204 (command 0x202)`
(0x1203 for a resource), and the host says
`Virtio-GPU submit rejected unknown context: id=3 bytes=44`. The guest's
applications then abort in the graphics stack and the framework follows. A
runner that suspends the guest reads the launcher's log from where it stood
before the suspend and fails the step with `FAILURE` and the first such line,
so the host defect is told apart from the failure it causes in the runner's
own subject.

The runner does not quiet the screen before the suspend: the reset is a defect
of the host that the product may share, and hiding it from a proof would hide
it from the product.
"""

from __future__ import annotations

import pathlib

KERNEL = "virtio_gpu_dequeue_ctrl_func"
HOST = "submit rejected unknown context"
FAILURE = "host GPU reset across the suspend"


def reset_line(text: str) -> str | None:
    """The first line of the launcher's log that shows the host's GPU device was reset: the guest kernel's error
    for a command refused by the host, or the host's own refusal of a context it no longer knows."""
    for line in text.splitlines():
        if (KERNEL in line and "ERROR" in line) or HOST in line:
            return line.strip()[:300]
    return None


def log_size(log: pathlib.Path) -> int:
    """How much of the launcher's log (the host's messages and the guest kernel's console) is written now."""
    try:
        return log.stat().st_size
    except OSError:
        return 0


def log_since(log: pathlib.Path, offset: int) -> str:
    with log.open("rb") as stream:
        stream.seek(offset)
        return stream.read().decode(errors="replace")


def failure_since(log: pathlib.Path, offset: int) -> str | None:
    """The failure message when the log, from `offset` on, shows the reset: FAILURE and the first line; else None."""
    line = reset_line(log_since(log, offset))
    return None if line is None else f"{FAILURE}: {line}"
