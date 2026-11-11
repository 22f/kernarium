#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Wi-Fi network runner's reading of the guest, without a guest.

run_android_wifi_network.py reads `cmd wifi status`, `cmd wifi list-networks`,
`dumpsys wifi` and `cmd overlay list`. The texts below have the layout those
commands print (WifiShellCommand, WifiConfiguration's dump and the overlay
manager), so the runner's parsers run here; the runner also takes the boot
script's settle time from the tracked script, and reads the overlay's integers
with the verifier's reader of compiled resource tables.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
import io
from pathlib import Path
import sys
import unittest
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
"""


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
    def test_the_settle_time_comes_from_the_tracked_script(self) -> None:
        settle = runner.boot_settle_seconds(runner.BOOT_SCRIPT.read_text(encoding="utf-8"))
        self.assertGreater(settle, 0)
        self.assertLessEqual(settle, 120)
        with self.assertRaises(runner.Failure):
            runner.boot_settle_seconds("marker=1\n")

    def test_the_tracked_overlay_holds_what_the_runner_expects(self) -> None:
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


if __name__ == "__main__":
    unittest.main()
