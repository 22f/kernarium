# SPDX-License-Identifier: GPL-2.0-or-later
"""The permission review that an old application gets before its first component runs.

An application that targets below API 23 gets a screen (ReviewPermissionsActivity) before any of its components
runs, and a broadcast to it is skipped until the person presses Continue, which is what a user does on the first
launch of such a game. A runner that starts a process of such a package with `am broadcast` presses the button
here, once after every install of the package.

`guest` is a running guest with `shell(command, timeout)` (the output of a command that exits 0) and
`adb_run(*arguments, timeout)` (a subprocess result). `failure` is the exception class the caller reports a
step's failure with.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re
import time
import xml.etree.ElementTree as ElementTree

#: What `dumpsys package` prints in the package's runtime permissions while the review is still to be done.
REVIEW_FLAG = "REVIEW_REQUIRED"
REVIEW_SECONDS = 60
UI_DUMP = "/data/local/tmp/permission-review-ui.xml"
BOUNDS = re.compile(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]")


def continue_button(ui: str) -> tuple[int, int] | None:
    """The centre of the review's Continue button in a `uiautomator dump`, if it is on screen."""
    try:
        root = ElementTree.fromstring(ui)
    except ElementTree.ParseError:
        return None
    for node in root.iter("node"):
        if node.get("resource-id", "").endswith(":id/continue_button"):
            match = BOUNDS.fullmatch(node.get("bounds", ""))
            if match:
                left, top, right, bottom = (int(group) for group in match.groups())
                return (left + right) // 2, (top + bottom) // 2
    return None


def review_pending(guest, package: str) -> bool:
    return REVIEW_FLAG in guest.shell(f"dumpsys package {package} | grep -A4 'runtime permissions'")


def complete_review(guest, package: str, failure: type[Exception] = RuntimeError) -> str:
    """Presses Continue on the review of `package` if one is pending; raises `failure` when it stays pending."""
    if not review_pending(guest, package):
        return "no review pending"
    guest.shell(f"am start -W -a android.intent.action.REVIEW_PERMISSIONS "
                f"--es android.intent.extra.PACKAGE_NAME {package}", timeout=60)
    deadline = time.monotonic() + REVIEW_SECONDS
    while time.monotonic() < deadline:
        guest.adb_run("shell", f"uiautomator dump {UI_DUMP}", timeout=60)
        dump = guest.adb_run("shell", f"cat {UI_DUMP}", timeout=30)
        button = continue_button(dump.stdout) if dump.returncode == 0 else None
        if button is not None:
            guest.shell(f"input tap {button[0]} {button[1]}")
            time.sleep(2)
            if not review_pending(guest, package):
                return "permission review completed"
        time.sleep(2)
    raise failure(f"the permission review of {package} was not completed")
