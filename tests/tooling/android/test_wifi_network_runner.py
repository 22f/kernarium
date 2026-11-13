#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Wi-Fi network runner's reading of the guest, without a guest.

run_android_wifi_network.py reads `cmd wifi status`, `cmd wifi list-networks`,
`dumpsys wifi` and `cmd overlay list`. The texts below have the layout those
commands print (WifiShellCommand, WifiConfiguration's dump and the overlay
manager), so the runner's parsers run here; the runner also takes the boot
script's waits from the tracked script, and reads the overlay's integers with the
verifier's reader of compiled resource tables. The upgrade section's checks run
against a guest that answers from a script, on a clock that a sleep moves on.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
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
                "script_log": f"logcat -d -s {runner.SCRIPT_LOG_TAG}", "log": "logcat -d -b all"}

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


NETWORK_STACK = """\
Validation logs (most recent first):
100 - "VirtWifi"
  2026-11-12T10:33:06.272454 - Wi-Fi is validated without probes, resolving private DNS

useNeighborResource: false
"""
STOCK_NETWORK_STACK = """\
Validation logs (most recent first):
100 - "VirtWifi"
  2026-11-12T10:33:06.272454 - Probing https://connectivitycheck.gstatic.com/generate_204
"""
CONNECTIVITY = (
    "  NetworkAgentInfo{network{100}  handle{432902426637}  ni{WIFI CONNECTED extra: } created=2026-11-12T10:33:01.583Z "
    "Score(Policies : TRANSPORT_PRIMARY&EVER_EVALUATED&IS_UNMETERED&EVER_VALIDATED&IS_VALIDATED ; KeepConnected : 0) "
    "lp{{InterfaceName: wlan0}}\n")
CONNECTIVITY_NOT_VALIDATED = CONNECTIVITY.replace("&IS_VALIDATED", "")
NOTHING_COUNTED = " NetworkSelectionStatus NETWORK_SELECTION_ENABLED\n hasEverValidatedInternetAccess: true\n"


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


class UnprobedValidationTests(unittest.TestCase):
    """The platform of this image validates Wi-Fi without probes (route p0029), so it counts no failed internet check."""

    class Guest(runner.Guest):
        def __init__(self, dump: str, network_stack: str, connectivity: str) -> None:
            self.texts = (dump, network_stack, connectivity)

        def inject_failing_checks(self) -> None:
            pass

        def forget_and_save(self) -> None:
            pass

        def wait_joined(self, seconds: float, what: str) -> None:
            pass

        def joined(self) -> bool:
            return True

        def alive(self) -> None:
            pass

        def dump(self) -> str:
            return self.texts[0]

        def network_stack_dump(self) -> str:
            return self.texts[1]

        def connectivity_dump(self) -> str:
            return self.texts[2]

    def setUp(self) -> None:
        clock = mock.patch.object(runner, "time", FakeClock())
        clock.start()
        self.addCleanup(clock.stop)

    def test_the_marker_and_the_validated_network_are_read(self) -> None:
        self.assertTrue(runner.validated_without_probes(NETWORK_STACK))
        self.assertFalse(runner.validated_without_probes(STOCK_NETWORK_STACK))
        self.assertTrue(runner.wifi_validated(CONNECTIVITY))
        self.assertFalse(runner.wifi_validated(CONNECTIVITY_NOT_VALIDATED))
        self.assertFalse(runner.wifi_validated("  NetworkAgentInfo{network{101} ni{MOBILE CONNECTED} Score(IS_VALIDATED)\n"))

    def test_counted_failures_pass_as_before(self) -> None:
        guest = self.Guest(ENABLED_DUMP, "", "")
        self.assertIn("5 failed internet checks counted", runner.check_not_disabled(guest))

    def test_nothing_counted_passes_when_the_platform_validated_without_probes(self) -> None:
        guest = self.Guest(NOTHING_COUNTED, NETWORK_STACK, CONNECTIVITY)
        self.assertIn("validates Wi-Fi without probes", runner.check_not_disabled(guest))

    def test_nothing_counted_without_the_marker_is_the_old_vacuity_failure(self) -> None:
        guest = self.Guest(NOTHING_COUNTED, STOCK_NETWORK_STACK, CONNECTIVITY)
        with self.assertRaisesRegex(runner.Failure, "does not say that it validated Wi-Fi without probes"):
            runner.check_not_disabled(guest)

    def test_a_network_that_is_not_validated_is_reported_not_failed(self) -> None:
        # The injected strict private DNS name does not resolve, so the platform reports the network as not validated.
        guest = self.Guest(NOTHING_COUNTED, NETWORK_STACK, CONNECTIVITY_NOT_VALIDATED)
        self.assertIn("not validated", runner.check_not_disabled(guest))
        guest = self.Guest(NOTHING_COUNTED, NETWORK_STACK, CONNECTIVITY)
        self.assertRegex(runner.check_not_disabled(guest), r"joined, validated$")

    def test_a_disabled_network_still_fails(self) -> None:
        guest = self.Guest(DISABLED_DUMP, NETWORK_STACK, CONNECTIVITY)
        with self.assertRaisesRegex(runner.Failure, "became NETWORK_SELECTION_PERMANENTLY_DISABLED"):
            runner.check_not_disabled(guest)


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


if __name__ == "__main__":
    unittest.main()
