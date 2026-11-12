#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify the guest's one Wi-Fi network: never disabled, a failed check only warns, rejoined after a suspend and a boot.

The platform disables a saved network at its first failed internet check, and a
disabled network is never joined again, so a guest whose first check failed has
no network from then on (android/lineage-24/README.md, "Networking"). The image
overlays the thresholds that disable it (rro/KernariumWifiOverlay) and a boot
script saves the network again, until the station has joined it once, when a
boot finds Wi-Fi on and nothing joined (overlay/tools/kernarium_wifi_network.sh).
The platform validates the network before it checks it (routes p0029 and p0033):
a failed check, and a sign-in or block page in place of the expected answer, only
show Android's own Wi-Fi mark. The runner shows all of it:

- fresh: the module's overlay is applied (`cmd overlay list`) and holds the
  largest threshold for both no-internet reasons; the script ran without
  SELinux denials; with the validation probes at a black hole, the network stays
  validated and joined while Settings says "Limited connection", the status bar's
  Wi-Fi icon says "No internet", the validation log says the check failed and no
  notification or sign-in activity follows, also after a guest suspend and wake
  (a new network, with a higher ID); a sign-in page (a redirect) and a block page
  (a 200 answer) that the host's server gives the probes through `adb reverse`
  show the same and nothing more, and the current network's own probe log names
  the section's URL and the answer it received; on one network that is never
  replaced or validated again (`lastValidated` unchanged), with the Internet
  page in front and no other action, the network is validated before its first
  check, the warning shows in Settings and in the status bar, the check comes
  back after the stock back-off, and when the server answers 204 the warning
  goes from both and a new passing check is in the log; a forced re-evaluation
  (`cmd connectivity reevaluate`, whose failure fails the section) brings it back
  and the next passing check takes it away; a passing check on a network that
  has just joined, by the host's server and by the stock servers, leaves no
  warning; with a strict private DNS name the platform can use, a check that
  fails again and again leaves the network validated with its private DNS not
  broken, and the private DNS probe is not repeated for each check; across all
  of it NetworkStack, SystemUI and Settings (each by the exact name it runs
  under) neither crash nor stop answering and nothing hits an illegal
  instruction, which every section of a guest of the release under test reads
  from the marker it wrote at its start (a section whose marker the log has
  dropped fails); with the
  probes at a black hole and a strict private DNS name that does not resolve,
  and VirtWifi forgotten and saved again, the network stays enabled and joined for
  a minute and a half, and is joined again after a guest suspend and wake (the
  host's route, `suspend` and `wake` on the management console) and after a
  restart; a network the user marked metered, or turned Auto-connect off for,
  keeps that after a restart (the script has nothing to save once the station
  has joined); a forgotten network is still forgotten after a restart; and Wi-Fi
  that was turned off is still off;
- upgrade (only with --previous-release, once for each release): the data disk
  of a release whose platform disables the network for lack of internet (the
  stock rules), on which the probes and a forgotten and saved network disable
  VirtWifi and which holds the network disabled, with the station away from it,
  after a restart of its own guest, boots the release under test: the first boot
  saves the network again, the station joins it, the next boot leaves it alone,
  and a failed check warns. The data disk of a release that never disables the
  network, with the network joined, the user's metered setting and the probes at
  a black hole, boots the release under test: nothing saves the network again,
  the setting and the join are kept, and a failed check warns.

The checks of one section do not stop the others, so a release that lacks the
overlay shows every symptom; the run passes only if none failed. Every wait has a
deadline. The guest is reached through the shared adb server, always named with
`-s`, and stopped through its own process on any exit.

The runner is opt-in: it needs the macos-android17 executable, an installed
release, and an adb client.
usage: run_android_wifi_network.py --emulator uml-darwin-android17 [--release DIR]
       [--previous-release DIR]...
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import http.server
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
import threading
import time
import uuid
import zipfile
from collections.abc import Callable
from dataclasses import dataclass
from xml.etree import ElementTree

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
# What fails the check alone, leaving the network itself alone: a strict private DNS name that does not resolve
# fails the network (the platform reports it invalid), so the warning sections leave the private DNS settings out.
CHECK_SETTINGS = {key: value for key, value in PROBE_SETTINGS.items() if not key.startswith("private_dns")}
PRIVATE_DNS_SETTINGS = ("private_dns_mode", "private_dns_specifier")
USE_HTTPS_SETTING = "captive_portal_use_https"
ALL_CHECK_SETTINGS = (*PROBE_SETTINGS, USE_HTTPS_SETTING)
MARKER_PROPERTY = "persist.sys.kernarium.virtwifi_saved"
# The validation log lines of the NetworkMonitor topic of route p0029 (the verifier requires them in the image).
CHECK_FAILED_LINE = "Wi-Fi internet check failed, the network stays validated"
CHECK_PASSED_LINE = "Wi-Fi internet check passed"
VALIDATED_FIRST_LINE = "Wi-Fi is validated before its internet check"
PRIVATE_DNS_PROBE = "PROBE_PRIVDNS "
# A private DNS server that serves the strict name the platform probes (the host must reach it, as it must
# reach the stock servers for the section that takes the warning away with them).
PRIVATE_DNS_HOST = "dns.google"
# The checks of one network that a section counts for the back-off (1 s, 2 s, 4 s between the first four),
# and how close to the stock delay a gap between two checks must be.
RETRIES_NEEDED = 4
INITIAL_BACKOFF_SECONDS = 1.0
BACKOFF_FLOOR = 0.9
# How long a forced re-evaluation has to show: a new check at the server.
FORCE_SECONDS = 45
# Android's own words: the Wi-Fi list's summary and the status bar icon's spoken description.
LIMITED_CONNECTION = "Limited connection"
NO_INTERNET = "No internet"
SYSTEMUI_PACKAGE = "com.android.systemui"
UI_DUMP = "/data/local/tmp/wifi-network-ui.xml"
# From the join to the mark: a blocked check takes about 10 to 25 s, and Settings redraws on the report.
WARNING_SECONDS = 90
# How long a warning is held against a disabled, disconnected, unvalidated or notified network.
WARNING_HOLD_SECONDS = 60
SHORT_HOLD_SECONDS = 30
PAGE_BYTES = 2048
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


def wifi_networks(dump: str) -> dict[int, frozenset[str]]:
    """The capabilities of each Wi-Fi network `dumpsys connectivity` lists, by network ID."""
    found: dict[int, frozenset[str]] = {}
    for line in dump.splitlines():
        match = re.match(r"\s*NetworkAgentInfo\{network\{(\d+)\}.*?\bni\{WIFI\b.*?"
                         r"\bnc\{\[ Transports: WIFI Capabilities: ([A-Z_&]+)", line)
        if match:
            found[int(match[1])] = frozenset(match[2].split("&"))
    return found


def last_validated(dump: str) -> dict[int, int]:
    """When the platform last gave each Wi-Fi network the validated capability (`lastValidated`, which it
    sets only when the capability changes, so it stays put while a check reports on a validated network)."""
    found: dict[int, int] = {}
    for line in dump.splitlines():
        match = re.match(r"\s*NetworkAgentInfo\{network\{(\d+)\}.*?\bni\{WIFI\b.*?\blastValidated (\d+)", line)
        if match:
            found[int(match[1])] = int(match[2])
    return found


def newest_wifi_network(dump: str) -> int | None:
    """The ID of the Wi-Fi network `dumpsys connectivity` lists with the highest ID (the one the station
    joined last), or None."""
    return max(wifi_networks(dump), default=None)


def validation_problems(capabilities: frozenset[str] | None) -> list[str]:
    """What is wrong with a Wi-Fi network that must stay validated: not listed, not validated, partial,
    behind a sign-in page, or with its private DNS reported broken."""
    if capabilities is None:
        return ["dumpsys connectivity lists no Wi-Fi network"]
    problems = []
    if "VALIDATED" not in capabilities:
        problems.append("the network is not validated")
    for name in ("PARTIAL_CONNECTIVITY", "CAPTIVE_PORTAL", "PRIVATE_DNS_BROKEN"):
        if name in capabilities:
            problems.append(f"the network has {name}")
    return problems


def validation_entries(dump: str) -> dict[int, list[tuple[str, str]]]:
    """The entries (time stamp, message) of each network's validation log in `dumpsys network_stack`, by
    network ID, oldest first."""
    logs: dict[int, list[tuple[str, str]]] = {}
    current: list[tuple[str, str]] | None = None
    in_section = False
    for line in dump.splitlines():
        if line.startswith("Validation logs"):
            in_section = True
            continue
        if not in_section:
            continue
        header = re.fullmatch(r'(\d+) - ".*"\s*', line)
        if header:
            current = logs.setdefault(int(header[1]), [])
            continue
        entry = re.fullmatch(r"\s+(\d{4}-\d\d-\d\dT[\d:.]+) - (.*)", line)
        if entry and current is not None:
            current.append((entry[1], entry[2]))
        elif line.strip():
            current = None
            if not line.startswith(" "):
                in_section = False
    return logs


def validation_logs(dump: str) -> dict[int, list[str]]:
    """The messages of each network's validation log in `dumpsys network_stack`, by network ID."""
    return {net_id: [message for _, message in entries] for net_id, entries in validation_entries(dump).items()}


class LogUnion:
    """The entries of a validation log collected over several polls. The log keeps 20 lines, so a section
    that counts over a long time reads it often and keeps each entry (its time stamp and message) once."""

    def __init__(self) -> None:
        self.entries: dict[tuple[str, str], None] = {}

    def add(self, entries: list[tuple[str, str]]) -> None:
        for entry in entries:
            self.entries.setdefault(entry, None)

    def count(self, text: str) -> int:
        return sum(1 for _, message in self.entries if text in message)


def check_log_problems(messages: list[str], expected: str, probes_failed: bool = False) -> list[str]:
    """What the validation log of the network lacks: the line of the check's result, and with `probes_failed`
    a failed HTTP and a failed HTTPS probe (a page answer ends the probing before the HTTPS probe fails)."""
    problems = []
    if not any(expected in message for message in messages):
        problems.append(f"the validation log has no line {expected!r}")
    if probes_failed:
        for probe in ("PROBE_HTTP", "PROBE_HTTPS"):
            if not any(message.startswith(f"{probe} ") and "Probe failed" in message for message in messages):
                problems.append(f"the validation log shows no failed {probe}")
    return problems


def first_validation_problems(messages: list[str]) -> list[str]:
    """Whether the network was validated before its first check: the log's reason line comes before any
    probe of the check (the private DNS probe of a strict name comes before the validation, as stock)."""
    reason = next((index for index, message in enumerate(messages) if VALIDATED_FIRST_LINE in message), None)
    probe = next((index for index, message in enumerate(messages)
                  if re.match(r"PROBE_(?!PRIVDNS)\w+ ", message)), None)
    if reason is None:
        return [f"the validation log has no line {VALIDATED_FIRST_LINE!r}: the network was not validated "
                "before its check"]
    if probe is not None and probe < reason:
        return ["a probe of the check completed before the network was validated"]
    return []


def passed_after_failed_problems(messages: list[str]) -> list[str]:
    """A new passing check after the failed ones: the last passing line comes after the last failed one."""
    passed = [index for index, message in enumerate(messages) if CHECK_PASSED_LINE in message]
    failed = [index for index, message in enumerate(messages) if CHECK_FAILED_LINE in message]
    if not passed:
        return [f"the validation log has no line {CHECK_PASSED_LINE!r}"]
    if failed and max(failed) > max(passed):
        return ["the last check in the validation log failed after the one that passed"]
    return []


def probe_answer_problems(messages: list[str], scenario: str, code: int) -> list[str]:
    """Whether the current network's HTTP probe of this scenario's URL received the answer `code`: a line
    of the validation log names the URL and the code (`ret=`), and is not a probe that failed (a request
    that the server saw and the probe timed out on is no answer)."""
    lines = [message for message in messages if message.startswith("PROBE_HTTP ") and scenario in message]
    if not lines:
        return [f"the validation log shows no HTTP probe of {scenario}"]
    answered = [line for line in lines if f" ret={code} " in line and "Probe failed" not in line]
    if not answered:
        failures = [line for line in lines if "Probe failed" in line]
        if failures:
            return [f"the HTTP probe of {scenario} failed without an answer: {failures[-1][:160]}"]
        return [f"the HTTP probe of {scenario} did not receive {code}: {lines[-1][:160]}"]
    if code == 200 and any("interpreted as failed" in line and scenario in line for line in messages):
        return [f"the 200 answer of {scenario} was taken for a failed answer, not a page"]
    return []


def retry_problems(times: list[float]) -> list[str]:
    """The first checks of a network that fails them come back after the stock back-off: with the times at
    which their requests reached the server, at least RETRIES_NEEDED of them, and each gap at least as long
    as the delay that follows the failure before it (1 s, 2 s, 4 s)."""
    if len(times) < RETRIES_NEEDED:
        return [f"only {len(times)} of {RETRIES_NEEDED} checks reached the server"]
    problems = []
    for index in range(1, RETRIES_NEEDED):
        gap = times[index] - times[index - 1]
        floor = BACKOFF_FLOOR * INITIAL_BACKOFF_SECONDS * 2 ** (index - 1)
        if gap < floor:
            problems.append(f"check {index + 1} came {gap:.1f} s after the one before, under the back-off "
                            f"of {floor:.1f} s")
    return problems


def connectivity_notifications(dump: str) -> list[str]:
    """The records `dumpsys notification` lists that ConnectivityService posted (no Internet, partial
    connectivity, lost Internet, Sign in to network all carry the tag ConnectivityNotification:<netId>)."""
    return [line.strip()[:200] for line in dump.splitlines()
            if re.search(r"\btag=ConnectivityNotification:\d+", line)]


def captive_portal_activities(dump: str) -> list[str]:
    """The lines of `dumpsys activity activities` that name the captive portal login activity."""
    return [line.strip()[:200] for line in dump.splitlines()
            if re.search(r"captiveportallogin|CaptivePortalLogin", line)]


@dataclass(frozen=True)
class UiNode:
    text: str
    description: str
    package: str


def ui_nodes(hierarchy: str) -> list[UiNode]:
    """The nodes of a `uiautomator dump --windows` (the status bar is a window of its own)."""
    start = hierarchy.find("<?xml")
    if start < 0:
        start = hierarchy.find("<")
    if start < 0:
        raise Failure(f"the UI dump is not XML: {hierarchy[:200]!r}")
    # What the tool prints after the document ("UI hierarchy dumped to: ...") is not part of it.
    end = -1
    for closing in ("</displays>", "</hierarchy>"):
        found = hierarchy.rfind(closing)
        if found >= 0 and found + len(closing) > end:
            end = found + len(closing)
    try:
        root = ElementTree.fromstring(hierarchy[start:end] if end >= 0 else hierarchy[start:])
    except ElementTree.ParseError as error:
        raise Failure(f"the UI dump is not XML: {error}") from error
    return [UiNode(node.get("text", ""), node.get("content-desc", ""), node.get("package", ""))
            for node in root.iter("node")]


def summary_limited(nodes: list[UiNode]) -> bool:
    """Whether the Wi-Fi list says "Limited connection" (its summary reads "Connected / Limited connection")."""
    return any(LIMITED_CONNECTION in node.text for node in nodes)


def summary_connected(nodes: list[UiNode]) -> bool:
    """Whether the Wi-Fi list shows the network as plain "Connected"."""
    return any(node.text == "Connected" for node in nodes)


def status_bar_no_internet(nodes: list[UiNode]) -> bool:
    """Whether the status bar's Wi-Fi icon speaks "No internet" (its description is the signal and "No internet")."""
    return any(node.package == SYSTEMUI_PACKAGE and NO_INTERNET in node.description for node in nodes)


def status_bar_present(nodes: list[UiNode]) -> bool:
    """Whether the dump holds the status bar's Wi-Fi icon at all (a mark is judged absent only where it
    could be seen)."""
    return any(node.package == SYSTEMUI_PACKAGE and re.search(r"wi-?fi", node.description, re.I)
               for node in nodes)


def mark_problems(nodes: list[UiNode], shown: bool) -> list[str]:
    """What the screen shows against what Android's Wi-Fi mark must say: with `shown`, the list's "Limited
    connection" and the status bar's "No internet"; without, plain "Connected" and no "No internet", the
    status bar's Wi-Fi icon being in the dump."""
    problems = []
    if shown:
        if not summary_limited(nodes):
            problems.append(f"the Wi-Fi list does not say {LIMITED_CONNECTION!r}")
        if not status_bar_no_internet(nodes):
            problems.append(f"the status bar's Wi-Fi icon does not say {NO_INTERNET!r}")
    else:
        if summary_limited(nodes):
            problems.append(f"the Wi-Fi list still says {LIMITED_CONNECTION!r}")
        if not summary_connected(nodes):
            problems.append("the Wi-Fi list does not say 'Connected'")
        if not status_bar_present(nodes):
            problems.append("the dump has no status bar Wi-Fi icon, so its mark cannot be judged absent")
        elif status_bar_no_internet(nodes):
            problems.append(f"the status bar's Wi-Fi icon still says {NO_INTERNET!r}")
    return problems


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


class ProbeServer:
    """The host's answer to the guest's probes: a sign-in page (a redirect), a block page (a 200 answer with a
    body) or the expected 204. The guest cannot reach the machine the runner runs on (the provider refuses
    its connections to every address of a host interface, darwin/README.md), so the guest reaches the server
    through `adb reverse` on its own loopback, as any host page server is reached."""

    MODES = ("redirect", "page", "expected")

    def __init__(self) -> None:
        self.mode = "expected"
        self.seen: list[tuple[float, str, int]] = []
        self.lock = threading.Lock()
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:  # noqa: N802
                mode = server.mode
                server.record(self.path, {"redirect": 302, "page": 200}.get(mode, 204))
                if mode == "redirect":
                    self.send_response(302)
                    self.send_header("Location", f"http://127.0.0.1:{server.port}/login")
                    self.send_header("Content-Length", "0")
                    body = b""
                elif mode == "page":
                    body = server.page()
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html")
                    self.send_header("Content-Length", str(len(body)))
                else:
                    self.send_response(204)
                    body = b""
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)

            do_HEAD = do_GET  # noqa: N815

            def log_message(self, *arguments: object) -> None:
                pass

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    @staticmethod
    def page() -> bytes:
        """A page that is not the expected answer: 2 KiB of HTML."""
        head = b"<html><head><title>Terms of use</title></head><body><p>Sign in to continue.</p><!--"
        tail = b"--></body></html>"
        return head + b"x" * (PAGE_BYTES - len(head) - len(tail)) + tail

    @staticmethod
    def scenario() -> str:
        """A path prefix no other section's probes use, for the URLs of one section."""
        return "/s" + uuid.uuid4().hex[:8]

    def record(self, path: str, status: int = 0, when: float | None = None) -> None:
        """A request the server saw, with the status it answers with, and when."""
        with self.lock:
            self.seen.append((time.monotonic() if when is None else when, path, status))

    def requests(self, scenario: str | None = None) -> list[tuple[float, str, int]]:
        """The requests (time, path, status), in order, of one scenario or all."""
        with self.lock:
            return [seen for seen in self.seen if scenario is None or seen[1].startswith(scenario + "/")]

    def probes(self, scenario: str | None = None) -> list[str]:
        """The paths the guest's probes asked for, in order."""
        return [path for _, path, _ in self.requests(scenario)]

    def checks(self, scenario: str) -> list[float]:
        """When the HTTP probes of a scenario arrived: one for each check."""
        return [when for when, path, _ in self.requests(scenario) if path.endswith("/generate_204")]

    def clear(self) -> None:
        with self.lock:
            self.seen.clear()

    def close(self) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=10)


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
    crash_log: "CrashLog | None" = None

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
        # Set for a guest of the release under test: its sections read their crash evidence.
        self.crash_log: CrashLog | None = None

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

    def connectivity(self) -> str:
        return self.shell("dumpsys connectivity", timeout=180)

    def network_stack(self) -> str:
        return self.shell("dumpsys network_stack", timeout=180)

    def notifications(self) -> str:
        return self.shell("dumpsys notification --noredact", timeout=180)

    def activities(self) -> str:
        return self.shell("dumpsys activity activities", timeout=180)

    def ui(self) -> str:
        """The windows on the screen: Settings, and the status bar as a window of its own."""
        return self.shell(f"uiautomator dump --windows {UI_DUMP} >/dev/null 2>&1; cat {UI_DUMP} 2>/dev/null; "
                          f"rm -f {UI_DUMP}", timeout=90)

    def marker(self) -> str:
        """The boot script's marker: 1 once the network is saved, 2 once the station has joined it."""
        return self.shell(f"getprop {MARKER_PROPERTY}").strip()

    def validation_entries(self, net_id: int) -> list[tuple[str, str]]:
        return validation_entries(self.network_stack()).get(net_id, [])

    def validation_log(self, net_id: int) -> list[str]:
        return [message for _, message in self.validation_entries(net_id)]

    def reevaluate(self, net_id: int) -> None:
        """Forces the platform to check the network again (`cmd connectivity reevaluate`, which reports the
        opposite of what the network is and so makes the platform evaluate it). A refusal, an unknown network
        or any error is a failure: a section that forces a check must see it run."""
        result = self.adb_run("shell", f"cmd connectivity reevaluate {net_id}", timeout=60)
        output = result.stdout.decode(errors="replace").strip()
        if result.returncode != 0 or re.search(r"Unknown network|Exception|Error|denied|not found", output, re.I):
            raise Failure(f"cmd connectivity reevaluate {net_id} (rc {result.returncode}): {output[-300:]}")

    def reverse(self, port: int) -> None:
        """The guest's loopback port to the host's server (the shared adb server, named with -s)."""
        result = self.adb_run("reverse", f"tcp:{port}", f"tcp:{port}", timeout=60)
        if result.returncode != 0:
            raise Failure(f"adb reverse tcp:{port}: {result.stdout.decode(errors='replace').strip()[-300:]}")

    def unreverse(self, port: int) -> None:
        self.adb_run("reverse", "--remove", f"tcp:{port}", timeout=60)

    def show_internet_page(self) -> None:
        """Settings' Internet page, in front, on a lit screen."""
        for command in ("svc power stayon true", "input keyevent KEYCODE_WAKEUP", "wm dismiss-keyguard"):
            self.shell(f"{command} >/dev/null 2>&1; true")
        started = self.shell("am start -W -a android.settings.WIFI_SETTINGS")
        if "Error" in started:
            raise Failure(f"starting the Internet page of Settings: {started.strip()[-300:]}")

    def leave_internet_page(self) -> None:
        self.shell("input keyevent KEYCODE_HOME >/dev/null 2>&1; true")

    # What the runner does to the guest.
    def inject_failing_checks(self) -> None:
        """Probes at a black hole, and a strict private DNS name that does not resolve: either fails the
        platform's internet check, so the stock platform disables a network it has not validated."""
        for key, value in PROBE_SETTINGS.items():
            self.shell(f"settings put global {key} {value}")

    def put_settings(self, settings: dict[str, str]) -> None:
        for key, value in settings.items():
            self.shell(f"settings put global {key} {value}")

    def delete_settings(self, keys: tuple[str, ...] | list[str]) -> None:
        for key in keys:
            self.shell(f"settings delete global {key}")

    def read_setting(self, key: str) -> str:
        return self.shell(f"settings get global {key}").strip()

    def rejoin_with_settings(self, settings: dict[str, str], clear: tuple[str, ...]) -> None:
        """Sets the probe settings and deletes `clear`, then joins the network again: the platform reads them
        when the network's monitor starts, so they take effect only for a network that is joined after."""
        self.delete_settings(clear)
        self.put_settings(settings)
        self.forget_and_save()
        self.wait_joined(JOIN_SECONDS, "the station joins the network saved again")

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
        that follows at once loses it (a forgotten network comes back), so the restart waits. A restart
        clears the log, so the crash evidence of the section is read before it and a new marker follows it."""
        time.sleep(STORE_SECONDS)
        watch = self.crash_log
        watching = watch is not None and watch.active
        if watching:
            watch.collect(self)
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
        # The restart cleared the log: the rest of the section is read from a new marker.
        if watching:
            watch.mark(self)


# ---- Starting and stopping a guest.

def start_guest(options: argparse.Namespace, adb: str, private: pathlib.Path, release: pathlib.Path | None,
                instance: pathlib.Path, provision: bool, watched: bool = False) -> Guest:
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
    if watched:
        guest.crash_log = CrashLog()
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
        """Runs one section. On a guest of the release under test the section's crash evidence is read at its
        end (also when it failed, for a crash may be why) and a crash, an ANR or a lost marker fails it."""
        watch = guest.crash_log if guest is not None else None
        problem: Exception | None = None
        note = None
        try:
            if watch is not None:
                watch.begin(guest, name)
            note = body()
        except (Failure, boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as failed:
            problem = failed
        if watch is not None:
            try:
                crashed = watch.end(guest)
            except (Failure, subprocess.TimeoutExpired, OSError) as unread:
                crashed = [f"the crash evidence could not be read: {unread}"]
            if crashed:
                text = "; ".join(crashed[:5])
                problem = Failure(f"{problem}; and {text}" if problem is not None else text)
        if problem is None:
            print(f"ok   {name}" + (f" ({note})" if note else ""))
            return
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
        raise Failure("the platform counted no failed internet check, so the injected failures did not "
                      "reach it and the network was never at risk")
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


CRASH_SECTION = "NetworkStack, SystemUI and Settings neither crash nor stop answering"
# The processes of the warning, each by the exact name it runs under: the platform's network stack (the
# manifest's own process name, not its package), the status bar and Settings. A name that only starts with one
# of these, another package or a private subprocess such as com.android.systemui:screenshot, is another
# process, and none of those subprocesses is read as one of these three.
WARNING_PROCESSES = ("com.android.networkstack.process", "com.android.systemui", "com.android.settings")
# `logcat -v threadtime`: date, time, pid, tid, priority, tag, message. The log's own columns bind the lines of
# one record, which other processes' lines may interleave.
LOG_LINE = re.compile(r"^(\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\s+(\d+)\s+(\d+)\s+([VDIWEFS])\s+(\S+)\s*:\s?(.*)$")
JAVA_PROCESS_LINE = re.compile(r"Process: (\S+), PID: (\d+)")
ANR_LINE = re.compile(r"ANR in (\S+)(?: \(\S*\))?")
NATIVE_PROCESS_LINE = re.compile(r"pid: \d+, (?:ppid: \d+, )?tid: \d+, name: .*  >>> (\S+) <<<")
NATIVE_SIGNAL_LINE = re.compile(r"signal (\d+) \((\S+)\)")
SIGILL_LINE = re.compile(r"signal 4 \(SIGILL\)")
SECTION_MARKER_TAG = "wifi-runner"


@dataclass(frozen=True)
class LogLine:
    when: str
    pid: int
    tid: int
    tag: str
    message: str
    text: str

    @property
    def stream(self) -> tuple[int, int, str]:
        """What binds the lines of one record: the process and thread that logged them, under one tag."""
        return self.pid, self.tid, self.tag


def log_lines(log: str) -> list[LogLine]:
    lines = []
    for text in log.splitlines():
        match = LOG_LINE.match(text)
        if match:
            lines.append(LogLine(match[1], int(match[2]), int(match[3]), match[5], match[6], text.strip()))
    return lines


def next_of_record(lines: list[LogLine], index: int, same_time: bool) -> LogLine | None:
    """The next line of the record that starts at `lines[index]`: the next one of the same process, thread and
    tag, however many lines of other processes come between. One logged message that has several lines (a
    crash's header and its process line) carries one time stamp."""
    first = lines[index]
    for line in lines[index + 1:]:
        if line.stream == first.stream and (line.when == first.when or not same_time):
            return line
    return None


def crash_problems(log: str) -> list[str]:
    """What the log says of the processes of the warning crashing or not answering, and of any illegal
    instruction (an instruction the guest's CPU lacks is a defect of the host, in whatever process). A Java
    crash, a native crash (any fatal signal) and an ANR are each read as the one process they name, by its
    exact name; a crash that names no process is a problem too, since it may be any of them."""
    lines = log_lines(log)
    problems = []
    for index, line in enumerate(lines):
        if SIGILL_LINE.search(line.message):
            problems.append(f"an illegal instruction: {line.text[:160]}")
        if line.message.startswith(("FATAL EXCEPTION", "*** FATAL EXCEPTION")):
            follow = next_of_record(lines, index, same_time=True)
            named = JAVA_PROCESS_LINE.fullmatch(follow.message.strip()) if follow else None
            if named is None:
                problems.append(f"a process crashed without naming itself: {line.text[:160]}")
            elif named[1] in WARNING_PROCESSES:
                problems.append(f"{named[1]} crashed: {line.text[:160]}")
        anr = ANR_LINE.fullmatch(line.message.strip())
        if anr and anr[1] in WARNING_PROCESSES:
            problems.append(f"{anr[1]} did not respond: {line.text[:160]}")
        named = NATIVE_PROCESS_LINE.fullmatch(line.message.strip())
        if named and named[1] in WARNING_PROCESSES:
            for after in lines[index + 1:]:
                if after.stream != line.stream:
                    continue
                signal = NATIVE_SIGNAL_LINE.match(after.message.strip())
                if signal:
                    # Signal 4 is reported above in any process.
                    if signal[1] != "4":
                        problems.append(f"{named[1]} died of signal {signal[1]} ({signal[2]}): {line.text[:160]}")
                    break
                if NATIVE_PROCESS_LINE.fullmatch(after.message.strip()):
                    break
    return problems


class CrashLog:
    """The crash and ANR evidence of one guest, read at the end of every section from a marker that the section
    wrote at its start: the log is small and rotates, so one read at the end of a run cannot rule out what
    the log has dropped. A section whose marker is gone fails, for the evidence it would show is gone. A guest
    restart clears the log, so it reads what the section has logged so far first and writes a new marker."""

    def __init__(self) -> None:
        self.token: str | None = None
        self.name = ""
        # What every section found, kept for the summary at the end; and what this section has found.
        self.problems: list[str] = []
        self.section_problems: list[str] = []
        self.sections = 0

    def begin(self, guest: "Guest", name: str) -> None:
        self.name = name
        self.section_problems = []
        self.mark(guest)

    def mark(self, guest: "Guest") -> None:
        token = uuid.uuid4().hex
        guest.shell(f"log -t {SECTION_MARKER_TAG} section-start-{token}")
        self.token = token

    def collect(self, guest: "Guest") -> None:
        """Reads the log from the marker on: what it holds of the section so far."""
        if self.token is None:
            return
        token, self.token = self.token, None
        lines = guest.shell("logcat -d -b all -v threadtime", timeout=180).splitlines()
        start = next((index for index, line in enumerate(lines) if token in line), None)
        if start is None:
            found = ["the log no longer holds the start of the section (it rotated): a crash or an ANR in it "
                     "cannot be ruled out"]
        else:
            found = crash_problems("\n".join(lines[start + 1:]))
        self.sections += 1
        self.section_problems += found
        self.problems += [f"{self.name}: {problem}" for problem in found]

    def end(self, guest: "Guest") -> list[str]:
        self.collect(guest)
        return self.section_problems

    @property
    def active(self) -> bool:
        return self.token is not None


def check_no_crashes(guest: Guest) -> str | None:
    """Across the sections of this guest, NetworkStack, SystemUI and Settings neither crashed nor stopped
    answering, and nothing hit an illegal instruction: what every section read of the log from its own marker."""
    watch = guest.crash_log
    if watch is None or watch.sections == 0:
        raise Failure("no section of this guest read its crash evidence")
    if watch.problems:
        raise Failure("; ".join(watch.problems[:5]))
    return f"{watch.sections} reads of the log from a section's own marker, no crash, ANR or illegal instruction"


@dataclass
class Pin:
    """The network a section judges, and when the platform last gave it the validated capability: the same
    network, never given it again, is what "validated throughout" means."""
    net_id: int
    validated_at: int


def pin_problems(dump: str, pin: Pin) -> list[str]:
    """How the network differs from the pinned one: replaced by a newer network, gone, or validated again
    (`lastValidated` moved), as it would be after the capability dropped and came back."""
    networks = wifi_networks(dump)
    if pin.net_id not in networks:
        return [f"network {pin.net_id} is gone"]
    problems = []
    newest = max(networks)
    if newest != pin.net_id:
        problems.append(f"network {pin.net_id} was replaced by network {newest}")
    now = last_validated(dump).get(pin.net_id)
    if now is None:
        problems.append(f"network {pin.net_id} is no longer validated")
    elif now != pin.validated_at:
        problems.append(f"network {pin.net_id} was validated again (lastValidated {pin.validated_at} -> {now})")
    return problems


def pin_network(guest: "Guest", seconds: float = WARNING_SECONDS, newer_than: int | None = None) -> Pin:
    """Waits for the newest Wi-Fi network (newer than `newer_than`, when given) to be validated, and pins it."""
    found: list[Pin] = []

    def validated() -> str | None:
        dump = guest.connectivity()
        net_id = newest_wifi_network(dump)
        if net_id is None:
            return "dumpsys connectivity lists no Wi-Fi network"
        if newer_than is not None and net_id <= newer_than:
            return f"the newest network is still {net_id}, not a new one"
        if "VALIDATED" not in wifi_networks(dump)[net_id]:
            return f"network {net_id} is not validated yet"
        when = last_validated(dump).get(net_id)
        if when is None:
            return f"network {net_id} shows no lastValidated"
        found.append(Pin(net_id, when))
        return None

    wait_for("the joined network is validated", seconds, validated, guest, interval=1.0)
    return found[-1]


def wait_problems(description: str, seconds: float, guest: Guest, problems: Callable[[], list[str]],
                  interval: float = 5.0) -> None:
    """Waits until `problems` returns none; the last ones are the failure."""
    wait_for(description, seconds, lambda: "; ".join(problems()) or None, guest, interval=interval)


def sign_in_problems(guest: Guest) -> list[str]:
    """Whether anything asks to sign in: a notification of ConnectivityService (Sign in to network, and the
    no Internet, partial connectivity and lost Internet notices that tap through to a dialog whose answer
    tears the network down), or the captive portal login activity."""
    problems = [f"a notification of ConnectivityService: {line}"
                for line in connectivity_notifications(guest.notifications())]
    problems += [f"the captive portal login activity: {line}" for line in captive_portal_activities(guest.activities())]
    return problems


def warning_problems(guest: Guest, probes_failed: bool, pin: Pin | None = None) -> list[str]:
    """What differs from a validated network that shows only the warning of a failed check: the capabilities
    (validated, not partial, not behind a sign-in page), the validation log, the notifications and the
    activities (no sign-in), and the screen (Settings' "Limited connection", the status bar's "No internet").
    With `pin` it is the pinned network, still the one."""
    dump = guest.connectivity()
    net_id = pin.net_id if pin is not None else newest_wifi_network(dump)
    if net_id is None:
        return validation_problems(None)
    problems = pin_problems(dump, pin) if pin is not None else []
    problems += validation_problems(wifi_networks(dump).get(net_id))
    logs = validation_logs(guest.network_stack()).get(net_id, [])
    problems += check_log_problems(logs, CHECK_FAILED_LINE, probes_failed)
    problems += sign_in_problems(guest)
    problems += mark_problems(ui_nodes(guest.ui()), shown=True)
    return problems


def cleared_problems(guest: Guest, pin: Pin | None = None, after_failure: bool = False) -> list[str]:
    """What differs from a validated network whose check passed: the same, with the check's passing line and
    without the mark (and the status bar's icon in the dump). With `after_failure` the passing line is a
    new one, after the last failed check of the same network."""
    dump = guest.connectivity()
    net_id = pin.net_id if pin is not None else newest_wifi_network(dump)
    if net_id is None:
        return validation_problems(None)
    problems = pin_problems(dump, pin) if pin is not None else []
    problems += validation_problems(wifi_networks(dump).get(net_id))
    logs = validation_logs(guest.network_stack()).get(net_id, [])
    problems += passed_after_failed_problems(logs) if after_failure else check_log_problems(logs, CHECK_PASSED_LINE)
    problems += sign_in_problems(guest)
    problems += mark_problems(ui_nodes(guest.ui()), shown=False)
    return problems


def stays_validated(guest: Guest, pin: Pin | None = None) -> None:
    """Raises unless the network is enabled and joined with Auto-connect on, validated (not partial, not
    behind a sign-in page, its private DNS not broken), and nothing asks to sign in. With `pin` it is also
    the one network, never validated again."""
    dump = guest.dump()
    status = selection_status(dump)
    if status != "NETWORK_SELECTION_ENABLED":
        raise Failure(f"the network became {status} ({selection_disable_reason(dump)}) while the check failed")
    values = autojoin_values(dump)
    if not values or not all(values):
        raise Failure("Auto-connect is off")
    if not guest.joined():
        raise Failure("the station left the network")
    connectivity = guest.connectivity()
    net_id = pin.net_id if pin is not None else newest_wifi_network(connectivity)
    problems = pin_problems(connectivity, pin) if pin is not None else []
    problems += validation_problems(None if net_id is None else wifi_networks(connectivity).get(net_id))
    problems += sign_in_problems(guest)
    if problems:
        raise Failure("; ".join(problems))


def hold_validated(guest: Guest, seconds: float, pin: Pin | None = None) -> None:
    """For `seconds`, the network stays as `stays_validated` says."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        guest.alive()
        stays_validated(guest, pin)
        time.sleep(POLL_SECONDS)


def wait_watching(description: str, seconds: float, guest: Guest, pin: Pin, check: Callable[[], str | None],
                  interval: float = 3.0) -> None:
    """Waits for `check` while the network stays the one validated network: a change of it fails at once."""
    def step() -> str | None:
        stays_validated(guest, pin)
        return check()

    wait_for(description, seconds, step, guest, interval=interval)


def warning_shown(guest: Guest, probes_failed: bool, seconds: float, what: str, pin: Pin | None = None) -> None:
    """The warning shows within WARNING_SECONDS, and is held for `seconds`, with the Internet page in front."""
    guest.show_internet_page()
    try:
        wait_problems(what, WARNING_SECONDS, guest, lambda: warning_problems(guest, probes_failed, pin))
        hold_validated(guest, seconds, pin)
        problems = warning_problems(guest, probes_failed, pin)
        if problems:
            raise Failure("after the hold: " + "; ".join(problems))
    finally:
        guest.leave_internet_page()


def check_failed_check_warns(guest: Guest) -> str | None:
    """The validation probes at a black hole fail the check, and nothing else: the network stays validated
    (as on a host that reaches no validation server) while Android's Wi-Fi mark shows the failed check."""
    guest.rejoin_with_settings(CHECK_SETTINGS, clear=(*PRIVATE_DNS_SETTINGS, USE_HTTPS_SETTING))
    pin = pin_network(guest)
    warning_shown(guest, True, WARNING_HOLD_SECONDS, "the failed check shows only the warning", pin)
    return f"validated, joined and enabled for {WARNING_HOLD_SECONDS} s with the mark shown"


def check_warning_after_wake(guest: Guest) -> str | None:
    """Every wake drops the station and joins it again, as a new network with a new check: its ID is higher
    than the one before the suspend, so an old network's log and mark cannot stand for it."""
    before = newest_wifi_network(guest.connectivity())
    if before is None:
        raise Failure("dumpsys connectivity lists no Wi-Fi network before the suspend")
    guest.suspend_and_wake()
    guest.wait_joined(RESUME_SECONDS, "the station joins again after the wake")
    pin = pin_network(guest, RESUME_SECONDS, newer_than=before)
    warning_shown(guest, True, SHORT_HOLD_SECONDS, "the failed check shows only the warning after the wake", pin)
    return f"network {before} became network {pin.net_id}"


def page_settings(port: int, scenario: str = "") -> dict[str, str]:
    """The probes' HTTP and fallback URLs at the host's server (through `adb reverse` on the guest's loopback),
    under the scenario's path, and the HTTPS URL at the black hole, so a page answer is all the check gets."""
    return {"captive_portal_http_url": f"http://127.0.0.1:{port}{scenario}/generate_204",
            "captive_portal_fallback_url": f"http://127.0.0.1:{port}{scenario}/gen_204",
            "captive_portal_https_url": f"https://{BLACKHOLE}/generate_204"}


PAGE_CODES = {"redirect": 302, "page": 200}


def server_answer_problems(server: "ProbeServer", scenario: str, code: int) -> list[str]:
    """What the server gave the HTTP probes of the scenario: some request, each answered with `code`."""
    statuses = [status for _, path, status in server.requests(scenario) if path.endswith("/generate_204")]
    if not statuses:
        return [f"the server saw no HTTP probe of {scenario}"]
    wrong = sorted({status for status in statuses if status != code})
    return [f"the server answered the probes of {scenario} with {wrong}, not {code}"] if wrong else []


def check_page_answer_warns(guest: Guest, server: "ProbeServer", mode: str) -> str | None:
    """A sign-in page or a block page answers the probe in place of the expected answer: the current network's
    own probe received it (its log names the scenario's URL and the code, and the server gave that code), the
    platform stays validated, no captive portal state, notification or login activity follows, and only the
    warning shows."""
    code = PAGE_CODES[mode]
    scenario = server.scenario()
    server.mode = mode
    server.clear()
    guest.reverse(server.port)
    guest.rejoin_with_settings(page_settings(server.port, scenario), clear=(*PRIVATE_DNS_SETTINGS, USE_HTTPS_SETTING))
    pin = pin_network(guest)
    wait_for("a probe reaches the host's server", WARNING_SECONDS,
             lambda: None if server.probes(scenario) else "the server has seen no request of this scenario", guest)
    wait_problems(f"the current network's probe received the {mode} answer", WARNING_SECONDS, guest,
                  lambda: probe_answer_problems(guest.validation_log(pin.net_id), scenario, code)
                  + server_answer_problems(server, scenario, code), interval=2.0)
    warning_shown(guest, False, WARNING_HOLD_SECONDS, f"the {mode} answer shows only the warning", pin)
    return f"{len(server.probes(scenario))} requests answered {code} and read by the network's own probe"


def check_recovery_on_one_network(guest: Guest, server: "ProbeServer") -> str | None:
    """One network, never replaced and never validated again, from a failing check to a passing one: the
    probes ask a server that answers with a page. With the Internet page in front and no other action, the
    network is validated before its first check, the warning shows in Settings and in the status bar, the
    check comes back after the stock back-off, and when the server answers 204 the next check passes and the
    warning goes from both, with a new passing line in the log. A forced re-evaluation then checks the
    network again, which brings the warning back, and the next passing check takes it away."""
    scenario = server.scenario()
    server.mode = "page"
    server.clear()
    guest.reverse(server.port)
    guest.show_internet_page()
    try:
        guest.rejoin_with_settings({**page_settings(server.port, scenario), USE_HTTPS_SETTING: "0"},
                                   clear=PRIVATE_DNS_SETTINGS)
        pin = pin_network(guest)
        wait_for("the first check reaches the host's server", WARNING_SECONDS,
                 lambda: None if server.checks(scenario) else "the server has seen no check", guest, interval=1.0)
        first = first_validation_problems(guest.validation_log(pin.net_id))
        if first:
            raise Failure("; ".join(first))
        wait_watching("the failed check shows in Settings and in the status bar, with no other action",
                      WARNING_SECONDS, guest, pin,
                      lambda: "; ".join(warning_problems(guest, False, pin)
                                        + probe_answer_problems(guest.validation_log(pin.net_id), scenario, 200)) or None)

        def retried() -> str | None:
            times = server.checks(scenario)
            if len(times) < RETRIES_NEEDED:
                return f"{len(times)} of {RETRIES_NEEDED} checks so far"
            problems = retry_problems(times)
            if problems:
                raise Failure("; ".join(problems))
            return None

        wait_watching("the check comes back after the back-off", WARNING_SECONDS, guest, pin, retried)
        wrong = server_answer_problems(server, scenario, 200)
        if wrong:
            raise Failure("; ".join(wrong))
        # The server now gives the expected answer: nothing else changes, and no rejoin.
        server.mode = "expected"
        wait_watching("the warning goes from Settings and the status bar when a check passes", WARNING_SECONDS,
                      guest, pin, lambda: "; ".join(cleared_problems(guest, pin, after_failure=True)) or None)
        # A forced re-evaluation checks the network again; no timer is left after a pass, so a new check at the
        # server is the forced one.
        server.mode = "page"
        seen = len(server.checks(scenario))
        guest.reevaluate(pin.net_id)
        wait_watching("a forced re-evaluation checks the network again", FORCE_SECONDS, guest, pin,
                      lambda: None if len(server.checks(scenario)) > seen else "the server saw no new check")
        wait_watching("the warning returns after the forced check", WARNING_SECONDS, guest, pin,
                      lambda: "; ".join(warning_problems(guest, False, pin)) or None)
        server.mode = "expected"
        wait_watching("the warning goes again when the next check passes", WARNING_SECONDS, guest, pin,
                      lambda: "; ".join(cleared_problems(guest, pin, after_failure=True)) or None)
        hold_validated(guest, SHORT_HOLD_SECONDS, pin)
    finally:
        guest.leave_internet_page()
    return f"network {pin.net_id} validated at {pin.validated_at} throughout, {len(server.checks(scenario))} checks"


def private_dns_probe_problems(first: int, last: int) -> list[str]:
    """The strict private DNS name is probed when the network is validated and not again for each check: at
    least one probe by the first failed check, and no more by the last."""
    if first < 1:
        return ["the log shows no private DNS probe: the strict name was never evaluated"]
    if last > first:
        return [f"private DNS was probed {last - first} more times while the checks failed ({first} -> {last}): "
                "each check evaluates it again"]
    return []


def check_strict_private_dns_holds(guest: Guest, server: "ProbeServer") -> str | None:
    """With a working strict private DNS name and a check that fails again and again, the network stays
    validated with its private DNS not broken, and the private DNS probe is not repeated for each check
    (the validation log, collected over the polls, holds the same probes before and after the checks)."""
    scenario = server.scenario()
    server.mode = "page"
    server.clear()
    guest.reverse(server.port)
    settings = {**page_settings(server.port, scenario), USE_HTTPS_SETTING: "0", "private_dns_mode": "hostname",
                "private_dns_specifier": PRIVATE_DNS_HOST}
    guest.rejoin_with_settings(settings, clear=())
    try:
        pin = pin_network(guest)
    except Failure as problem:
        raise Failure(f"{problem}; the host must reach the private DNS server {PRIVATE_DNS_HOST}") from problem
    union = LogUnion()
    first: list[int] = []

    def failed_enough() -> str | None:
        union.add(guest.validation_entries(pin.net_id))
        failed = union.count(CHECK_FAILED_LINE)
        if failed and not first:
            first.append(union.count(PRIVATE_DNS_PROBE))
        return None if failed >= RETRIES_NEEDED + 1 else f"{failed} failed checks so far"

    wait_watching("the checks fail one after the other while the network stays validated", WARNING_SECONDS,
                  guest, pin, failed_enough, interval=2.0)
    last = union.count(PRIVATE_DNS_PROBE)
    problems = private_dns_probe_problems(first[0], last)
    if problems:
        raise Failure("; ".join(problems))
    hold_validated(guest, SHORT_HOLD_SECONDS, pin)
    return f"{union.count(CHECK_FAILED_LINE)} failed checks, {last} private DNS probes, validated throughout"


def check_passing_check_clears(guest: Guest, server: "ProbeServer") -> str | None:
    """The expected answer (the host's server with the HTTPS probe off) passes the check of a network that has
    just joined: the current network's probe received the 204, the warning is not there and the network is
    validated."""
    scenario = server.scenario()
    server.mode = "expected"
    server.clear()
    guest.reverse(server.port)
    guest.rejoin_with_settings({**page_settings(server.port, scenario), USE_HTTPS_SETTING: "0"},
                               clear=PRIVATE_DNS_SETTINGS)
    pin = pin_network(guest)
    guest.show_internet_page()
    try:
        wait_problems("the warning is not there when the check passes", WARNING_SECONDS, guest,
                      lambda: cleared_problems(guest, pin)
                      + probe_answer_problems(guest.validation_log(pin.net_id), scenario, 204))
        hold_validated(guest, SHORT_HOLD_SECONDS, pin)
    finally:
        guest.leave_internet_page()
    return None


def check_stock_servers_clear(guest: Guest) -> str | None:
    """With every setting deleted the probes go to the stock servers through the host's network: the check
    passes and the warning is gone. A host that reaches no stock server cannot show this."""
    guest.rejoin_with_settings({}, clear=ALL_CHECK_SETTINGS)
    pin = pin_network(guest)
    guest.show_internet_page()
    try:
        wait_problems("the stock servers leave no warning (the host must reach them)", WARNING_SECONDS, guest,
                      lambda: cleared_problems(guest, pin))
    finally:
        guest.leave_internet_page()
    return None


def warning_sections(guest: Guest, server: "ProbeServer", report: "Report") -> None:
    """A failed check, and a page in place of the answer, show only the warning; the warning follows one
    network from a failing check to a passing one and back; a passing check shows none; strict private DNS
    is not probed again for each check. The settings are deleted after, so the sections that follow start
    from the stock ones."""
    try:
        report.section("a failed internet check shows only the warning, and the network stays validated", guest,
                       lambda: check_failed_check_warns(guest))
        report.section("the warning shows again after a suspend and wake", guest,
                       lambda: check_warning_after_wake(guest))
        report.section("a sign-in page in place of the expected answer shows only the warning", guest,
                       lambda: check_page_answer_warns(guest, server, "redirect"))
        report.section("a block page in place of the expected answer shows only the warning", guest,
                       lambda: check_page_answer_warns(guest, server, "page"))
        report.section("the warning follows one network from a failing check to a passing one and back", guest,
                       lambda: check_recovery_on_one_network(guest, server))
        report.section("a passing check leaves no warning", guest,
                       lambda: check_passing_check_clears(guest, server))
        report.section("the stock servers leave no warning", guest,
                       lambda: check_stock_servers_clear(guest))
        report.section("a failing check does not probe a strict private DNS name again", guest,
                       lambda: check_strict_private_dns_holds(guest, server))
    finally:
        try:
            guest.delete_settings(ALL_CHECK_SETTINGS)
            guest.unreverse(server.port)
        except (Failure, subprocess.TimeoutExpired, OSError) as problem:
            print(f"     the probe settings were not deleted: {problem}", file=sys.stderr)


def fresh(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report, waits: Waits) -> None:
    print("fresh data disk")
    guest = start_guest(options, adb, private, options.release, private / "fresh", provision=True, watched=True)
    server: ProbeServer | None = None
    try:
        server = ProbeServer()
        report.section("overlay is applied", guest, lambda: check_overlay(guest))
        report.section("VirtWifi is saved and joined at the first boot", guest, lambda: check_saved_and_joined(guest))
        report.section("the boot script runs without SELinux denials", guest, lambda: check_no_denials(guest))
        warning_sections(guest, server, report)
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
        report.section(CRASH_SECTION, guest, lambda: check_no_crashes(guest))
    finally:
        if server is not None:
            server.close()
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


def previous_never_disables(guest: Guest) -> bool:
    """Whether the previous release's platform keeps the network enabled for lack of internet: its overlay
    holds the largest thresholds. A platform without the overlay has the stock ones and disables it."""
    try:
        check_overlay(guest)
    except Failure:
        return False
    return True


def prepare_kept(guest: Guest, waits: Waits) -> str | None:
    """On the disk of a release that never disables the network: the station has joined it (the marker is 2),
    the user marked it metered (saving it again would reset that), and the probes fail the check; a restart
    of this guest shows what the disk holds."""
    wait_for("VirtWifi is saved", 60, lambda: None if network_id(guest.listing()) is not None else "not saved", guest)
    guest.wait_joined(JOIN_SECONDS, "the previous release joins VirtWifi")
    wait_for("the boot script marks the network joined", waits.repair,
             lambda: None if guest.marker() == "2" else f"the marker is {guest.marker()!r}", guest,
             interval=POLL_SECONDS)
    guest.shell(f"cmd wifi add-network {SSID} open -m")
    guest.wait_joined(JOIN_SECONDS, "the metered network is joined")
    if metered_override(guest.dump()) != 1:
        raise Failure("the network is not marked metered")
    guest.delete_settings(PRIVATE_DNS_SETTINGS)
    guest.put_settings(CHECK_SETTINGS)
    guest.restart()
    guest.wait_joined(JOIN_SECONDS, "the station joins after the restart")
    if guest.marker() != "2" or metered_override(guest.dump()) != 1:
        raise Failure(f"the disk holds marker {guest.marker()!r} and metered {metered_override(guest.dump())}, "
                      "not marker '2' and metered 1")
    if selection_status(guest.dump()) != "NETWORK_SELECTION_ENABLED":
        raise Failure("the previous release's network is not enabled")
    return None


def check_left_alone(guest: Guest) -> str | None:
    """The first boot of the release under test finds the station joined once (marker 2), so the script saves
    nothing again: the marker, the metered setting and the join are the disk's."""
    guest.wait_joined(JOIN_SECONDS, "the station joins after the first boot of the release")
    time.sleep(MARGIN_SECONDS)
    if guest.marker() != "2":
        raise Failure(f"the marker is {guest.marker()!r}, not '2'")
    if metered_override(guest.dump()) != 1:
        raise Failure("the boot script saved a joined network again and reset its metered setting")
    status = selection_status(guest.dump())
    if status != "NETWORK_SELECTION_ENABLED":
        raise Failure(f"the network is {status}")
    check_no_repair_logged(guest, "although the station had joined")
    return None


def check_warning_on_disk(guest: Guest) -> str | None:
    """The probe settings the disk holds fail the check of the network the station joined at this boot."""
    lost = [key for key, value in CHECK_SETTINGS.items() if guest.read_setting(key) != value]
    if lost:
        raise Failure(f"the data disk lost the probe settings {lost}")
    pin = pin_network(guest)
    warning_shown(guest, True, WARNING_HOLD_SECONDS, "the failed check shows only the warning on the upgraded disk",
                  pin)
    return None


def upgrade(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report, waits: Waits) -> None:
    current = boot_android.release_path(options.release)
    for index, release in enumerate(options.previous_release):
        previous = boot_android.release_path(release)
        if previous == current:
            print("upgrade: the previous release is the release under test; nothing to upgrade from")
            continue
        upgrade_from(options, adb, private, report, waits, previous, current, private / f"upgrade-{index}")


def upgrade_from(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report, waits: Waits,
                 previous: pathlib.Path, current: pathlib.Path, instance: pathlib.Path) -> None:
    print(f"upgrade: a data disk of {previous.name} booted by {current.name}")
    guest = start_guest(options, adb, private, previous, instance, provision=True)
    prepared = False
    try:
        kept = previous_never_disables(guest)

        def disable() -> str | None:
            nonlocal prepared
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
            prepared = True
            return None

        def keep() -> str | None:
            nonlocal prepared
            note = prepare_kept(guest, waits)
            prepared = True
            return note

        if kept:
            report.section(f"{previous.name} leaves VirtWifi joined and metered on its disk, with the probes failing",
                           guest, keep)
        else:
            report.section(f"{previous.name} leaves VirtWifi disabled on its disk", guest, disable)
    finally:
        stop(guest)
    if not prepared:
        return
    guest = start_guest(options, adb, private, options.release, instance, provision=False, watched=True)
    try:
        if kept:
            report.section("the first boot of the release leaves the joined network alone", guest,
                           lambda: check_left_alone(guest))
            report.section("the boot script runs without SELinux denials", guest, lambda: check_no_denials(guest))
            report.section("a failed internet check shows only the warning on the upgraded disk", guest,
                           lambda: check_warning_on_disk(guest))
        else:
            report.section("the first boot of the release repairs the network", guest,
                           lambda: check_repaired(guest, waits))
            report.section("the boot script runs without SELinux denials", guest, lambda: check_no_denials(guest))
            report.section("the repaired network joins again after a suspend and wake", guest,
                           lambda: check_suspend(guest))
            report.section("a failed internet check shows only the warning after the repair", guest,
                           lambda: check_failed_check_warns(guest))
            report.section("the next boot leaves the repaired network alone", guest,
                           lambda: check_autojoin_kept(guest, waits))
        report.section(CRASH_SECTION, guest, lambda: check_no_crashes(guest))
    finally:
        stop(guest)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", type=pathlib.Path,
                        help="the macos-android17 uml-darwin-android17 (the launcher's default)")
    parser.add_argument("--release", type=pathlib.Path, help="installed Android release under test (default: accepted)")
    parser.add_argument("--previous-release", type=pathlib.Path, action="append",
                        help="installed release whose data disk the upgrade section boots first (the section "
                        "runs only when this is given; give the option once for each release)")
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
        if options.previous_release:
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
