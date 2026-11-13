#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify the guest's one Wi-Fi network: never disabled, rejoined after a suspend and a boot.

The platform disables a saved network at its first failed internet check, and a
disabled network is never joined again, so a guest whose first check failed has
no network from then on (android/lineage-24/README.md, "Networking"). The image
overlays the thresholds that disable it (rro/KernariumWifiOverlay) and a boot
script saves the network again, until the station has joined it once, when a
boot finds Wi-Fi on and nothing joined (overlay/tools/kernarium_wifi_network.sh).
The runner shows both:

- fresh: the module's overlay is applied (`cmd overlay list`) and holds the
  largest threshold for both no-internet reasons; the script ran without
  SELinux denials; with the validation probes pointed at a black hole, a strict
  private DNS name that does not resolve, and VirtWifi forgotten and saved
  again, the network stays enabled and joined for a minute and a half (an
  image that validates Wi-Fi without probes counts no failed check: it must
  then say so in its validation log), and is joined again after a guest
  suspend and wake (the host's route, `suspend` and `wake` on the management
  console) and after a restart; a network the user marked metered, or turned
  Auto-connect off for, keeps that after a restart (the script has nothing to
  save once the station has joined); a forgotten network is still forgotten
  after a restart; and Wi-Fi that was turned off is still off;
- upgrade (only with --previous-release): the data disk of that release, on
  which the probes and a forgotten and saved network disable VirtWifi as the
  stock platform does and which holds the network disabled, with the station
  away from it, after a restart of its own guest, boots the release under test:
  the first boot saves the network again, the station joins it, and the next
  boot leaves it alone.

The checks of one section do not stop the others, so a release that lacks the
overlay shows every symptom; the run passes only if none failed. Every wait has a
deadline. The guest is reached through the shared adb server, always named with
`-s`, and stopped through its own process on any exit.

The runner is opt-in: it needs the macos-android17 executable, an installed
release, and an adb client.
usage: run_android_wifi_network.py --emulator uml-darwin-android17 [--release DIR]
       [--previous-release DIR]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import io
import os
import pathlib
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zipfile
from collections.abc import Callable
from dataclasses import dataclass

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
ANDROID_TOOLS = REPOSITORY / "android" / "lineage-24" / "tools"
BOOT_SCRIPT = REPOSITORY / "android" / "lineage-24" / "overlay" / "tools" / "kernarium_wifi_network.sh"
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import slot_ports  # noqa: E402


def load_module(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


boot_android = load_module("boot_android", REPOSITORY / "tools" / "boot_android.py")

DEFAULT_ADB_PORT = 5584
COMMAND_SECONDS = 120
BOOT_SECONDS = 900
JOIN_SECONDS = 240
RESUME_SECONDS = 120
HOLD_SECONDS = 90
POLL_SECONDS = 5
SUSPEND_ATTEMPTS = 4
STORE_SECONDS = 30
MARGIN_SECONDS = 30
# A saved network the disk holds enabled is joined well within this after boot_completed.
BOOT_JOIN_SECONDS = 30
BLACKHOLE = "192.0.2.1"
UNRESOLVED_NAME = "no-such-host.invalid"
SSID = "VirtWifi"
OVERLAY_PACKAGE = "com.android.wifi.resources.kernarium"
OVERLAY_FILE = "/vendor/overlay/KernariumWifiOverlay.apk"
SCRIPT_LOG_TAG = "kernarium_wifi_network"
SCRIPT_DOMAIN = "kernarium_wifi_network"
SCRIPT_LOG_LINE = "VirtWifi saved again"
PROBE_SETTINGS = {
    "captive_portal_https_url": f"https://{BLACKHOLE}/generate_204",
    "captive_portal_http_url": f"http://{BLACKHOLE}/generate_204",
    "captive_portal_fallback_url": f"http://{BLACKHOLE}/gen_204",
    "private_dns_mode": "hostname",
    "private_dns_specifier": UNRESOLVED_NAME,
}
MAGIC, VERSION, DATA = 0xCAFEBABE, 2, 512


class Failure(RuntimeError):
    pass


# ---- What the guest prints, read.

def script_seconds(script: str, name: str) -> int:
    """A wait of the boot script, from its `name=` line."""
    match = re.search(rf"^{name}=(\d+)\s*$", script, re.M)
    if match is None:
        raise Failure(f"the boot script has no {name}= line")
    return int(match[1])


def boot_settle_seconds(script: str) -> int:
    """The seconds the boot script gives the station to join before it saves the network again."""
    return script_seconds(script, "settle")


def boot_rejoin_seconds(script: str) -> int:
    """The seconds the boot script waits for the station to join after it saved the network again."""
    return script_seconds(script, "rejoin")


@dataclass(frozen=True)
class Waits:
    """How long a section waits after a boot for the script, which gives the station `settle` seconds
    and, after saving the network again, `rejoin` more."""
    settle: int
    rejoin: int

    @property
    def quiet(self) -> int:
        """Long enough after a boot to have seen the script save the network again, if it would."""
        return self.settle + MARGIN_SECONDS

    @property
    def repair(self) -> int:
        """Long enough after a boot for the script to save the network again and wait for the join."""
        return self.settle + self.rejoin + MARGIN_SECONDS


def wifi_enabled(status: str) -> bool:
    """Whether `cmd wifi status` says Wi-Fi is on."""
    return "Wifi is enabled" in status.splitlines()


def wifi_joined(status: str) -> bool:
    """Whether `cmd wifi status` says the station has joined VirtWifi."""
    return f'Wifi is connected to "{SSID}"' in status.splitlines()


def saved_ssids(listing: str) -> list[str]:
    """The SSIDs `cmd wifi list-networks` lists (a network twice, once per security type, counts once)."""
    found: list[str] = []
    for line in listing.splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 3 and fields[0].isdigit() and fields[1] not in found:
            found.append(fields[1])
    return found


def network_id(listing: str) -> int | None:
    """The network ID of VirtWifi, or None when it is not saved."""
    for line in listing.splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 3 and fields[0].isdigit() and fields[1] == SSID:
            return int(fields[0])
    return None


def selection_status(dump: str) -> str | None:
    """The first network selection status `dumpsys wifi` prints, e.g. NETWORK_SELECTION_ENABLED."""
    match = re.search(r"^\s*NetworkSelectionStatus (NETWORK_SELECTION_\w+)\s*$", dump, re.M)
    return match[1] if match else None


def selection_disable_reason(dump: str) -> str | None:
    """The reason of a disabled network, or None."""
    match = re.search(r"^\s*mNetworkSelectionDisableReason (NETWORK_SELECTION_DISABLED_\w+)\s*$", dump, re.M)
    return match[1] if match else None


def no_internet_count(dump: str) -> int:
    """How many failed internet checks the platform counted against the network, summed over both reasons.

    The dump prints the network more than once, each time with its counters, so a reason counts once."""
    total = 0
    for reason in ("PERMANENT", "TEMPORARY"):
        counts = re.findall(rf"^NETWORK_SELECTION_DISABLED_NO_INTERNET_{reason} counter:(\d+)\s*$", dump, re.M)
        total += max((int(count) for count in counts), default=0)
    return total


UNPROBED_MARKER = "Wi-Fi is validated without probes"
VALIDATED_WIFI = re.compile(r"^\s*NetworkAgentInfo\{[^\n]*ni\{WIFI CONNECTED[^\n]*(?<![A-Z_])IS_VALIDATED(?![A-Z_])", re.M)


def validated_without_probes(network_stack_dump: str) -> bool:
    """`dumpsys network_stack`: the validation log of a network says the platform validated Wi-Fi without probes
    (route p0029 of the product's NetworkStack)."""
    return UNPROBED_MARKER in network_stack_dump


def wifi_validated(connectivity_dump: str) -> bool:
    """`dumpsys connectivity`: the connected Wi-Fi network is validated."""
    return VALIDATED_WIFI.search(connectivity_dump) is not None


def metered_override(dump: str) -> int | None:
    """The metered override of the saved network (0 none, 1 metered, 2 not metered)."""
    match = re.search(r"^\s*meteredOverride (\d+)\s*$", dump, re.M)
    return int(match[1]) if match else None


def autojoin_values(dump: str) -> list[bool]:
    """Auto-connect of the saved network, one value for each time `dumpsys wifi` prints it."""
    return [value == "true" for value in re.findall(r"\ballowAutojoin=(true|false)\b", dump)]


def avc_denials(log: str, domain: str) -> list[str]:
    """The SELinux denials in a log that name the domain, its executable or its property type, as the
    source or the target."""
    named = re.compile(rf"u:(?:object_)?r:{domain}(?:_exec|_prop)?:")
    return [line.strip() for line in log.splitlines()
            if "avc:" in line and "denied" in line and named.search(line)]


def repair_logged(log: str) -> bool:
    """Whether the boot script's log shows that it saved the network again."""
    return SCRIPT_LOG_LINE in log


def overlay_enabled(listing: str) -> bool:
    """Whether `cmd overlay list` shows the Wi-Fi module's overlay enabled."""
    return f"[x] {OVERLAY_PACKAGE}" in listing.splitlines()


def overlay_integers(apk: bytes) -> dict[str, list[int]]:
    """The integer resources of an overlay APK, by the verifier's reader of compiled resource tables."""
    verifier = load_verifier()
    with zipfile.ZipFile(io.BytesIO(apk)) as archive:
        return verifier.resource_integers(archive.read("resources.arsc"))


def load_verifier():
    sys.path.insert(0, str(ANDROID_TOOLS))
    try:
        return load_module("verify_target_files", ANDROID_TOOLS / "verify-target-files.py")
    finally:
        sys.path.remove(str(ANDROID_TOOLS))


def wait_for(description: str, seconds: float, check: Callable[[], str | None],
             guest: "Guest | None" = None, interval: float = 2.0) -> None:
    """Calls `check` until it returns None (done); its last message is the failure."""
    deadline = time.monotonic() + seconds
    message: str | None = "not checked"
    while time.monotonic() < deadline:
        if guest is not None:
            guest.alive()
        message = check()
        if message is None:
            return
        time.sleep(interval)
    raise Failure(f"{description}: {message} (after {seconds:.0f} s)")


class Console:
    """The guest's management console: one datagram request, one reply."""

    def __init__(self, directory: pathlib.Path, reply_directory: pathlib.Path) -> None:
        self.directory = directory
        self.reply = reply_directory / "mc"

    def request(self, command: str, seconds: float = 90) -> str:
        endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            self.reply.unlink(missing_ok=True)
            endpoint.bind(str(self.reply))
            data = command.encode()
            endpoint.sendto(struct.pack("<III", MAGIC, VERSION, len(data)) + data.ljust(DATA, b"\0"),
                            str(self.directory / "mconsole"))
            endpoint.settimeout(seconds)
            try:
                answer = endpoint.recv(12 + DATA)
            except socket.timeout:
                raise Failure(f"mconsole {command}: no reply in {seconds:.0f} s") from None
            error, _, length = struct.unpack_from("<III", answer)
            text = answer[12:12 + length].decode(errors="replace").rstrip("\0")
            if error:
                raise Failure(f"mconsole {command} failed: {text!r}")
            return text
        finally:
            endpoint.close()
            self.reply.unlink(missing_ok=True)


class Guest:
    def __init__(self, adb: str, serial: str, process: subprocess.Popen, log: pathlib.Path,
                 console: Console, private: pathlib.Path) -> None:
        self.adb = adb
        self.serial = serial
        self.process = process
        self.log = log
        self.console = console
        self.private = private
        # The boot script's denials found by every read of the log so far.
        self.denials: list[str] = []
        self.denial_reads = 0

    def adb_run(self, *arguments: str, timeout: float = COMMAND_SECONDS) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, "-s", self.serial, *arguments], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, check=False, timeout=timeout)

    def shell(self, command: str, timeout: float = COMMAND_SECONDS) -> str:
        result = self.adb_run("shell", command, timeout=timeout)
        text = result.stdout.decode(errors="replace")
        if result.returncode != 0:
            raise Failure(f"'{command}' exited {result.returncode}: {text.strip()[-500:]}")
        return text

    def alive(self) -> None:
        if self.process.poll() is not None:
            raise Failure(f"the guest exited with status {self.process.returncode}:\n"
                          + self.log.read_text(errors="replace")[-3000:])

    # What the guest says about the network.
    def status(self) -> str:
        return self.shell("cmd wifi status")

    def listing(self) -> str:
        return self.shell("cmd wifi list-networks")

    def dump(self) -> str:
        return self.shell("dumpsys wifi", timeout=180)

    def network_stack_dump(self) -> str:
        return self.shell("dumpsys network_stack", timeout=180)

    def connectivity_dump(self) -> str:
        return self.shell("dumpsys connectivity", timeout=180)

    def joined(self) -> bool:
        return wifi_joined(self.status())

    def script_log(self) -> str:
        """What the boot script logged since this boot."""
        return self.shell(f"logcat -d -s {SCRIPT_LOG_TAG}")

    def read_denials(self) -> int:
        """Keeps the boot script's SELinux denials that the log holds now; returns how many denials it
        holds in all. The log is small and rotates, so it is read more than once and nothing is dropped."""
        log = self.shell("logcat -d -b all", timeout=180)
        for line in avc_denials(log, SCRIPT_DOMAIN):
            if line not in self.denials:
                self.denials.append(line)
        self.denial_reads += 1
        return log.count("avc:")

    def uptime(self) -> float:
        return float(self.shell("cat /proc/uptime").split()[0])

    def diagnose(self) -> str:
        parts = []
        for title, command in (
                ("wifi status", "cmd wifi status | head -5"),
                ("saved networks", "cmd wifi list-networks"),
                ("selection", "dumpsys wifi | grep -E 'NetworkSelectionStatus|mNetworkSelectionDisableReason|"
                              "DISABLED_NO_INTERNET|meteredOverride' | head -12"),
                ("boot script log", f"logcat -d -s {SCRIPT_LOG_TAG} | tail -5")):
            try:
                parts.append(f"{title}:\n{self.shell(command, timeout=60).strip()}")
            except (Failure, subprocess.TimeoutExpired, OSError) as problem:
                parts.append(f"{title}: unavailable ({problem})")
        return "\n".join(parts)

    # What the runner does to the guest.
    def inject_failing_checks(self) -> None:
        """Probes at a black hole, and a strict private DNS name that does not resolve: either fails the
        platform's internet check, so the stock platform disables a network it has not validated."""
        for key, value in PROBE_SETTINGS.items():
            self.shell(f"settings put global {key} {value}")

    def forget_and_save(self) -> None:
        identifier = network_id(self.listing())
        if identifier is not None:
            self.shell(f"cmd wifi forget-network {identifier}")
        wait_for("the network is forgotten", 30, lambda: None if network_id(self.listing()) is None
                 else "still saved", self)
        self.shell(f"cmd wifi add-network {SSID} open")

    def wait_joined(self, seconds: float, what: str) -> None:
        wait_for(what, seconds, lambda: None if self.joined() else "the station has not joined", self)

    def suspend_and_wake(self) -> None:
        """The route the host's quiesce takes."""
        # A suspend can be refused while the guest is busy (EBUSY); it is asked for again.
        answer = ""
        for attempt in range(SUSPEND_ATTEMPTS):
            try:
                answer = self.console.request("suspend")
            except Failure as refused:
                if "Suspend failed" not in str(refused):
                    raise
                answer = str(refused)
            if "suspended" in answer and "failed" not in answer:
                break
            print(f"     suspend attempt {attempt + 1}: {answer!r}")
            time.sleep(10)
        else:
            raise Failure(f"suspend: refused {SUSPEND_ATTEMPTS} times: {answer!r}")
        time.sleep(15)
        answer = self.console.request("wake")
        if "running" not in answer:
            raise Failure(f"wake: the guest replied {answer!r}")
        wait_for("adb answers after the wake", RESUME_SECONDS, lambda: self.adb_answers(), self)

    def adb_answers(self) -> str | None:
        subprocess.run([self.adb, "connect", self.serial], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=30, check=False)
        try:
            return None if self.shell("echo up", timeout=30).strip() == "up" else "no answer"
        except (Failure, subprocess.TimeoutExpired):
            return "no answer"

    def restart(self) -> None:
        """A guest restart, shown by the uptime going back, then boot_completed again.

        The Wi-Fi service writes a change to its configuration store a little later, and a restart
        that follows at once loses it (a forgotten network comes back), so the restart waits."""
        time.sleep(STORE_SECONDS)
        before = self.uptime()
        self.adb_run("reboot", timeout=60)

        def rebooted() -> str | None:
            if self.adb_answers() is not None:
                return "adb is down"
            try:
                if self.uptime() >= before or self.shell("getprop sys.boot_completed").strip() != "1":
                    return "the guest has not restarted and booted"
            except (Failure, subprocess.TimeoutExpired, ValueError, IndexError):
                return "the guest does not answer"
            return None

        wait_for("the guest restarts", BOOT_SECONDS, rebooted, self, interval=3.0)


# ---- Starting and stopping a guest.

def start_guest(options: argparse.Namespace, adb: str, private: pathlib.Path, release: pathlib.Path | None,
                instance: pathlib.Path, provision: bool) -> Guest:
    boot_options = boot_android.parse_arguments([
        *(["--release", str(release)] if release else []),
        *(["--emulator", str(options.emulator)] if options.emulator else []),
        "--instance", str(instance),
        "--adb-port", str(options.adb_port),
        "--cpus", str(options.cpus),
        "--memory", options.memory,
        "--display", "offscreen",
        "--audio-output", "null",
        "--engine", "gadgets",
    ])
    emulator = boot_android.resolve_executable(boot_options.emulator)
    release_directory = boot_android.release_path(boot_options.release)
    identity = boot_android.android_instance.read_release(release_directory)
    decision = boot_android.plan_start(boot_options, identity, instance)
    boot_android.android_instance.apply(instance, identity, decision)
    command = boot_android.emulator_command(
        boot_options, release_directory, instance / boot_android.android_instance.DATA_DISK, emulator,
        boot_android.data_capacity(boot_options, identity))
    log = private / f"emulator-{instance.name}-{int(time.time())}.log"
    stream = log.open("wb")
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=stream,
                               start_new_session=True,
                               env={**os.environ, "TMPDIR": str(private), "PYTHONDONTWRITEBYTECODE": "1"})
    stream.close()
    serial = f"127.0.0.1:{options.adb_port}"
    guest = Guest(adb, serial, process, log, Console(private / f"uml-kern{process.pid}", private), private)
    try:
        boot_android.wait_for_adb(adb, serial, process, options.boot_timeout)
        if not boot_android.wait_for_boot_completed(adb, serial, process, options.boot_timeout):
            raise Failure("Android did not reach sys.boot_completed")
    except BaseException:
        stop(guest)
        raise
    # A pending update counts as applied from here, as for the launcher.
    boot_android.android_instance.booted(instance, identity)
    if provision:
        boot_android.provision_guest(adb, serial)
    return guest


def stop(guest: Guest) -> None:
    boot_android.disconnect_adb(guest.adb, guest.serial)
    boot_android.stop_guest(guest.process)


# ---- The sections.

class Report:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def section(self, name: str, guest: Guest | None, body: Callable[[], str | None]) -> None:
        try:
            note = body()
            print(f"ok   {name}" + (f" ({note})" if note else ""))
        except (Failure, boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as problem:
            print(f"FAIL {name}: {problem}", file=sys.stderr)
            if guest is not None:
                try:
                    print(guest.diagnose(), file=sys.stderr)
                except (Failure, subprocess.TimeoutExpired, OSError):
                    pass
            self.failures.append(f"{name}: {problem}")


def check_overlay(guest: Guest) -> str | None:
    listing = guest.shell("cmd overlay list --user 0")
    if not overlay_enabled(listing):
        raise Failure(f"the Wi-Fi module's overlay {OVERLAY_PACKAGE} is not enabled: {listing[-300:].strip()}")
    # adb pull is refused for /vendor/overlay; the shell may read the file.
    result = subprocess.run([guest.adb, "-s", guest.serial, "exec-out", "cat", OVERLAY_FILE],
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            check=False, timeout=60)
    if result.returncode != 0 or not result.stdout:
        raise Failure(f"cannot read {OVERLAY_FILE}: {result.stderr.decode(errors='replace').strip()[-300:]}")
    verifier = load_verifier()
    try:
        integers = overlay_integers(result.stdout)
    except (KeyError, ValueError, struct.error, IndexError, zipfile.BadZipFile) as error:
        raise Failure(f"{OVERLAY_FILE} has no readable resource table: {error}") from error
    for name in verifier.WIFI_NO_INTERNET_THRESHOLDS:
        if integers.get(name) != [verifier.WIFI_NEVER_DISABLED_THRESHOLD]:
            raise Failure(f"the overlay sets {name} to {integers.get(name)}, not "
                          f"{verifier.WIFI_NEVER_DISABLED_THRESHOLD}")
    return None


def check_saved_and_joined(guest: Guest) -> str | None:
    wait_for("VirtWifi is saved", 60, lambda: None if network_id(guest.listing()) is not None else "not saved", guest)
    # The boot script has saved the network: the denials of its first commands are read before they rotate out.
    guest.read_denials()
    guest.wait_joined(JOIN_SECONDS, "the station joins VirtWifi")
    status = selection_status(guest.dump())
    if status != "NETWORK_SELECTION_ENABLED":
        raise Failure(f"the saved network is {status}")
    return None


def check_not_disabled(guest: Guest) -> str | None:
    guest.inject_failing_checks()
    guest.forget_and_save()
    guest.wait_joined(JOIN_SECONDS, "the station joins the network saved again")
    deadline = time.monotonic() + HOLD_SECONDS
    dump = ""
    while time.monotonic() < deadline:
        guest.alive()
        dump = guest.dump()
        status = selection_status(dump)
        if status != "NETWORK_SELECTION_ENABLED":
            raise Failure(f"the network became {status} ({selection_disable_reason(dump)}) after a failed "
                          f"internet check (counted {no_internet_count(dump)})")
        time.sleep(POLL_SECONDS)
    if not guest.joined():
        raise Failure("the station left the network")
    counted = no_internet_count(dump)
    if counted < 1:
        # A platform that probes counts the injected failures; if it counted none they never reached it and the
        # network was never at risk. This image validates Wi-Fi without probes (route p0029), so nothing is ever
        # counted: what shows that the network was not at risk is the platform's own statement that it validated
        # without probes, and the network staying enabled and joined.
        if not validated_without_probes(guest.network_stack_dump()):
            raise Failure("the platform counted no failed internet check and does not say that it validated Wi-Fi "
                          "without probes, so the injected failures did not reach it and the network was never at risk")
        # Whether the network is validated is only reported: the injected strict private DNS name does not resolve,
        # which the platform's validation (it resolves private DNS after skipping the probes) reports as not validated.
        state = "validated" if wifi_validated(guest.connectivity_dump()) else "not validated (the injected private DNS name does not resolve)"
        return f"no failed internet check counted: the platform validates Wi-Fi without probes; the network stays enabled and joined, {state}"
    return f"{counted} failed internet checks counted, none disabled the network"


def check_suspend(guest: Guest) -> str | None:
    guest.suspend_and_wake()
    guest.wait_joined(RESUME_SECONDS, "the station joins again after the wake")
    status = selection_status(guest.dump())
    if status != "NETWORK_SELECTION_ENABLED":
        raise Failure(f"the saved network is {status} after the wake")
    return None


def check_restart(guest: Guest) -> str | None:
    guest.restart()
    guest.wait_joined(JOIN_SECONDS, "the station joins after the restart")
    status = selection_status(guest.dump())
    if status != "NETWORK_SELECTION_ENABLED":
        raise Failure(f"the saved network is {status} after the restart")
    return None


def check_no_repair_logged(guest: Guest, when: str) -> None:
    if repair_logged(guest.script_log()):
        raise Failure(f"the boot script saved the network again {when}")


def check_metered_kept(guest: Guest, waits: Waits) -> str | None:
    guest.shell(f"cmd wifi add-network {SSID} open -m")
    wait_for("the metered network is joined", JOIN_SECONDS, lambda: None if guest.joined() else "not joined", guest)
    if metered_override(guest.dump()) != 1:
        raise Failure("the network is not marked metered")
    guest.restart()
    guest.wait_joined(JOIN_SECONDS, "the station joins after the restart")
    time.sleep(waits.quiet)
    if metered_override(guest.dump()) != 1:
        raise Failure("the boot script saved a joined network again and reset its metered setting")
    check_no_repair_logged(guest, "although the station had joined")
    return None


def check_autojoin_kept(guest: Guest, waits: Waits) -> str | None:
    """Auto-connect turned off is a choice of the user's that no boot undoes (saving the network replaces it)."""
    guest.shell(f"cmd wifi add-network {SSID} open -d")
    def autojoin_off() -> str | None:
        values = autojoin_values(guest.dump())
        return None if values and not any(values) else "still on"

    wait_for("Auto-connect is off", 60, autojoin_off, guest)
    guest.restart()
    time.sleep(waits.quiet)
    dump = guest.dump()
    if any(autojoin_values(dump)):
        raise Failure("Auto-connect is on again after a restart: the boot script saved the network again")
    check_no_repair_logged(guest, "although the user turned Auto-connect off")
    return None


def check_forgotten_stays_forgotten(guest: Guest, waits: Waits) -> str | None:
    identifier = network_id(guest.listing())
    if identifier is None:
        raise Failure("VirtWifi is not saved")
    guest.shell(f"cmd wifi forget-network {identifier}")
    wait_for("the network is forgotten", 30, lambda: None if network_id(guest.listing()) is None else "saved", guest)
    guest.restart()
    time.sleep(waits.quiet)
    if SSID in saved_ssids(guest.listing()):
        raise Failure("VirtWifi is saved again after a restart although the user forgot it")
    check_no_repair_logged(guest, "although the user forgot the network")
    if not wifi_enabled(guest.status()):
        raise Failure("Wi-Fi is not on")
    return None


def check_wifi_off_stays_off(guest: Guest, waits: Waits) -> str | None:
    guest.shell(f"cmd wifi add-network {SSID} open")
    guest.wait_joined(JOIN_SECONDS, "the station joins the network saved again")
    guest.shell("cmd wifi set-wifi-enabled disabled")
    wait_for("Wi-Fi is off", 60, lambda: None if not wifi_enabled(guest.status()) else "still on", guest)
    guest.restart()
    time.sleep(waits.quiet)
    if wifi_enabled(guest.status()):
        raise Failure("Wi-Fi is on after a restart although the user turned it off")
    check_no_repair_logged(guest, "although Wi-Fi was off")
    if network_id(guest.listing()) is None:
        raise Failure("the saved network is gone")
    return None


def check_no_denials(guest: Guest) -> str | None:
    """The boot script runs in a domain of its own (overlay/sepolicy/system_ext/private); a rule it lacks
    shows as a denial, and the script then fails without a word. The denials earlier reads of this guest's
    log found count too, since the log may have rotated them out since."""
    logged = guest.read_denials()
    if guest.denials:
        raise Failure(f"SELinux denied the boot script's domain {len(guest.denials)} times:\n"
                      + "\n".join(guest.denials[:5]))
    return f"{logged} denials logged in all, none for {SCRIPT_DOMAIN} in {guest.denial_reads} reads of the log"


def fresh(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report, waits: Waits) -> None:
    print("fresh data disk")
    guest = start_guest(options, adb, private, options.release, private / "fresh", provision=True)
    try:
        report.section("overlay is applied", guest, lambda: check_overlay(guest))
        report.section("VirtWifi is saved and joined at the first boot", guest, lambda: check_saved_and_joined(guest))
        report.section("the boot script runs without SELinux denials", guest, lambda: check_no_denials(guest))
        report.section("a failed internet check does not disable the network", guest,
                       lambda: check_not_disabled(guest))
        report.section("the station joins again after a suspend and wake", guest, lambda: check_suspend(guest))
        report.section("the station joins again after a restart", guest, lambda: check_restart(guest))
        report.section("a joined network keeps its metered setting across a restart", guest,
                       lambda: check_metered_kept(guest, waits))
        report.section("a network with Auto-connect off keeps it across a restart", guest,
                       lambda: check_autojoin_kept(guest, waits))
        report.section("a forgotten network stays forgotten", guest,
                       lambda: check_forgotten_stays_forgotten(guest, waits))
        report.section("Wi-Fi that is off stays off", guest, lambda: check_wifi_off_stays_off(guest, waits))
    finally:
        stop(guest)


def check_repaired(guest: Guest, waits: Waits) -> str | None:
    """The main log buffer is small and nearly full at boot, so the script's repair line is looked for at
    every poll and kept once seen, and its denials are read early as well as by the later check."""
    state = selection_status(guest.dump())
    logged = repair_logged(guest.script_log())
    print(f"     the network is {state} when the release under test has booted")
    # The script has polled the station by now.
    guest.read_denials()
    if state != "NETWORK_SELECTION_PERMANENTLY_DISABLED" and not logged:
        raise Failure(f"the data disk holds the network {state}, not disabled: the previous release's disabled "
                      "state did not reach the disk, so no repair is shown")

    def joined() -> str | None:
        nonlocal logged
        station = guest.joined()
        if not logged and repair_logged(guest.script_log()):
            logged = True
            # The script has saved the network again and logged it.
            guest.read_denials()
        return None if station else "the station has not joined"

    wait_for("the first boot of the release joins the network", waits.repair, joined, guest, interval=POLL_SECONDS)
    status = selection_status(guest.dump())
    if status != "NETWORK_SELECTION_ENABLED":
        raise Failure(f"the network joined but is {status}")
    if not logged and not repair_logged(guest.script_log()):
        raise Failure("the network joined without the boot script saving it again")
    return None


def check_disabled_on_disk(guest: Guest) -> None:
    """The previous release's disk holds the network disabled: after a restart of its guest the station
    stays away from the network and the dump says it is disabled. With the checks still failing, a
    network the disk held enabled is joined at boot and disabled again in memory, which a dump alone does
    not tell apart, so the station is watched first."""
    guest.restart()
    deadline = time.monotonic() + BOOT_JOIN_SECONDS
    while True:
        guest.alive()
        if guest.joined():
            raise Failure("the station joined the network after a restart of the previous release, so its "
                          "disk holds the network enabled")
        if time.monotonic() >= deadline:
            break
        time.sleep(POLL_SECONDS)
    state = selection_status(guest.dump())
    if state != "NETWORK_SELECTION_PERMANENTLY_DISABLED":
        raise Failure(f"the network is {state} after a restart of the previous release, so its "
                      "disabled state did not reach the disk")
    if guest.joined():
        raise Failure("the station joined the network after a restart of the previous release")


def upgrade(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report, waits: Waits) -> None:
    previous = boot_android.release_path(options.previous_release)
    current = boot_android.release_path(options.release)
    if previous == current:
        print("upgrade: the previous release is the release under test; nothing to upgrade from")
        return
    print(f"upgrade: a data disk of {previous.name} booted by {current.name}")
    instance = private / "upgrade"
    guest = start_guest(options, adb, private, previous, instance, provision=True)
    disabled = False
    try:
        def disable() -> str | None:
            nonlocal disabled
            wait_for("VirtWifi is saved", 60, lambda: None if network_id(guest.listing()) is not None
                     else "not saved", guest)
            guest.inject_failing_checks()
            guest.forget_and_save()
            guest.wait_joined(JOIN_SECONDS, "the station joins the network saved again")
            wait_for("the previous release disables the network", HOLD_SECONDS + JOIN_SECONDS,
                     lambda: None if selection_status(guest.dump()) == "NETWORK_SELECTION_PERMANENTLY_DISABLED"
                     else "still enabled", guest, interval=POLL_SECONDS)
            # The Wi-Fi service stores the change a few seconds later, and the disk the release under test
            # boots holds only what was stored: a restart of this guest shows what that is.
            check_disabled_on_disk(guest)
            disabled = True
            return None

        report.section("the previous release leaves VirtWifi disabled on its disk", guest, disable)
    finally:
        stop(guest)
    if not disabled:
        return
    guest = start_guest(options, adb, private, options.release, instance, provision=False)
    try:
        report.section("the first boot of the release repairs the network", guest,
                       lambda: check_repaired(guest, waits))
        report.section("the boot script runs without SELinux denials", guest, lambda: check_no_denials(guest))
        report.section("the repaired network joins again after a suspend and wake", guest,
                       lambda: check_suspend(guest))
        report.section("the next boot leaves the repaired network alone", guest,
                       lambda: check_autojoin_kept(guest, waits))
    finally:
        stop(guest)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", type=pathlib.Path,
                        help="the macos-android17 uml-darwin-android17 (the launcher's default)")
    parser.add_argument("--release", type=pathlib.Path, help="installed Android release under test (default: accepted)")
    parser.add_argument("--previous-release", type=pathlib.Path,
                        help="installed release whose data disk the upgrade section boots first (the section "
                        "runs only when this is given)")
    parser.add_argument("--adb", default="adb", help="adb client to use")
    parser.add_argument("--adb-port", type=int, default=slot_ports.slot_port(slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_ADB_PORT})")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--memory", default="4G")
    parser.add_argument("--boot-timeout", type=int, default=BOOT_SECONDS,
                        help="seconds to wait for adbd and for boot_completed")
    return parser


def main() -> int:
    options = build_parser().parse_args()
    if options.cpus <= 0 or options.boot_timeout <= 0:
        print("counts and timeouts must be positive", file=sys.stderr)
        return 2
    adb = str(boot_android.resolve_executable(options.adb, sdk_fallback=True))
    script = BOOT_SCRIPT.read_text(encoding="utf-8")
    waits = Waits(boot_settle_seconds(script), boot_rejoin_seconds(script))
    # Short, so the management console's socket path stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-awn-"))
    report = Report()
    try:
        fresh(options, adb, private, report, waits)
        if options.previous_release is not None:
            upgrade(options, adb, private, report, waits)
    except (Failure, boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        report.failures.append(str(problem))
    finally:
        shutil.rmtree(private, ignore_errors=True)
    if report.failures:
        print("FAIL: " + "; ".join(report.failures), file=sys.stderr)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
