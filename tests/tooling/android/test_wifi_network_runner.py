#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Wi-Fi network runner's reading of the guest, without a guest.

run_android_wifi_network.py reads `cmd wifi status`, `cmd wifi list-networks`,
`dumpsys wifi` and `cmd overlay list`. The texts below have the layout those
commands print (WifiShellCommand, WifiConfiguration's dump and the overlay
manager), so the runner's parsers run here; the runner also takes the boot
script's waits from the tracked script, and reads the overlay's integers with the
verifier's reader of compiled resource tables. The upgrade section's checks run
against a guest that answers from a script, on a clock that a sleep moves on. So do the sections that show
a failed check as a warning (`dumpsys connectivity`, `dumpsys network_stack`, `dumpsys notification`,
`dumpsys activity` and a UI dump), and the host's probe server answers a real client.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import http.client
import importlib.util
import io
from pathlib import Path
import sys
import unittest
from unittest import mock
import zipfile

REPOSITORY = Path(__file__).resolve().parents[3]
SCRIPT = REPOSITORY / "tests/integration/android/run_android_wifi_network.py"
SPEC = importlib.util.spec_from_file_location("run_android_wifi_network", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_builder_lane import synthetic_resource_table  # noqa: E402

JOINED = """\
Wifi is enabled
Wifi scanning is only available when wifi is enabled
==== Primary ClientModeManager instance ====
Wifi is connected to "VirtWifi"
WifiInfo: SSID: "VirtWifi", Supplicant state: COMPLETED
"""
NOT_JOINED = """\
Wifi is enabled
Wifi scanning is only available when wifi is enabled
==== Primary ClientModeManager instance ====
Wifi is not connected
"""
OFF = """\
Wifi is disabled
Wifi scanning is only available when wifi is enabled
"""
LISTING = """\
Network Id      SSID                         Security type
0            VirtWifi                         open
0            VirtWifi                         owe^
"""
DISABLED_DUMP = """\
 NetworkSelectionStatus NETWORK_SELECTION_PERMANENTLY_DISABLED
 mNetworkSelectionDisableReason NETWORK_SELECTION_DISABLED_NO_INTERNET_PERMANENT
 hasEverValidatedInternetAccess: false
NETWORK_SELECTION_DISABLED_NO_INTERNET_PERMANENT counter:1
 meteredOverride 1
"""
ENABLED_DUMP = """\
 NetworkSelectionStatus NETWORK_SELECTION_ENABLED
 hasEverValidatedInternetAccess: true
NETWORK_SELECTION_DISABLED_NO_INTERNET_PERMANENT counter:3
NETWORK_SELECTION_DISABLED_NO_INTERNET_TEMPORARY counter:2
 meteredOverride 0
 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false
"""
AUTOJOIN_OFF_DUMP = ENABLED_DUMP.replace("allowAutojoin=true", "allowAutojoin=false")
DENIAL = ("11-11 09:15:55.822   333   333 W auditd  : type=1400 audit(0.0:7): avc:  denied  "
          "{ read } for comm=\"cmd\" scontext=u:r:kernarium_wifi_network:s0 "
          "tcontext=u:object_r:system_file:s0 tclass=file permissive=0\n")
REPAIR_LINE = "11-11 09:20:00.000 I kernarium_wifi_network: VirtWifi saved again: Wi-Fi is on and nothing joined\n"

def agent(net_id: int, capabilities: str, transport: str = "WIFI") -> str:
    """The line `dumpsys connectivity` prints for a network (NetworkAgentInfo), trimmed to what the runner reads."""
    return (f"  NetworkAgentInfo{{network{{{net_id}}}  handle{{441492361229}}  ni{{{transport} CONNECTED extra: }} "
            "created=2026-11-10T13:30:32.485Z Score(Policies : TRANSPORT_PRIMARY&EVER_EVALUATED ; KeepConnected : 0) "
            "created 149238 lp{{InterfaceName: wlan0 LinkAddresses: [ 10.0.2.16/24 ] DnsAddresses: [ /10.0.2.3 ]}} "
            f"nc{{[ Transports: {transport} Capabilities: {capabilities} LinkUpBandwidth>=12000Kbps "
            'Specifier: <WifiNetworkAgentSpecifier [WifiConfiguration=, SSID="VirtWifi"]>]}  everValidated{true}\n')


STOCK = "NOT_METERED&INTERNET&NOT_RESTRICTED&TRUSTED&NOT_VPN&NOT_ROAMING&FOREGROUND&NOT_CONGESTED&NOT_SUSPENDED"
VALIDATED = STOCK.replace("NOT_VPN&", "NOT_VPN&VALIDATED&")
CONNECTIVITY = "Current networks:\n" + agent(101, STOCK) + agent(102, VALIDATED) + "Other state\n"
NETWORK_STACK = """\
NetworkStack version:
Recently active IpClient logs:
IpClient.wlan0

Validation logs (most recent first):
102 - "VirtWifi"
  2026-11-10T13:30:32.803492 - Wi-Fi is validated before its internet check
  2026-11-10T13:30:44.965598 - PROBE_HTTPS https://192.0.2.1/generate_204 Probe failed with exception java.net.SocketTimeoutException: failed to connect after 10000ms
  2026-11-10T13:30:44.971100 - PROBE_HTTP http://192.0.2.1/generate_204 Probe failed with exception java.net.SocketTimeoutException: failed to connect after 10000ms
  2026-11-10T13:30:44.975000 - Wi-Fi internet check failed, the network stays validated
101 - "VirtWifi"
  2026-11-10T13:29:29.956111 - PROBE_DNS 192.0.2.1 16ms FAIL in type ADDRCONFIG java.net.UnknownHostException
"""
PASSED_NETWORK_STACK = NETWORK_STACK.replace(
    "Wi-Fi internet check failed, the network stays validated", "Wi-Fi internet check passed")
QUIET_NOTIFICATIONS = """\
Current Notification Manager state:
  Notification List:
    NotificationRecord(0x0a1b2c3d: pkg=com.android.systemui user=UserHandle{0} id=7 tag=null importance=1 key=0|com.android.systemui|7|null|10089)
"""
SIGN_IN_NOTIFICATIONS = QUIET_NOTIFICATIONS + (
    "    NotificationRecord(0x0f3a2b1c: pkg=android user=UserHandle{0} id=41 tag=ConnectivityNotification:102 "
    "importance=2 key=0|android|41|ConnectivityNotification:102|1000)\n")
QUIET_ACTIVITIES = "ACTIVITY MANAGER ACTIVITIES\n  * Task{abc #1 type=home} A=1000:com.android.launcher3\n"
PORTAL_ACTIVITIES = QUIET_ACTIVITIES + (
    "  * Task{def #9 type=standard} A=10200:com.android.captiveportallogin\n"
    "    ActivityRecord{1 u0 com.android.captiveportallogin/.CaptivePortalLoginActivity t9}\n")


def hierarchy(settings_text: str, bar_description: str | None) -> str:
    """A `uiautomator dump --windows`: Settings' Internet page and, when given, the status bar's Wi-Fi icon."""
    bar = ("" if bar_description is None else
           '<node index="2" text="" resource-id="com.android.systemui:id/wifi_signal" '
           'class="android.widget.ImageView" package="com.android.systemui" '
           f'content-desc="{bar_description}" bounds="[600,0][640,24]" />')
    return (
        "<?xml version='1.0' encoding='UTF-8' standalone='yes' ?>\n"
        '<displays><display id="0"><hierarchy rotation="0">'
        '<node index="0" text="" class="android.widget.FrameLayout" package="com.android.settings" '
        'content-desc="" bounds="[0,0][720,1280]">'
        '<node index="1" text="VirtWifi" class="android.widget.TextView" package="com.android.settings" '
        'content-desc="" bounds="[0,100][720,150]" />'
        f'<node index="2" text="{settings_text}" class="android.widget.TextView" package="com.android.settings" '
        'content-desc="" bounds="[0,150][720,200]" /></node>'
        f"{bar}</hierarchy></display></displays>\nUI hierarchy dumped to: /data/local/tmp/x.xml\n")


WARNING_SCREEN = hierarchy("Connected / Limited connection", "Wi-Fi signal full.,No internet")
CLEAR_SCREEN = hierarchy("Connected", "Wi-Fi signal full.")

WAITS = runner.Waits(45, 180)


class FakeClock:
    """The runner's clock: a sleep moves it on at once."""

    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


class ScriptedGuest(runner.Guest):
    """A guest that answers each command from its list, one answer per call; the last answer repeats."""

    COMMANDS = {"status": "cmd wifi status", "dump": "dumpsys wifi",
                "script_log": f"logcat -d -s {runner.SCRIPT_LOG_TAG}", "log": "logcat -d -b all",
                "connectivity": "dumpsys connectivity", "network_stack": "dumpsys network_stack",
                "notifications": "dumpsys notification --noredact", "activities": "dumpsys activity activities",
                "ui": (f"uiautomator dump --windows {runner.UI_DUMP} >/dev/null 2>&1; cat {runner.UI_DUMP} "
                       f"2>/dev/null; rm -f {runner.UI_DUMP}"),
                "marker": f"getprop {runner.MARKER_PROPERTY}", "listing": "cmd wifi list-networks"}

    def __init__(self, **answers: list[str]) -> None:
        self.answers = {self.COMMANDS[name]: list(values) for name, values in answers.items()}
        self.denials = []
        self.denial_reads = 0
        self.restarts = 0

    def shell(self, command: str, timeout: float = runner.COMMAND_SECONDS) -> str:
        answers = self.answers[command]
        return answers.pop(0) if len(answers) > 1 else answers[0]

    def alive(self) -> None:
        pass

    def restart(self) -> None:
        self.restarts += 1

    def show_internet_page(self) -> None:
        pass

    def leave_internet_page(self) -> None:
        pass


class StubbedGuest(ScriptedGuest):
    """A scripted guest whose actions on the network (settings, rejoin, suspend, adb reverse) are only recorded."""

    def __init__(self, **answers: list[str]) -> None:
        super().__init__(**answers)
        self.calls: list[str] = []

    def reevaluate(self) -> None:
        self.calls.append("reevaluate")

    def reverse(self, port: int) -> None:
        self.calls.append(f"reverse {port}")

    def unreverse(self, port: int) -> None:
        self.calls.append(f"unreverse {port}")

    def put_settings(self, settings: dict[str, str]) -> None:
        self.calls.append(f"put {sorted(settings)}")

    def delete_settings(self, keys: tuple[str, ...] | list[str]) -> None:
        self.calls.append(f"delete {sorted(keys)}")

    def forget_and_save(self) -> None:
        self.calls.append("forget and save")

    def wait_joined(self, seconds: float, what: str) -> None:
        self.calls.append(f"joined {what}")

    def suspend_and_wake(self) -> None:
        self.calls.append("suspend and wake")

    def diagnose(self) -> str:
        return ""


class ReadingTests(unittest.TestCase):
    def test_the_station_is_joined_only_when_it_says_so(self) -> None:
        self.assertTrue(runner.wifi_joined(JOINED))
        self.assertFalse(runner.wifi_joined(NOT_JOINED))
        self.assertFalse(runner.wifi_joined(OFF))
        self.assertFalse(runner.wifi_joined('Wifi is connected to "OtherNet"\n'))

    def test_wifi_is_on_only_when_it_says_so(self) -> None:
        self.assertTrue(runner.wifi_enabled(JOINED))
        self.assertTrue(runner.wifi_enabled(NOT_JOINED))
        self.assertFalse(runner.wifi_enabled(OFF))

    def test_a_network_listed_twice_counts_once(self) -> None:
        self.assertEqual(runner.saved_ssids(LISTING), ["VirtWifi"])
        self.assertEqual(runner.network_id(LISTING), 0)
        self.assertEqual(runner.saved_ssids("No networks\n"), [])
        self.assertIsNone(runner.network_id("No networks\n"))

    def test_the_selection_status_and_its_reason(self) -> None:
        self.assertEqual(runner.selection_status(DISABLED_DUMP), "NETWORK_SELECTION_PERMANENTLY_DISABLED")
        self.assertEqual(runner.selection_disable_reason(DISABLED_DUMP),
                         "NETWORK_SELECTION_DISABLED_NO_INTERNET_PERMANENT")
        self.assertEqual(runner.selection_status(ENABLED_DUMP), "NETWORK_SELECTION_ENABLED")
        self.assertIsNone(runner.selection_disable_reason(ENABLED_DUMP))
        self.assertIsNone(runner.selection_status("nothing\n"))

    def test_failed_checks_are_counted_over_both_reasons(self) -> None:
        self.assertEqual(runner.no_internet_count(DISABLED_DUMP), 1)
        self.assertEqual(runner.no_internet_count(ENABLED_DUMP), 5)
        self.assertEqual(runner.no_internet_count("nothing\n"), 0)

    def test_a_network_the_dump_prints_three_times_is_counted_once(self) -> None:
        self.assertEqual(runner.no_internet_count(ENABLED_DUMP * 3), 5)

    def test_auto_connect_is_read_from_every_copy_of_the_network(self) -> None:
        self.assertEqual(runner.autojoin_values(ENABLED_DUMP), [True])
        self.assertEqual(runner.autojoin_values(AUTOJOIN_OFF_DUMP * 2), [False, False])
        self.assertEqual(runner.autojoin_values(ENABLED_DUMP + AUTOJOIN_OFF_DUMP), [True, False])
        self.assertEqual(runner.autojoin_values("mAutoJoinEnabled=false\n"), [])

    def test_only_denials_of_the_scripts_domain_count(self) -> None:
        domain = runner.SCRIPT_DOMAIN
        self.assertEqual(len(runner.avc_denials(DENIAL, domain)), 1)
        self.assertEqual(len(runner.avc_denials(DENIAL.replace("scontext=u:r:kernarium_wifi_network:s0",
                                                               "scontext=u:r:init:s0")
                                                .replace("system_file", "kernarium_wifi_network_exec"), domain)), 1)
        self.assertEqual(len(runner.avc_denials(DENIAL.replace("kernarium_wifi_network", "kernarium_net_setup"),
                                                domain)), 0)
        self.assertEqual(runner.avc_denials(DENIAL.replace("denied", "granted"), domain), [])
        self.assertEqual(runner.avc_denials("kernarium_wifi_network: VirtWifi saved again\n", domain), [])

    def test_the_scripts_repair_line_is_found(self) -> None:
        self.assertTrue(runner.repair_logged(
            "11-11 09:20:00.000 I kernarium_wifi_network: VirtWifi saved again: Wi-Fi is on and nothing joined\n"))
        self.assertFalse(runner.repair_logged("--------- beginning of main\n"))

    def test_the_metered_setting(self) -> None:
        self.assertEqual(runner.metered_override(DISABLED_DUMP), 1)
        self.assertEqual(runner.metered_override(ENABLED_DUMP), 0)
        self.assertIsNone(runner.metered_override("nothing\n"))

    def test_the_overlay_is_enabled_only_when_marked(self) -> None:
        listing = ("com.android.wifi.resources\n[x] com.android.wifi.resources.kernarium\n\n"
                   "com.android.intentresolver\n--- com.android.intentresolver.auto_generated_rro_product__\n")
        self.assertTrue(runner.overlay_enabled(listing))
        self.assertFalse(runner.overlay_enabled(listing.replace("[x]", "[ ]")))
        self.assertFalse(runner.overlay_enabled("com.android.wifi.resources\n--- other\n"))


class ScriptAndOverlayTests(unittest.TestCase):
    def test_the_waits_come_from_the_tracked_script(self) -> None:
        script = runner.BOOT_SCRIPT.read_text(encoding="utf-8")
        settle = runner.boot_settle_seconds(script)
        rejoin = runner.boot_rejoin_seconds(script)
        self.assertGreater(settle, 0)
        self.assertLessEqual(settle, 120)
        self.assertGreater(rejoin, settle)
        waits = runner.Waits(settle, rejoin)
        self.assertGreater(waits.quiet, settle)
        self.assertGreater(waits.repair, settle + rejoin)
        for reader in (runner.boot_settle_seconds, runner.boot_rejoin_seconds):
            with self.assertRaises(runner.Failure):
                reader("marker=1\n")

    def test_the_runner_reads_the_integers_of_a_compiled_overlay(self) -> None:
        # A synthetic resource table with the tracked overlay's resource names, read back through the
        # verifier's reader; the tracked overlay's own values are checked by test_builder_lane.
        verifier = runner.load_verifier()
        table = synthetic_resource_table(
            [("config_fixture", [])],
            integers=[(name, verifier.WIFI_NEVER_DISABLED_THRESHOLD) for name in verifier.WIFI_NO_INTERNET_THRESHOLDS])
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w") as apk:
            apk.writestr("resources.arsc", table)
        self.assertEqual(runner.overlay_integers(archive.getvalue()),
                         {name: [2**31 - 1] for name in verifier.WIFI_NO_INTERNET_THRESHOLDS})

    def test_the_injected_failures_name_the_probes_and_a_private_dns_name(self) -> None:
        self.assertEqual(runner.PROBE_SETTINGS["private_dns_mode"], "hostname")
        self.assertTrue(runner.PROBE_SETTINGS["private_dns_specifier"].endswith(".invalid"))
        for key in ("captive_portal_https_url", "captive_portal_http_url", "captive_portal_fallback_url"):
            self.assertIn(runner.BLACKHOLE, runner.PROBE_SETTINGS[key])


class UpgradeCheckTests(unittest.TestCase):
    def setUp(self) -> None:
        clock = mock.patch.object(runner, "time", FakeClock())
        clock.start()
        self.addCleanup(clock.stop)
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)

    def test_a_repair_line_that_rotates_out_before_the_join_is_kept(self) -> None:
        guest = ScriptedGuest(dump=[DISABLED_DUMP, ENABLED_DUMP], script_log=["", "", REPAIR_LINE, ""],
                              status=[NOT_JOINED, NOT_JOINED, NOT_JOINED, JOINED], log=[""])
        self.assertIsNone(runner.check_repaired(guest, WAITS))

    def test_a_join_without_the_repair_line_fails(self) -> None:
        guest = ScriptedGuest(dump=[DISABLED_DUMP, ENABLED_DUMP], script_log=[""], status=[NOT_JOINED, JOINED],
                              log=[""])
        with self.assertRaisesRegex(runner.Failure, "without the boot script saving it again"):
            runner.check_repaired(guest, WAITS)

    def test_an_enabled_disk_without_the_repair_line_fails_at_once(self) -> None:
        guest = ScriptedGuest(dump=[ENABLED_DUMP], script_log=[""], status=[JOINED], log=[""])
        with self.assertRaisesRegex(runner.Failure, "not disabled"):
            runner.check_repaired(guest, WAITS)

    def test_a_denial_read_at_boot_fails_the_later_check(self) -> None:
        guest = ScriptedGuest(dump=[DISABLED_DUMP, ENABLED_DUMP], script_log=[REPAIR_LINE],
                              status=[NOT_JOINED, JOINED], log=[DENIAL, ""])
        runner.check_repaired(guest, WAITS)
        with self.assertRaisesRegex(runner.Failure, "denied the boot script's domain 1 times"):
            runner.check_no_denials(guest)

    def test_a_denial_read_when_the_repair_line_shows_fails_the_later_check(self) -> None:
        guest = ScriptedGuest(dump=[DISABLED_DUMP, ENABLED_DUMP], script_log=["", REPAIR_LINE],
                              status=[NOT_JOINED, NOT_JOINED, JOINED], log=["", DENIAL, ""])
        runner.check_repaired(guest, WAITS)
        with self.assertRaisesRegex(runner.Failure, "denied the boot script's domain 1 times"):
            runner.check_no_denials(guest)

    def test_a_log_without_the_scripts_denials_passes(self) -> None:
        guest = ScriptedGuest(log=[DENIAL.replace("kernarium_wifi_network", "kernarium_net_setup")])
        self.assertIn(f"none for {runner.SCRIPT_DOMAIN}", runner.check_no_denials(guest))

    def test_a_station_that_joins_after_the_previous_release_restarts_fails(self) -> None:
        # The join comes at the last poll of the watch.
        guest = ScriptedGuest(status=[NOT_JOINED] * (runner.BOOT_JOIN_SECONDS // runner.POLL_SECONDS) + [JOINED],
                              dump=[DISABLED_DUMP])
        with self.assertRaisesRegex(runner.Failure, "disk holds the network enabled"):
            runner.check_disabled_on_disk(guest)
        self.assertEqual(guest.restarts, 1)

    def test_a_disk_that_holds_the_network_disabled_passes(self) -> None:
        guest = ScriptedGuest(status=[NOT_JOINED], dump=[DISABLED_DUMP])
        runner.check_disabled_on_disk(guest)
        self.assertEqual(guest.restarts, 1)

    def test_a_network_enabled_after_the_previous_release_restarts_fails(self) -> None:
        guest = ScriptedGuest(status=[NOT_JOINED], dump=[ENABLED_DUMP])
        with self.assertRaisesRegex(runner.Failure, "did not reach the disk"):
            runner.check_disabled_on_disk(guest)


class WarningReadingTests(unittest.TestCase):
    def test_a_wifi_network_is_read_with_its_capabilities(self) -> None:
        networks = runner.wifi_networks(CONNECTIVITY)
        self.assertEqual(sorted(networks), [101, 102])
        self.assertNotIn("VALIDATED", networks[101])
        self.assertIn("VALIDATED", networks[102])
        self.assertEqual(runner.newest_wifi_network(CONNECTIVITY), 102)
        self.assertIsNone(runner.newest_wifi_network("Current networks:\n"))

    def test_a_network_of_another_transport_is_not_a_wifi_network(self) -> None:
        self.assertEqual(runner.wifi_networks(agent(103, VALIDATED, transport="CELLULAR")), {})

    def test_a_network_that_must_stay_validated(self) -> None:
        self.assertEqual(runner.validation_problems(frozenset(VALIDATED.split("&"))), [])
        self.assertEqual(runner.validation_problems(None), ["dumpsys connectivity lists no Wi-Fi network"])
        self.assertEqual(runner.validation_problems(frozenset(STOCK.split("&"))), ["the network is not validated"])
        for name in ("PARTIAL_CONNECTIVITY", "CAPTIVE_PORTAL"):
            problems = runner.validation_problems(frozenset(VALIDATED.split("&") + [name]))
            self.assertEqual(problems, [f"the network has {name}"])

    def test_the_validation_log_is_kept_for_each_network(self) -> None:
        logs = runner.validation_logs(NETWORK_STACK)
        self.assertEqual(sorted(logs), [101, 102])
        self.assertEqual(len(logs[102]), 4)
        self.assertEqual(logs[102][0], "Wi-Fi is validated before its internet check")
        self.assertTrue(logs[101][0].startswith("PROBE_DNS"))
        # What the dump prints before and after the section is not read.
        self.assertEqual(runner.validation_logs("IpClient.wlan0\n  2026-11-10T13:30:32.803492 - x\n"), {})
        after = NETWORK_STACK + "Other section:\n  2026-11-10T13:31:00.000000 - not a validation line\n"
        self.assertEqual(runner.validation_logs(after), logs)

    def test_the_checks_result_is_the_line_the_log_holds(self) -> None:
        messages = runner.validation_logs(NETWORK_STACK)[102]
        self.assertEqual(runner.check_log_problems(messages, runner.CHECK_FAILED_LINE, probes_failed=True), [])
        self.assertEqual(runner.check_log_problems(messages, runner.CHECK_FAILED_LINE), [])
        self.assertEqual(runner.check_log_problems(messages, runner.CHECK_PASSED_LINE),
                         [f"the validation log has no line {runner.CHECK_PASSED_LINE!r}"])
        self.assertEqual(runner.check_log_problems(messages[:1], runner.CHECK_FAILED_LINE, probes_failed=True),
                         [f"the validation log has no line {runner.CHECK_FAILED_LINE!r}",
                          "the validation log shows no failed PROBE_HTTP",
                          "the validation log shows no failed PROBE_HTTPS"])

    def test_a_notification_of_connectivity_service_is_found_by_its_tag(self) -> None:
        self.assertEqual(runner.connectivity_notifications(QUIET_NOTIFICATIONS), [])
        found = runner.connectivity_notifications(SIGN_IN_NOTIFICATIONS)
        self.assertEqual(len(found), 1)
        self.assertIn("ConnectivityNotification:102", found[0])

    def test_the_captive_portal_login_activity_is_found_by_its_package(self) -> None:
        self.assertEqual(runner.captive_portal_activities(QUIET_ACTIVITIES), [])
        self.assertEqual(len(runner.captive_portal_activities(PORTAL_ACTIVITIES)), 2)

    def test_the_marks_of_the_screen(self) -> None:
        warning = runner.ui_nodes(WARNING_SCREEN)
        clear = runner.ui_nodes(CLEAR_SCREEN)
        self.assertTrue(runner.summary_limited(warning))
        self.assertTrue(runner.status_bar_no_internet(warning))
        self.assertFalse(runner.summary_limited(clear))
        self.assertTrue(runner.summary_connected(clear))
        self.assertFalse(runner.status_bar_no_internet(clear))
        self.assertEqual(runner.mark_problems(warning, shown=True), [])
        self.assertEqual(runner.mark_problems(clear, shown=False), [])
        self.assertEqual(len(runner.mark_problems(clear, shown=True)), 2)
        self.assertEqual(len(runner.mark_problems(warning, shown=False)), 3)

    def test_a_no_internet_description_outside_the_status_bar_is_not_the_mark(self) -> None:
        elsewhere = runner.ui_nodes(WARNING_SCREEN.replace('package="com.android.systemui"',
                                                           'package="com.android.settings"'))
        self.assertFalse(runner.status_bar_no_internet(elsewhere))
        self.assertEqual(runner.mark_problems(elsewhere, shown=True),
                         [f"the status bar's Wi-Fi icon does not say {runner.NO_INTERNET!r}"])

    def test_a_dump_that_is_not_xml_fails(self) -> None:
        for text in ("", "uiautomator: not found", "<displays><node"):
            with self.assertRaisesRegex(runner.Failure, "not XML"):
                runner.ui_nodes(text)


class ProbeServerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.server = runner.ProbeServer()
        self.addCleanup(self.server.close)

    def get(self, path: str = "/generate_204") -> tuple[int, dict[str, str], bytes]:
        connection = http.client.HTTPConnection("127.0.0.1", self.server.port, timeout=10)
        try:
            connection.request("GET", path)
            response = connection.getresponse()
            return response.status, {key.lower(): value for key, value in response.getheaders()}, response.read()
        finally:
            connection.close()

    def test_the_expected_answer_is_a_204_without_a_body(self) -> None:
        self.server.mode = "expected"
        status, headers, body = self.get()
        self.assertEqual((status, body), (204, b""))

    def test_a_sign_in_page_is_a_redirect_to_its_own_login(self) -> None:
        self.server.mode = "redirect"
        status, headers, body = self.get("/gen_204")
        self.assertEqual(status, 302)
        self.assertEqual(headers["location"], f"http://127.0.0.1:{self.server.port}/login")
        self.assertEqual(body, b"")

    def test_a_block_page_is_a_200_answer_of_two_kibibytes(self) -> None:
        self.server.mode = "page"
        status, headers, body = self.get()
        self.assertEqual(status, 200)
        self.assertEqual(headers["content-type"], "text/html")
        self.assertEqual(int(headers["content-length"]), runner.PAGE_BYTES)
        self.assertEqual(len(body), runner.PAGE_BYTES)
        self.assertTrue(body.startswith(b"<html>") and body.endswith(b"</html>"))

    def test_the_paths_the_probes_asked_for_are_kept_until_cleared(self) -> None:
        self.get("/generate_204")
        self.get("/gen_204")
        self.assertEqual(self.server.probes(), ["/generate_204", "/gen_204"])
        self.server.clear()
        self.assertEqual(self.server.probes(), [])

    def test_the_server_listens_on_the_loopback_alone(self) -> None:
        self.assertEqual(self.server.httpd.server_address[0], "127.0.0.1")
        self.assertGreater(self.server.port, 1023)


class WarningSectionTests(unittest.TestCase):
    """The sections that show a failed check as a warning, against a guest that answers from a script."""

    def setUp(self) -> None:
        clock = mock.patch.object(runner, "time", FakeClock())
        clock.start()
        self.addCleanup(clock.stop)
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)

    def warned(self, **changes: list[str]) -> ScriptedGuest:
        answers = {"connectivity": [CONNECTIVITY], "network_stack": [NETWORK_STACK],
                   "notifications": [QUIET_NOTIFICATIONS], "activities": [QUIET_ACTIVITIES],
                   "ui": [WARNING_SCREEN], "status": [JOINED], "dump": [ENABLED_DUMP]}
        answers.update(changes)
        return ScriptedGuest(**answers)

    def test_a_failed_check_that_shows_only_the_warning_has_no_problem(self) -> None:
        self.assertEqual(runner.warning_problems(self.warned(), probes_failed=True), [])

    def test_a_network_that_is_not_validated_is_a_problem(self) -> None:
        guest = self.warned(connectivity=[agent(102, STOCK)])
        self.assertEqual(runner.warning_problems(guest, probes_failed=True), ["the network is not validated"])

    def test_a_sign_in_state_is_a_problem(self) -> None:
        portal = agent(102, VALIDATED.replace("VALIDATED", "CAPTIVE_PORTAL&VALIDATED"))
        problems = runner.warning_problems(self.warned(connectivity=[portal]), probes_failed=True)
        self.assertEqual(problems, ["the network has CAPTIVE_PORTAL"])
        problems = runner.warning_problems(self.warned(notifications=[SIGN_IN_NOTIFICATIONS]), probes_failed=True)
        self.assertEqual(len(problems), 1)
        self.assertIn("a notification of ConnectivityService", problems[0])
        problems = runner.warning_problems(self.warned(activities=[PORTAL_ACTIVITIES]), probes_failed=True)
        self.assertEqual(len(problems), 2)
        self.assertIn("the captive portal login activity", problems[0])

    def test_a_check_that_the_log_does_not_report_is_a_problem(self) -> None:
        guest = self.warned(network_stack=[PASSED_NETWORK_STACK])
        problems = runner.warning_problems(guest, probes_failed=True)
        self.assertEqual(problems, [f"the validation log has no line {runner.CHECK_FAILED_LINE!r}"])

    def test_a_page_answer_needs_no_failed_probe_in_the_log(self) -> None:
        page_log = NETWORK_STACK.replace("Probe failed with exception", "time=3ms ret=200")
        self.assertEqual(runner.warning_problems(self.warned(network_stack=[page_log]), probes_failed=False), [])
        self.assertEqual(len(runner.warning_problems(self.warned(network_stack=[page_log]), probes_failed=True)), 2)

    def test_a_screen_without_the_marks_is_a_problem(self) -> None:
        problems = runner.warning_problems(self.warned(ui=[CLEAR_SCREEN]), probes_failed=True)
        self.assertEqual(len(problems), 2)

    def test_a_cleared_warning_has_the_passing_line_and_no_mark(self) -> None:
        guest = self.warned(network_stack=[PASSED_NETWORK_STACK], ui=[CLEAR_SCREEN])
        self.assertEqual(runner.cleared_problems(guest), [])
        still = self.warned(network_stack=[PASSED_NETWORK_STACK])
        self.assertEqual(len(runner.cleared_problems(still)), 3)

    def test_the_warning_that_shows_late_is_waited_for(self) -> None:
        guest = self.warned(ui=[CLEAR_SCREEN, CLEAR_SCREEN, WARNING_SCREEN])
        runner.wait_problems("the warning", 60, guest, lambda: runner.warning_problems(guest, True))

    def test_a_warning_that_never_shows_fails_after_its_deadline(self) -> None:
        guest = self.warned(ui=[CLEAR_SCREEN])
        with self.assertRaisesRegex(runner.Failure, r"the warning: .*does not say 'Limited connection'.*\(after 60 s\)"):
            runner.wait_problems("the warning", 60, guest, lambda: runner.warning_problems(guest, True))

    def test_the_hold_passes_for_a_network_that_stays_as_it_is(self) -> None:
        runner.hold_validated(self.warned(), 20)

    def test_the_hold_fails_for_a_network_that_is_disabled_or_left_or_not_validated(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "became NETWORK_SELECTION_PERMANENTLY_DISABLED"):
            runner.hold_validated(self.warned(dump=[DISABLED_DUMP]), 20)
        with self.assertRaisesRegex(runner.Failure, "Auto-connect is off"):
            runner.hold_validated(self.warned(dump=[AUTOJOIN_OFF_DUMP]), 20)
        with self.assertRaisesRegex(runner.Failure, "station left"):
            runner.hold_validated(self.warned(status=[NOT_JOINED]), 20)
        with self.assertRaisesRegex(runner.Failure, "not validated"):
            runner.hold_validated(self.warned(connectivity=[agent(102, STOCK)]), 20)
        with self.assertRaisesRegex(runner.Failure, "ConnectivityService"):
            runner.hold_validated(self.warned(notifications=[SIGN_IN_NOTIFICATIONS]), 20)

    def stubbed(self, **changes: list[str]) -> StubbedGuest:
        answers = {"connectivity": [CONNECTIVITY], "network_stack": [NETWORK_STACK],
                   "notifications": [QUIET_NOTIFICATIONS], "activities": [QUIET_ACTIVITIES],
                   "ui": [WARNING_SCREEN], "status": [JOINED], "dump": [ENABLED_DUMP]}
        answers.update(changes)
        return StubbedGuest(**answers)

    def test_a_failed_check_section_sets_only_the_probes_and_joins_the_network_again(self) -> None:
        guest = self.stubbed()
        self.assertIn("validated, joined and enabled", runner.check_failed_check_warns(guest))
        self.assertEqual(guest.calls[0], f"delete {sorted((*runner.PRIVATE_DNS_SETTINGS, runner.USE_HTTPS_SETTING))}")
        self.assertEqual(guest.calls[1], f"put {sorted(runner.CHECK_SETTINGS)}")
        self.assertEqual(guest.calls[2:4], ["forget and save", "joined the station joins the network saved again"])
        self.assertIn("reevaluate", guest.calls)

    def test_a_failed_check_section_fails_when_the_mark_never_shows(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "shows only the warning: .*Limited connection"):
            runner.check_failed_check_warns(self.stubbed(ui=[CLEAR_SCREEN]))

    def test_the_warning_after_a_wake_waits_for_the_station_and_the_mark(self) -> None:
        guest = self.stubbed(ui=[CLEAR_SCREEN, WARNING_SCREEN])
        self.assertIsNone(runner.check_warning_after_wake(guest))
        self.assertEqual(guest.calls[0], "suspend and wake")

    def test_a_page_answer_section_needs_a_request_at_the_server(self) -> None:
        server = runner.ProbeServer()
        self.addCleanup(server.close)
        guest = self.stubbed()
        with self.assertRaisesRegex(runner.Failure, "a probe reaches the host's server: the server has seen no request"):
            runner.check_page_answer_warns(guest, server, "redirect")
        self.assertEqual(server.mode, "redirect")
        self.assertEqual(guest.calls[0], f"reverse {server.port}")

        class Asked(StubbedGuest):
            def forget_and_save(self) -> None:
                super().forget_and_save()
                server.record("/generate_204")

        guest = Asked(connectivity=[CONNECTIVITY], network_stack=[NETWORK_STACK], notifications=[QUIET_NOTIFICATIONS],
                      activities=[QUIET_ACTIVITIES], ui=[WARNING_SCREEN], status=[JOINED], dump=[ENABLED_DUMP])
        self.assertEqual(runner.check_page_answer_warns(guest, server, "page"), "1 probe requests answered")

    def test_the_page_sections_delete_every_setting_and_the_reverse_even_when_one_fails(self) -> None:
        server = runner.ProbeServer()
        self.addCleanup(server.close)
        guest = self.stubbed(ui=[CLEAR_SCREEN])
        report = runner.Report()
        with contextlib.redirect_stderr(io.StringIO()):
            runner.warning_sections(guest, server, report)
        self.assertEqual(len(report.failures), 6)
        self.assertEqual(guest.calls[-2:], [f"delete {sorted(runner.ALL_CHECK_SETTINGS)}", f"unreverse {server.port}"])

    def test_the_page_settings_point_the_probes_at_the_server_and_the_black_hole(self) -> None:
        settings = runner.page_settings(41234)
        self.assertEqual(settings["captive_portal_http_url"], "http://127.0.0.1:41234/generate_204")
        self.assertEqual(settings["captive_portal_fallback_url"], "http://127.0.0.1:41234/gen_204")
        self.assertIn(runner.BLACKHOLE, settings["captive_portal_https_url"])
        # The strict private DNS name fails the network itself, so no warning section sets it.
        for settings in (runner.CHECK_SETTINGS, runner.page_settings(1)):
            self.assertFalse([key for key in settings if key.startswith("private_dns")])
        self.assertEqual(set(runner.ALL_CHECK_SETTINGS), set(runner.PROBE_SETTINGS) | {runner.USE_HTTPS_SETTING})


class UpgradeSectionTests(unittest.TestCase):
    def setUp(self) -> None:
        clock = mock.patch.object(runner, "time", FakeClock())
        clock.start()
        self.addCleanup(clock.stop)
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)

    def test_the_previous_release_option_is_given_once_for_each_release(self) -> None:
        parser = runner.build_parser()
        options = parser.parse_args(["--previous-release", "r22", "--previous-release", "r23"])
        self.assertEqual([str(path) for path in options.previous_release], ["r22", "r23"])
        self.assertIsNone(parser.parse_args([]).previous_release)

    def test_a_previous_release_with_the_overlay_never_disables_the_network(self) -> None:
        with mock.patch.object(runner, "check_overlay", return_value=None):
            self.assertTrue(runner.previous_never_disables(ScriptedGuest()))
        with mock.patch.object(runner, "check_overlay", side_effect=runner.Failure("the overlay is not enabled")):
            self.assertFalse(runner.previous_never_disables(ScriptedGuest()))

    def test_a_disk_the_release_leaves_alone_keeps_its_marker_and_its_metered_setting(self) -> None:
        guest = ScriptedGuest(status=[JOINED], dump=[ENABLED_DUMP.replace("meteredOverride 0", "meteredOverride 1")],
                              marker=["2"], script_log=[""])
        self.assertIsNone(runner.check_left_alone(guest))

    def test_a_marker_that_is_not_2_fails(self) -> None:
        guest = ScriptedGuest(status=[JOINED], dump=[ENABLED_DUMP.replace("meteredOverride 0", "meteredOverride 1")],
                              marker=["1"], script_log=[""])
        with self.assertRaisesRegex(runner.Failure, "marker is '1'"):
            runner.check_left_alone(guest)

    def test_a_network_saved_again_loses_its_metered_setting(self) -> None:
        guest = ScriptedGuest(status=[JOINED], dump=[ENABLED_DUMP], marker=["2"], script_log=[""])
        with self.assertRaisesRegex(runner.Failure, "reset its metered setting"):
            runner.check_left_alone(guest)

    def test_a_repair_line_on_a_disk_that_needs_none_fails(self) -> None:
        guest = ScriptedGuest(status=[JOINED], dump=[ENABLED_DUMP.replace("meteredOverride 0", "meteredOverride 1")],
                              marker=["2"], script_log=[REPAIR_LINE])
        with self.assertRaisesRegex(runner.Failure, "saved the network again although the station had joined"):
            runner.check_left_alone(guest)

    def settings_guest(self, held: dict[str, str]) -> ScriptedGuest:
        class Settings(StubbedGuest):
            def shell(self, command: str, timeout: float = runner.COMMAND_SECONDS) -> str:
                if command.startswith("settings get global "):
                    return held.get(command.removeprefix("settings get global "), "null") + "\n"
                return super().shell(command, timeout)

        return Settings(connectivity=[CONNECTIVITY], network_stack=[NETWORK_STACK], notifications=[QUIET_NOTIFICATIONS],
                        activities=[QUIET_ACTIVITIES], ui=[WARNING_SCREEN], status=[JOINED], dump=[ENABLED_DUMP])

    def test_the_probe_settings_the_disk_holds_are_read_back(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "lost the probe settings"):
            runner.check_warning_on_disk(self.settings_guest({}))
        self.assertIsNone(runner.check_warning_on_disk(self.settings_guest(dict(runner.CHECK_SETTINGS))))


class RunnerNamesTests(unittest.TestCase):
    def test_the_log_lines_are_the_ones_the_verifier_requires_in_the_image(self) -> None:
        verifier = runner.load_verifier()
        required = [marker.decode() for marker in verifier.NETWORK_STACK_MARKERS]
        self.assertIn(runner.CHECK_FAILED_LINE, required)
        # The first line is the validation before the check; the runner reads the check's results.
        self.assertEqual(len(required), 2)

    def test_the_words_are_androids_own(self) -> None:
        # WifiTrackerLib's wifitrackerlib_wifi_limited_connection and SystemUI's "No internet" description.
        self.assertEqual((runner.LIMITED_CONNECTION, runner.NO_INTERNET), ("Limited connection", "No internet"))


if __name__ == "__main__":
    unittest.main()
