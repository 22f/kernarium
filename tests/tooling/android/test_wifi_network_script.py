#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The guest's Wi-Fi network script against a fake Wi-Fi framework.

The real script (android/lineage-24/overlay/tools/kernarium_wifi_network.sh) runs
under `sh` with stub `cmd`, `getprop`, `setprop`, `sleep` and `log` over a
fake framework that has what the Wi-Fi service has: a config store that the
platform writes some time after a save and reads once, a list in memory, a
station that joins a saved network, and the persistent marker. Each case boots
that framework once or twice and states what the script did: the saves it
asked for, the repair lines it logged, the marker and what the store holds
afterwards. A guest that stops is a script killed right after it wrote the
marker. The stubs keep time by the seconds the script sleeps, so nothing waits.

The test checks itself: mutants of the script (marker 1 final, no save of a
network the platform lost, a single look, no early end at marker 2) must each
fail a case.

`test_wifi_network_script.py --script FILE` prints what a script gets wrong.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import os
import sys

CLOCK_LIMIT = 600
HEADER = "Network Id      SSID                         Security type"


# ---- The fake framework: a `cmd wifi` the stub forwards to. It is the first thing in the file, so that
# a stub starts without the imports of the test.

def load(path: str) -> dict:
    """The framework's state: `name value` lines, a value being None, a boolean or an integer."""
    values = {"None": None, "True": True, "False": False}
    memory = {}
    with open(path) as stream:
        for line in stream:
            name, text = line.split()
            memory[name] = values[text] if text in values else int(text)
    return memory


def save(path: str, memory: dict) -> None:
    with open(path, "w") as stream:
        stream.write("".join(f"{name} {value}\n" for name, value in memory.items()))


def note(state: str, call: str) -> None:
    with open(os.path.join(state, "calls"), "a") as stream:
        stream.write(call + "\n")


def framework(state: str, arguments: list[str]) -> int:
    """One `cmd wifi ...` call against the framework's state; prints what the service prints."""
    path = os.path.join(state, "framework")
    memory = load(path)
    with open(os.path.join(state, "sleeps")) as stream:
        now = sum(int(seconds) for seconds in stream.read().split())
    advance(memory, now)
    status = 0
    command = arguments[:2]
    if command == ["wifi", "list-networks"]:
        note(state, "list")
        if memory["busy_looks"] > 0:
            memory["busy_looks"] -= 1
            print("No networks")
        elif memory["listed"]:
            print(HEADER)
            print("0            VirtWifi                         open")
        else:
            print("No networks")
    elif command == ["wifi", "status"]:
        note(state, "status")
        print("Wifi is enabled" if memory["wifi_on"] else "Wifi is disabled")
        if memory["wifi_on"] and memory["joined"]:
            print('Wifi is connected to "VirtWifi"')
        else:
            print("Wifi is not connected")
    elif command == ["wifi", "add-network"]:
        note(state, "add")
        if memory["add_fails"]:
            status = 1
        else:
            if not memory["listed"] or memory["disabled_now"]:
                memory["since"] = now
            memory["listed"] = True
            memory["disabled_now"] = False
            # Before the read the platform cannot save: the network lives in memory only.
            if memory["read_done"]:
                if memory["store_delay"] <= 0:
                    memory["stored"], memory["disabled"] = True, False
                else:
                    memory["store_due"] = now + memory["store_delay"]
    else:
        status = 2
    save(path, memory)
    return status


def advance(memory: dict, end: int) -> None:
    """Applies what happens by time `end`, in order: the config store's read, its write, the join."""
    if end > CLOCK_LIMIT:
        raise SystemExit("the script runs past the clock limit: it does not end")
    while True:
        due = []
        if not memory["read_done"] and memory["read_at"] <= end:
            due.append((memory["read_at"], "read"))
        if memory["store_due"] is not None and memory["store_due"] <= end:
            due.append((memory["store_due"], "store"))
        if (memory["listed"] and memory["wifi_on"] and not memory["disabled_now"] and not memory["joined"]
                and memory["join_after"] is not None and memory["since"] + memory["join_after"] <= end):
            due.append((memory["since"] + memory["join_after"], "join"))
        if not due:
            return
        _, what = min(due)
        if what == "read":
            memory["read_done"] = True
            memory["listed"] = memory["stored"]
            memory["disabled_now"] = memory["disabled"] and memory["stored"]
            memory["since"] = memory["read_at"] if memory["stored"] else None
            memory["joined"] = memory["joined"] and memory["stored"]
        elif what == "store":
            memory["stored"], memory["disabled"] = memory["listed"], memory["disabled_now"]
            memory["store_due"] = None
        else:
            memory["joined"] = True


if __name__ == "__main__" and sys.argv[1:2] == ["--stub"]:
    raise SystemExit(framework(sys.argv[2], sys.argv[3:]))

import dataclasses
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[3] / "android/lineage-24/overlay/tools/kernarium_wifi_network.sh"
MARKER = "persist.sys.kernarium.virtwifi_saved"


# ---- A guest the cases boot.

@dataclasses.dataclass(frozen=True)
class Boot:
    """What the framework does during one boot."""
    read_at: int | None = None       # when the config store is read; None: it was read before the script ran
    store_delay: int = 0             # seconds from a save to the store's write
    join_after: int | None = 5       # seconds a saved, enabled network takes to join; None: never
    busy_looks: int = 0              # listings that print what an empty one does
    add_fails: bool = False          # the Wi-Fi service is not up: saving does nothing
    wifi_on: bool = True
    kill_after_marker: bool = False  # the guest stops right after the script wrote the marker


@dataclasses.dataclass(frozen=True)
class Expect:
    status: int = 0
    adds: int = 0
    notes: int = 0
    marker: str = ""
    stored: bool = False
    asks: int | None = None          # the calls the script made of the Wi-Fi service, when it must be none


@dataclasses.dataclass(frozen=True)
class Case:
    name: str
    marker: str
    stored: bool
    boots: tuple[tuple[Boot, Expect], ...]
    disabled: bool = False


class Guest:
    """The persistent half of the framework and the property store, kept between boots."""

    def __init__(self, directory: Path, marker: str, stored: bool, disabled: bool) -> None:
        self.directory = directory
        self.state = directory / "state"
        (self.state / "properties").mkdir(parents=True)
        if marker:
            (self.state / "properties" / MARKER).write_text(marker + "\n")
        self.stored = stored
        self.disabled = disabled
        self.stubs = directory / "stubs"
        self.stubs.mkdir()
        python = sys.executable
        me = Path(__file__).resolve()
        state = self.state
        bodies = {
            "cmd": f'exec "{python}" -S -E "{me}" --stub "{state}" "$@"\n',
            "getprop": f'echo "getprop $*" >> "{state}/calls"\ncat "{state}/properties/$1" 2>/dev/null || echo\n',
            "setprop": (
                f'echo "setprop $*" >> "{state}/calls"\necho "$2" > "{state}/properties/$1"\n'
                f'if [ -f "{state}/kill" ]; then rm -f "{state}/kill"; kill -9 $PPID; fi\n'
            ),
            "sleep": f'echo "$1" >> "{state}/sleeps"\n',
            "log": f'echo "log $*" >> "{state}/calls"\n',
        }
        for name, body in bodies.items():
            (self.stubs / name).write_text("#!/bin/sh\n" + body)
            (self.stubs / name).chmod(0o755)

    def run(self, script: Path, boot: Boot) -> tuple[int, list[str], str, bool]:
        """Boots the framework and runs the script once; returns its status, the calls it made, the marker
        and whether the config store holds the network afterwards."""
        memory = {
            "read_at": boot.read_at if boot.read_at is not None else 0,
            "read_done": boot.read_at is None, "store_delay": boot.store_delay, "store_due": None,
            "join_after": boot.join_after, "busy_looks": boot.busy_looks, "add_fails": boot.add_fails,
            "wifi_on": boot.wifi_on, "stored": self.stored, "disabled": self.disabled,
            "listed": self.stored and boot.read_at is None,
            "disabled_now": self.disabled and self.stored and boot.read_at is None,
            "since": 0 if self.stored and boot.read_at is None else None, "joined": False,
        }
        save(str(self.state / "framework"), memory)
        (self.state / "sleeps").write_text("")
        (self.state / "calls").write_text("")
        if boot.kill_after_marker:
            (self.state / "kill").touch()
        done = subprocess.run(["/bin/sh", str(script)], env={"PATH": f"{self.stubs}:/usr/bin:/bin"},
                              capture_output=True, text=True, timeout=120)
        (self.state / "kill").unlink(missing_ok=True)
        after = load(str(self.state / "framework"))
        self.stored, self.disabled = after["stored"], after["disabled"]
        marker_file = self.state / "properties" / MARKER
        marker = marker_file.read_text().strip() if marker_file.exists() else ""
        return done.returncode, (self.state / "calls").read_text().splitlines(), marker, self.stored


# ---- The cases.

ONE = Boot()
CASES = (
    # Marker 2 never asks the service anything, in any state.
    *(Case(f"marker 2, {what}", "2", stored, ((boot, Expect(marker="2", stored=stored, asks=0)),), disabled=disabled)
      for what, stored, disabled, boot in (
          ("an empty list (forgotten)", False, False, ONE),
          ("a joined network", True, False, ONE),
          ("a network that does not join", True, False, Boot(join_after=None)),
          ("a disabled network", True, True, ONE),
          ("Wi-Fi off", True, False, Boot(wifi_on=False)),
          ("Wi-Fi off and an empty list", False, False, Boot(wifi_on=False)),
          ("a listing that fails", True, False, Boot(busy_looks=5)),
          ("the store read late", False, False, Boot(read_at=3)),
      )),
    # Two listings print what an empty one does: the third look finds the network.
    Case("a listing that fails at marker 1", "1", True, (
        (Boot(busy_looks=2), Expect(marker="2", stored=True)),
    )),
    Case("a first boot", "", False, ((ONE, Expect(adds=1, marker="2", stored=True)),)),
    # The guest stops right after the marker is written and before the store is: marker 1 and no network.
    Case("A: a stop before the store wrote the network, then a boot", "", False, (
        (Boot(store_delay=10, kill_after_marker=True), Expect(status=-9, adds=1, marker="1", stored=False)),
        (ONE, Expect(adds=1, notes=1, marker="2", stored=True)),
    )),
    # The platform has not read its store when the script saves; the read then drops the network.
    Case("B: a network saved before the store is read", "", False, (
        (Boot(read_at=3), Expect(adds=2, notes=1, marker="2", stored=True)),
    )),
    Case("data of an earlier release at marker 1 without the network", "1", False, (
        (ONE, Expect(adds=1, notes=1, marker="2", stored=True)),
    )),
    Case("E: marker 1 and a stored network that joins", "1", True, ((ONE, Expect(marker="2", stored=True)),)),
    Case("E: marker 1 and a stored network that joins late", "1", True, (
        (Boot(join_after=30), Expect(marker="2", stored=True)),
    )),
    Case("marker 1, Wi-Fi off and an empty list", "1", False, (
        (Boot(wifi_on=False), Expect(adds=1, notes=1, marker="1", stored=True)),
    )),
    Case("marker 1, Wi-Fi off and a stored network", "1", True, (
        (Boot(wifi_on=False), Expect(marker="1", stored=True)),
    )),
    Case("no marker and a network saved by hand", "", True, ((ONE, Expect(marker="2", stored=True)),)),
    Case("a disabled network", "1", True, (
        (ONE, Expect(adds=1, notes=1, marker="2", stored=True)),
    ), disabled=True),
    Case("a disabled network that does not join once saved again", "1", True, (
        (Boot(join_after=None), Expect(adds=1, notes=1, marker="1", stored=True)),
    ), disabled=True),
    Case("a Wi-Fi service that is not up, no marker", "", False, (
        (Boot(add_fails=True), Expect(status=1, adds=30, marker="", stored=False)),
    )),
    Case("a Wi-Fi service that is not up, marker 1", "1", False, (
        (Boot(add_fails=True), Expect(status=1, adds=30, marker="1", stored=False)),
    )),
)


def judge(script: Path, case: Case) -> list[str]:
    """What the script gets wrong in one case, as messages."""
    problems = []
    with tempfile.TemporaryDirectory(prefix="wifi-network-script-") as directory:
        guest = Guest(Path(directory), case.marker, case.stored, case.disabled)
        for number, (boot, expect) in enumerate(case.boots, 1):
            status, calls, marker, stored = guest.run(script, boot)
            adds = calls.count("add")
            notes = len([call for call in calls if call.startswith("log ")])
            asks = len([call for call in calls if call in ("add", "list", "status")])
            if (status, adds, notes, marker, stored) != (expect.status, expect.adds, expect.notes, expect.marker,
                                                          expect.stored):
                problems.append(f"{case.name}, boot {number}: exit {status}, saves {adds}, repair lines {notes}, "
                                f"marker {marker!r}, store holds the network {stored}; expected exit "
                                f"{expect.status}, saves {expect.adds}, repair lines {expect.notes}, marker "
                                f"{expect.marker!r}, store {expect.stored}")
            if expect.asks is not None and asks != expect.asks:
                problems.append(f"{case.name}, boot {number}: the script made {asks} calls of the Wi-Fi service; "
                                f"expected {expect.asks}")
            for call in calls:
                if call.startswith("setprop ") and call.split()[1:] not in (
                        [MARKER, "1"], [MARKER, "2"]):
                    problems.append(f"{case.name}, boot {number}: {call!r} is not the marker's 1 or 2")
    return problems


def problems_of(text: str, stop_at_first: bool = False) -> list[str]:
    with tempfile.TemporaryDirectory(prefix="wifi-network-script-text-") as directory:
        script = Path(directory) / "kernarium_wifi_network.sh"
        script.write_text(text)
        found: list[str] = []
        for case in CASES:
            found += judge(script, case)
            if found and stop_at_first:
                break
        return found


def mutated(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise AssertionError(f"the mutation target {old!r} is not in the script exactly once")
    return text.replace(old, new)


class WifiNetworkScript(unittest.TestCase):
    maxDiff = None

    def test_the_script_does_what_the_cases_say(self) -> None:
        self.assertEqual(problems_of(SCRIPT.read_text()), [])

    def test_the_test_bites(self) -> None:
        text = SCRIPT.read_text()
        early = '[ "$current" = 2 ] && exit 0\n'
        for what, mutant in (
            ("marker 1 is final", mutated(text, early, 'case "$current" in 1|2) exit 0 ;; esac\n')),
            ("a network the platform lost is not saved again after the settle", mutated(
                text, 'cmd wifi add-network VirtWifi open >/dev/null 2>&1\nlog -t',
                'look\n[ "$state" = saved ] || exit 0\ncmd wifi add-network VirtWifi open >/dev/null 2>&1\nlog -t')),
            ("one look decides at marker 1", mutated(text, "looks=3", "looks=1")),
            ("marker 2 asks the Wi-Fi service", mutated(text, early, "")),
        ):
            with self.subTest(what):
                self.assertNotEqual(problems_of(mutant, stop_at_first=True), [], what)


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--script":
        found = problems_of(Path(sys.argv[2]).read_text())
        print("\n".join(found) if found else "no case fails")
        raise SystemExit(1 if found else 0)
    unittest.main()
