#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the host's response to the system's memory pressure with a guest
running.

A Linux guest reads enough files to fill its page cache. The runner then
raises the levels the system's memory pressure source reports through an
injector inserted into the launcher (a real event needs a privileged tool):

- a warning turns the guest's own page-cache trimmer on and takes nothing
  else (the launcher sends the guest "trim on" and no other command), and says
  so on stderr with the footprint before and after, the trimmer's state and the
  guest RAM wire failures; the guest's page cache then falls eventually, in
  passes;
- the way back to the normal level is one line and turns the trimmer off;
- a warning inside the first one's interval starts no response, and still
  turns the trimmer back on (the interval limits responses, not the trimmer:
  the trimmer is on exactly while the last level is not normal);
- a second warning, with the trimmer on already, sends the guest nothing;
- a critical level frees the host GPU's caches (none without a GPU) and asks
  for the same gradual trimmer as a warning, never for a trim of all the cache
  ("trim once"): the launcher sends the guest "trim on" and says so with the
  footprint before and after, the bytes of graphics released, the trimmer's
  state and the guest RAM wire failures; the guest's page cache then falls
  eventually, in passes;
- the way back to the normal level turns the trimmer off, before and after the
  critical level.

The commands the launcher sends the guest are read from the injector's record,
not inferred from what the launcher writes.

The contract is docs/architecture.md (memory and executable code) and
runtime/memory_pressure_response.hpp.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import shutil
import sys
import tempfile
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


GUEST_MEMORY = "1G"
# The least the guest's page cache must grow by to make the trimmer worth
# seeing.
MINIMUM_FILL_KIB = 32 * 1024
# What the page cache must fall by, eventually, under a trimmer that is on.
MINIMUM_RELEASE_MIB = 16
# A response is written within this long of its level, and a quiet period
# (a response that must not come) lasts this long.
RESPONSE_SECONDS = 60
QUIET_SECONDS = 3
# The host's interval between two warnings' responses is 30 s; the second
# warning is raised well inside it, counted from before the first was raised.
WARN_INTERVAL_SECONDS = 30
INSIDE_INTERVAL_SECONDS = 20

# A warning's response line: the trimmer's state, nothing released.
WARNING = re.compile(
    r"uml: memory pressure warn: footprint (?P<before>\d+) -> "
    r"(?P<after>\d+) MiB; guest page cache trimmer on "
    r"\((?P<trim>done|sent|unanswered|refused|no console|guest quiesced|busy)\); "
    r"(?P<wires>\d+) guest RAM wire failures"
)
# A critical level's: the graphics released, and the trimmer's state.
RESPONSE = re.compile(
    r"uml: memory pressure (?P<level>critical): footprint "
    r"(?P<before>\d+) -> (?P<after>\d+) MiB; released "
    r"(?P<graphics>\d+) (?P<graphics_unit>MiB|KiB) graphics "
    r"\((?P<cached>\d+) cached allocations\); guest page cache trimmer on "
    r"\((?P<trim>done|sent|unanswered|refused|no console|guest quiesced|busy)\); "
    r"(?P<wires>\d+) guest RAM wire failures"
)
CHANGE = re.compile(
    r"uml: memory pressure (?P<level>normal|warn|critical) "
    r"\(was (?P<was>normal|warn|critical)\): footprint (?P<footprint>\d+) MiB, "
    r"(?P<wires>\d+) guest RAM wire failures"
)


class Injector:
    """The directory the runner and the injector inside the launcher talk
    through."""

    def __init__(self, directory: pathlib.Path) -> None:
        self.directory = directory
        self.count = 0

    def raise_level(self, level: str, timeout: float = 60) -> None:
        self.count += 1
        label = f"{level}-{self.count}"
        pending = self.directory / "request.tmp"
        pending.write_text(f"{level} {label}\n")
        pending.rename(self.directory / "request")
        result = self.directory / (label + ".done")
        deadline = time.monotonic() + timeout
        while not result.exists():
            if time.monotonic() > deadline:
                raise TimeoutError(f"the injector did not answer {label}")
            time.sleep(0.05)
        answer = result.read_text().split()
        if answer[:1] != ["ok"] or int(answer[1]) == 0:
            raise RuntimeError(
                f"the injector could not raise {label}: {answer}")
        print(f"{level}: raised on {answer[1]} sources")


def commands(directory: pathlib.Path) -> list[str]:
    """The trim commands the launcher has sent the guest, in order, from the
    injector's record of its console requests."""
    record = directory / "commands.log"
    if not record.exists():
        return []
    return [line for line in record.read_text().splitlines()
            if line.startswith("trim ")]


def responses(guest: Guest) -> list[re.Match[str]]:
    """The critical levels' response lines."""
    return list(RESPONSE.finditer(guest.host_output()))


def warnings(guest: Guest) -> list[re.Match[str]]:
    return list(WARNING.finditer(guest.host_output()))


def trimming(guest: Guest) -> str:
    """The guest's periodic page-cache trimmer: Y when it runs."""
    return guest.check_command(
        "cat /sys/module/page_cache_trim/parameters/enabled").strip()


def changes(guest: Guest) -> list[re.Match[str]]:
    return list(CHANGE.finditer(guest.host_output()))


def settle(guest: Guest, seconds: int) -> None:
    """Lets `seconds` pass in the guest. The runner reads the launcher's own
    output only while it reads the console."""
    guest.check_command(f"sleep {seconds}", seconds + 30)


def wait_for(guest: Guest, what: str, condition, timeout: float):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        guest.check_command("true", 30)
        found = condition()
        if found:
            return found
        time.sleep(0.1)
    raise RuntimeError(
        f"{what} within {timeout:.0f} s; the launcher wrote:\n"
        f"{guest.host_output()}"
    )


def cached_kib(guest: Guest) -> int:
    text = guest.check_command("awk '/^Cached:/ {print $2}' /proc/meminfo")
    return int(text.strip())


def fill_page_cache(guest: Guest, parity: int) -> int:
    """Reads files until the page cache has grown by enough; returns what it
    holds. The files of the two fills are different ones (the even and the odd
    entries of the same listing), so that the second does not find what the
    first read."""
    before = cached_kib(guest)
    guest.check_command(
        "find /usr /lib /opt -xdev -type f -size +32k 2>/dev/null "
        f"| awk 'NR % 2 == {parity}' | head -n 6000 "
        "| xargs cat > /dev/null 2>&1; true",
        300,
    )
    after = cached_kib(guest)
    if after - before < MINIMUM_FILL_KIB:
        raise RuntimeError(
            f"reading files grew the guest's page cache by only "
            f"{(after - before) // 1024} MiB ({before} -> {after} KiB)"
        )
    return after


def run(arguments: argparse.Namespace) -> None:
    directory = pathlib.Path(tempfile.mkdtemp(prefix="uml-mp-"))
    environment = dict(os.environ)
    environment["DYLD_INSERT_LIBRARIES"] = str(arguments.injector_library)
    environment["UML_MEMORY_PRESSURE_INJECTOR_DIRECTORY"] = str(directory)
    guest = Guest(
        arguments.emulator,
        arguments.rootfs,
        GUEST_MEMORY,
        "tmpfs",
        ("ncpus=2",),
        overlay_shell=True,
        environment=environment,
        separate_host_output=True,
    )
    try:
        guest.boot_to_shell(300)
        injector = Injector(directory)
        filled = fill_page_cache(guest, 1)
        print(f"the guest's page cache holds {filled // 1024} MiB")

        if trimming(guest) != "N":
            raise RuntimeError("the guest's trimmer ran before any warning")
        if commands(directory):
            raise RuntimeError(
                f"the host sent trim commands before any warning: "
                f"{commands(directory)}")

        # A warning turns the guest's trimmer on and takes nothing else.
        raised = time.monotonic()
        injector.raise_level("warn")
        first = wait_for(
            guest, "no response to the warning",
            lambda: warnings(guest), RESPONSE_SECONDS)[0]
        # The request is sent and not waited for: "sent" for a console that
        # took it.
        if first["trim"] != "sent":
            raise RuntimeError(f"the warning's response: {first.group(0)}")
        if trimming(guest) != "Y":
            raise RuntimeError(
                f"the warning left the trimmer off: {first.group(0)}")
        if commands(directory) != ["trim on"]:
            raise RuntimeError(
                f"a warning sent the guest {commands(directory)}, not just "
                f"the trimmer on")
        print(f"warn: {first.group(0)}")
        change = changes(guest)
        if not change or change[0]["level"] != "warn" or change[0]["was"] != "normal":
            raise RuntimeError(f"no line for the change to warn: {change}")

        # A second warning, still inside the interval of the first one's
        # response, is withheld: no response, and nothing sent to a trimmer
        # that is on already.
        injector.raise_level("warn")
        inside = time.monotonic() - raised
        if inside >= INSIDE_INTERVAL_SECONDS:
            raise RuntimeError(
                f"the second warning came {inside:.0f} s after the first, not "
                f"inside its {WARN_INTERVAL_SECONDS} s interval")
        settle(guest, QUIET_SECONDS)
        if len(warnings(guest)) != 1:
            raise RuntimeError(
                "a second warning inside the interval was answered:\n"
                + guest.host_output())
        if commands(directory) != ["trim on"]:
            raise RuntimeError(
                f"a withheld warning sent the guest {commands(directory)}")
        print(f"the second warning, {inside:.0f} s after the first, was "
              f"withheld and sent nothing")

        # The way back turns the trimmer off.
        injector.raise_level("normal")
        wait_for(
            guest, "the trimmer stayed on after the normal level",
            lambda: trimming(guest) == "N", RESPONSE_SECONDS)
        if commands(directory) != ["trim on", "trim off"]:
            raise RuntimeError(
                f"the normal level sent the guest {commands(directory)}")

        # A warning inside the interval of the first one's response starts no
        # response, and the trimmer is on again: the interval limits the
        # responses, not the trimmer. It comes before the wait for the page
        # cache to fall, which can outlast the interval.
        injector.raise_level("warn")
        inside = time.monotonic() - raised
        if inside >= INSIDE_INTERVAL_SECONDS:
            raise RuntimeError(
                f"the warning after the normal level came {inside:.0f} s after "
                f"the first, not inside its {WARN_INTERVAL_SECONDS} s interval")
        wait_for(
            guest, "the trimmer stayed off under a warning inside the interval",
            lambda: trimming(guest) == "Y", RESPONSE_SECONDS)
        settle(guest, QUIET_SECONDS)
        if len(warnings(guest)) != 1:
            raise RuntimeError(
                "a warning inside the interval was answered:\n"
                + guest.host_output())
        if commands(directory) != ["trim on", "trim off", "trim on"]:
            raise RuntimeError(
                f"a withheld warning sent the guest {commands(directory)}, "
                f"not just the trimmer on")
        print(f"the warning {inside:.0f} s after the first one's response was "
              f"withheld and turned the trimmer back on")

        # The page cache falls on its own, in passes: a fall that eventually
        # comes, not a drop at the warning.
        gradual = wait_for(
            guest, "the page cache did not fall eventually",
            lambda: filled - cached_kib(guest) >= MINIMUM_RELEASE_MIB * 1024
            and filled, RESPONSE_SECONDS)
        print(f"the guest's page cache eventually fell from "
              f"{gradual // 1024} MiB to {cached_kib(guest) // 1024} MiB")
        if responses(guest):
            raise RuntimeError(
                "a warning trimmed all the cache at once:\n"
                + guest.host_output())
        if "trim once" in commands(directory):
            raise RuntimeError("a warning sent the guest a trim of all the cache")

        # The way back before the critical level turns the trimmer off, so that
        # the critical level has a request to send.
        injector.raise_level("normal")
        wait_for(
            guest, "the trimmer stayed on after the normal level",
            lambda: trimming(guest) == "N", RESPONSE_SECONDS)
        if commands(directory) != ["trim on", "trim off", "trim on", "trim off"]:
            raise RuntimeError(
                f"the normal level sent the guest {commands(directory)}")

        # A critical level always acts: it frees the GPU's caches (none
        # without a GPU) and asks for the gradual trimmer, as a warning does,
        # and never for a trim of all the cache. The files are not the first
        # fill's.
        filled = fill_page_cache(guest, 0)
        injector.raise_level("critical")
        second = wait_for(
            guest, "no response to the critical level",
            lambda: responses(guest), RESPONSE_SECONDS)[0]
        # The request is sent and not waited for: "sent" for a console that
        # took it.
        if second["trim"] != "sent":
            raise RuntimeError(f"the critical response: {second.group(0)}")
        wait_for(
            guest, "the critical level left the trimmer off",
            lambda: trimming(guest) == "Y", RESPONSE_SECONDS)
        if commands(directory) != [
                "trim on", "trim off", "trim on", "trim off", "trim on"]:
            raise RuntimeError(
                f"the critical level sent the guest {commands(directory)}, "
                f"not just the trimmer on")
        print(f"critical: {second.group(0)}")

        # The page cache falls on its own, in passes: a fall that eventually
        # comes, not a drop at the critical level.
        after = wait_for(
            guest, "the page cache did not fall eventually after the critical "
            "level",
            lambda: filled - cached_kib(guest) >= MINIMUM_RELEASE_MIB * 1024
            and cached_kib(guest), RESPONSE_SECONDS)
        print(f"the guest's page cache eventually fell from "
              f"{filled // 1024} MiB to {after // 1024} MiB")
        if "trim once" in commands(directory):
            raise RuntimeError(
                "the critical level sent the guest a trim of all the cache")

        # The way back is one line and turns the trimmer off.
        injector.raise_level("normal")
        wait_for(
            guest, "no line for the way back to normal",
            lambda: sum(c["level"] == "normal" for c in changes(guest)) == 3,
            RESPONSE_SECONDS)
        wait_for(
            guest, "the trimmer stayed on after the normal level",
            lambda: trimming(guest) == "N", RESPONSE_SECONDS)
        settle(guest, QUIET_SECONDS)
        if len(warnings(guest)) != 1 or len(responses(guest)) != 1:
            raise RuntimeError(
                "the normal level was answered:\n" + guest.host_output())
        if commands(directory) != [
                "trim on", "trim off", "trim on", "trim off", "trim on",
                "trim off"]:
            raise RuntimeError(
                f"the host's requests were {commands(directory)}")
        lines = [line for line in guest.host_output().splitlines()
                 if "uml: memory pressure" in line]
        if len(lines) != 8:
            raise RuntimeError(
                "expected a line for each of the six changes (the second "
                "warning is no change) and the two responses:\n"
                + "\n".join(lines))
        if not guest.shutdown(60) or guest.return_code != 0:
            raise RuntimeError(
                f"the power-off did not end the process with 0: "
                f"{guest.return_code}")
    finally:
        guest.close()
        shutil.rmtree(directory, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--injector-library", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    for name in ("emulator", "rootfs", "injector_library"):
        if not getattr(arguments, name).is_file():
            parser.error(f"--{name.replace('_', '-')} is not a file")
    run(arguments)
    print("ok: a warning turns the guest's trimmer on, the normal level turns "
          "it off, a warning inside the interval starts no response and turns "
          "it on again, a critical level asks for the same trimmer and never "
          "for a trim of all the cache, the commands the guest was sent are "
          "exactly those, and the lines say so")
    return 0


if __name__ == "__main__":
    sys.exit(main())
