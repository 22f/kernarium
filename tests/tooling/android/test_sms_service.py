#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The text message service's logic on a plain JVM.

android/lineage-24/overlay/sms is the Java app that delivers a text message the host sends. What it builds
(the SMS-DELIVER PDUs, the argument's parsing, the memory of the identifiers it has delivered) does not touch
the platform: SmsServiceTest.java drives it, reading the PDUs back with a parser of its own and the JDK's
"UTF-16" decoder, which is what Android's SmsMessage uses for the user data. The sources compile here without
the Android glue (Delivery and InjectProvider, which need the framework); the glue is compiled by the image
build. Skipped (status 77) where the host has no JDK.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
SOURCES = REPOSITORY / "android" / "lineage-24" / "overlay" / "sms" / "src" / "android" / "kernarium" / "sms"
TEST = HERE / "sms" / "android" / "kernarium" / "sms" / "SmsServiceTest.java"
ANDROID_GLUE = ("Delivery.java", "InjectProvider.java")
PLATFORM_FREE = {"DeliveredIdentifiers.java", "DeliverPdu.java", "Message.java"}
SKIP = 77


def jdk_tool(name: str) -> str | None:
    """A working javac or java: the macOS /usr/bin stubs exist without a JDK, so ask it."""
    path = shutil.which(name)
    if path is None:
        return None
    probe = subprocess.run([path, "-version"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=60, check=False)
    return path if probe.returncode == 0 else None


def main() -> int:
    javac, java = jdk_tool("javac"), jdk_tool("java")
    if javac is None or java is None:
        print("SKIP: no JDK (javac and java) on PATH")
        return SKIP
    sources = sorted(path for path in SOURCES.glob("*.java") if path.name not in ANDROID_GLUE)
    if {path.name for path in sources} != PLATFORM_FREE:
        print(f"FAIL: the platform-free sources are {sorted(path.name for path in sources)}, "
              f"not {sorted(PLATFORM_FREE)}", file=sys.stderr)
        return 1
    for name in ANDROID_GLUE:
        if not (SOURCES / name).is_file():
            print(f"FAIL: {name} is missing", file=sys.stderr)
            return 1
    with tempfile.TemporaryDirectory(prefix="uml-sms-service-") as scratch:
        classes = pathlib.Path(scratch) / "classes"
        classes.mkdir()
        compiled = subprocess.run(
            [javac, "-Xlint:all", "-Werror", "-encoding", "UTF-8", "-d", str(classes),
             *(str(path) for path in sources), str(TEST)],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=240,
            check=False)
        if compiled.returncode != 0:
            print(f"FAIL: javac:\n{compiled.stdout}", file=sys.stderr)
            return 1
        ran = subprocess.run([java, "-Dfile.encoding=UTF-8", "-cp", str(classes), "android.kernarium.sms.SmsServiceTest"],
                             stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                             timeout=240, check=False)
        print(ran.stdout, end="")
        if ran.returncode != 0 or not ran.stdout.rstrip().endswith("PASS"):
            print("FAIL: the service's test", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
