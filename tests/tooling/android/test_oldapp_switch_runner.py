#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Old app compatibility runner without a guest.

run_android_oldapp_switch.py drives the image's `kernarium_oldapp` command and checks the property's policy
from an ordinary application. Here a scripted guest plays an image that behaves as the contract says, and
each way the image can differ makes one step fail: the probe packages (manifests, the stand-in library, the
probe's scripts), the parsers of the listing and of the probe's result, and every step against the scripted
guest, which keeps installed packages with their first-install times, processes, and the layer's mapping.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import pathlib
import re
import shlex
import struct
import sys
import tempfile
import unittest
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
INTEGRATION = HERE.parents[1] / "integration" / "android"
sys.path.insert(0, str(INTEGRATION))
import oldswitch_probe_apk as probe  # noqa: E402
import orientation_probe_apk as binary_xml  # noqa: E402
import property_probe_dex  # noqa: E402
import run_android_oldapp_switch as runner  # noqa: E402

Failure = runner.Failure
KEY_PART = re.compile(r"[A-Za-z0-9_@:-]+(\.[A-Za-z0-9_@:-]+)*")
HOOK_DOMAIN = "u:r:untrusted_app_25:s0:c512,c768"
AVC = ("11-15 17:00:00.000   500   500 W auditd : avc:  denied  { set } for property=persist.sys.kernarium.oldapp."
       "org.example.andarium.oldswitchprobe.running pid=4001 uid=10200 gid=10200 scontext=u:r:untrusted_app_25:s0:c512,c768 "
       "tcontext=u:object_r:kernarium_oldapp_prop:s0 tclass=property_service permissive=0")


class ScriptedGuest:
    """An image that follows the contract: packages with first-install times, processes, and the layer's
    mapping. Attributes switch one way of differing on."""

    def __init__(self) -> None:
        self.properties: dict[str, str] = {}
        self.clock = 1_700_000_000
        self.packages: dict[str, tuple[str, int]] = {}
        self.processes: dict[str, int] = {}
        self.results: dict[str, str] = {}
        self.next_pid = 4000
        self.commands: list[str] = []
        self.real = {"real": ("org.example.real", "Real Game")}
        # Ways to differ.
        self.list_extra: list[str] = []
        self.label: str | None = None
        self.refuse_with = 2
        self.app_can_set = False
        self.app_view_value: str | None = None
        self.domain = "u:r:untrusted_app_29:s0:c512,c768"
        self.hook_domain = HOOK_DOMAIN
        self.shell_context = "u:r:shell:s0"
        self.path = runner.WRAPPER_PATH
        self.keep_on_off = True
        self.result_file: str | None = None
        self.version_line = runner.VERSION_LINE
        self.on_time_offset = 0
        self.skip_cleanup: set[str] = set()      # causes `list` does not clear: absent, ineligible, reinstalled
        self.off_needs_eligible = False
        self.accept_unrepresentable = False
        self.layer = "when-on"                    # when-on, never, always
        self.keep_process = False                 # set does not end the application's process
        self.running_can_set = False
        self.avc_in_log = True
        for variant in probe.VARIANTS:
            self.install_variant(variant)

    # ---- the model
    def install_variant(self, variant: str, package: str | None = None) -> None:
        self.packages[package or probe.package_of(variant)] = (variant, self.clock)
        self.processes.pop(package or probe.package_of(variant), None)

    def eligible(self, package: str) -> bool:
        if package not in self.packages:
            return False
        variant = self.packages[package][0]
        if variant == "real":
            return True
        _, _, target, native, shared, _ = probe.SPECS[variant]
        return target < 14 and native and shared is None and KEY_PART.fullmatch(package) is not None

    def label_of(self, package: str) -> str:
        variant = self.packages[package][0]
        if variant == "real":
            return self.real["real"][1]
        if self.label is not None:
            return self.label
        return (probe.LISTED_LABEL if variant in ("old", "raised", "oldshared") else f"Switch Probe {variant}")

    def stored(self) -> dict[str, str]:
        return {name[len(probe.STORE_PROPERTY_PREFIX):]: value for name, value in self.properties.items()
                if name.startswith(probe.STORE_PROPERTY_PREFIX) and value}

    def clear_stale(self) -> None:
        for package, value in self.stored().items():
            if package not in self.packages:
                cause = "absent"
            elif not self.eligible(package):
                cause = "ineligible"
            elif value.isdigit() and self.packages[package][1] > int(value):
                cause = "reinstalled"
            else:
                continue
            if cause not in self.skip_cleanup:
                self.properties[probe.STORE_PROPERTY_PREFIX + package] = ""

    def adb_run(self, *arguments: str, timeout: float = 30):
        self.commands.append("adb " + " ".join(arguments))
        if arguments[:1] == ("install",):
            variant = pathlib.Path(arguments[-1]).stem
            self.clock += 3
            if variant in self.real:
                self.packages[self.real[variant][0]] = ("real", self.clock)
            else:
                self.install_variant(variant)
        elif arguments[:1] == ("uninstall",):
            self.packages.pop(arguments[1], None)
            self.processes.pop(arguments[1], None)

        class Completed:
            returncode = 0
            stdout = "Success\n"
            stderr = ""
        return Completed()

    def shell(self, command: str, timeout: float = 30) -> str:
        result = self.run(command, timeout)
        if result.status != 0:
            raise Failure(f"'{command}' exited {result.status}: {result.err}")
        return result.out

    def run(self, command: str, timeout: float = 120) -> runner.Run:
        self.commands.append(command)
        words = shlex.split(command)
        if words[:1] == ["date"]:
            self.clock += 1
            return runner.Run(0, f"{self.clock}\n", "")
        if words[:1] == ["id"]:
            return runner.Run(0, self.shell_context + "\n", "")
        if words[:1] == ["ls"]:
            return runner.Run(0, "\n".join(f"-rwxr-xr-x 1 root shell u:object_r:system_file:s0 1 {path}"
                                           for path in words[2:]) + "\n", "")
        if words[:2] == ["command", "-v"]:
            return runner.Run(0, self.path + "\n", "")
        if words[:1] == ["getprop"]:
            return runner.Run(0, self.properties.get(words[1], "") + "\n", "")
        if words[:1] == ["pidof"]:
            pid = self.processes.get(words[1])
            return runner.Run(0 if pid else 1, f"{pid}\n" if pid else "", "")
        if words[:1] == ["logcat"]:
            return runner.Run(0, (AVC + "\n") if self.avc_in_log else "I/other: nothing\n", "")
        if words[:3] == ["am", "broadcast", "-n"]:
            return self.broadcast(words[3])
        if words[:1] == ["run-as"]:
            if words[2] == "rm":
                self.results.pop(words[1], None)
                return runner.Run(0, "", "")
            if self.result_file is not None:
                return runner.Run(0, self.result_file, "")
            return runner.Run(0, self.results.get(words[1], ""), "")
        if words[:1] == [runner.COMMAND]:
            return self.command(words[1:])
        raise AssertionError(f"unscripted command: {command}")

    def broadcast(self, component: str) -> runner.Run:
        package = component.split("/")[0]
        variant = self.packages[package][0]
        if package not in self.processes:
            self.next_pid += 1
            self.processes[package] = self.next_pid
        name = probe.STORE_PROPERTY_PREFIX + (package if variant == "running" else probe.package_of("old"))
        value = self.properties.get(name, "")
        if variant == "running":
            mapped = {"when-on": bool(value), "never": False, "always": True}[self.layer]
            after = "7" if self.running_can_set else value
            self.results[package] = (f"context={self.hook_domain}\npid={self.processes[package]}\n"
                                     f"layer={1 if mapped else 0}\nget={value}\n"
                                     f"set={'allowed' if self.running_can_set else 'denied'}\nafter={after}\ndone\n")
        else:
            after = "7" if self.app_can_set else value
            self.results[package] = (f"context={self.domain}\nget={value if self.app_view_value is None else self.app_view_value}\n"
                                     f"set={'allowed' if self.app_can_set else 'denied'}\nafter={after}\ndone\n")
        return runner.Run(0, "Broadcast completed: result=0\n", "")

    def command(self, arguments: list[str]) -> runner.Run:
        usage = runner.Run(64, "", "usage: kernarium_oldapp list\n       kernarium_oldapp set PACKAGE on|off\n")
        if arguments == ["list"]:
            self.clear_stale()
            lines = [self.version_line]
            for package in sorted(self.packages):
                if self.eligible(package):
                    value = self.properties.get(probe.STORE_PROPERTY_PREFIX + package, "")
                    lines.append(f"{package}\t{self.label_of(package)}\t{'on' if value else 'off'}\t{value or 0}\t"
                                 f"{self.packages[package][1]}")
            lines += self.list_extra
            return runner.Run(0, "\n".join(lines) + "\n", "")
        if len(arguments) == 3 and arguments[0] == "set" and arguments[2] in ("on", "off"):
            package, mode = arguments[1], arguments[2]
            refused = runner.Run(self.refuse_with, "", f"kernarium_oldapp: {package} is not an application it applies to\n")
            if KEY_PART.fullmatch(package) is None and not self.accept_unrepresentable:
                return refused
            if mode == "on" and not self.eligible(package):
                return refused
            if mode == "off" and self.off_needs_eligible and not self.eligible(package):
                return refused
            name = probe.STORE_PROPERTY_PREFIX + package
            if mode == "on":
                self.properties[name] = str(self.clock + self.on_time_offset)
            elif self.keep_on_off and self.properties.get(name):
                self.properties[name] = ""
            if package in self.packages and not self.keep_process:
                self.processes.pop(package, None)
            return runner.Run(0, f"{package}\t{mode}\n", "")
        return usage


class ProbePackages(unittest.TestCase):
    def test_variants_differ_in_one_clause_each(self) -> None:
        old = probe.VARIANTS["old"]
        self.assertEqual((old[2], old[3], old[4]), (10, True, None))
        self.assertEqual(probe.VARIANTS["modern"][2], 29)
        self.assertTrue(probe.VARIANTS["modern"][3])
        self.assertEqual(probe.VARIANTS["javaonly"][2:5], (10, False, None))
        self.assertEqual(probe.VARIANTS["shared"][2:4], (10, True))
        self.assertIsNotNone(probe.VARIANTS["shared"][4])
        self.assertEqual(set(probe.VARIANTS), set(probe.ELIGIBLE) | set(probe.REFUSED))

    def test_successors_replace_old_under_its_name(self) -> None:
        for variant in probe.SUCCESSORS:
            self.assertEqual(probe.package_of(variant), probe.package_of("old"))
        self.assertEqual(probe.SUCCESSORS["raised"][2], 29)
        self.assertIsNone(probe.SUCCESSORS["raised"][4])
        self.assertEqual(probe.SUCCESSORS["oldshared"][2:5], (10, True, probe.PACKAGE_PREFIX + ".shareduid"))
        running = probe.RUNNING["running"]
        self.assertEqual((running[2], running[3], running[4], running[5]), (10, True, None, True))
        self.assertNotIn(probe.package_of("running"), {probe.package_of(v) for v in probe.VARIANTS})

    def test_manifests_read_back(self) -> None:
        for variant in probe.SPECS:
            tree = binary_xml.decode_manifest(binary_xml.encode_manifest(probe.manifest_tree(variant)))
            package, minimum, target, _, shared, receiver = probe.SPECS[variant]
            self.assertEqual(tree.attributes["package"], package)
            self.assertEqual(tree.attributes.get("sharedUserId"), shared)
            sdk = next(node for node in tree.children if node.tag == "uses-sdk")
            self.assertEqual(sdk.attributes, {"minSdkVersion": minimum, "targetSdkVersion": target})
            application = next(node for node in tree.children if node.tag == "application")
            if variant in ("old", "raised", "oldshared"):
                self.assertEqual(application.attributes["label"], probe.LABEL)
            else:
                self.assertEqual(application.attributes["label"], f"Switch Probe {variant}")
            self.assertEqual(application.attributes.get("debuggable") is True, receiver)
            self.assertEqual([node.attributes["name"] for node in application.children],
                             [probe.RECEIVER] if receiver else [])

    def test_a_receiver_holds_internet(self) -> None:
        """The probe class lists the network interfaces first: without the permission the list is null and it dies."""
        for variant in probe.SPECS:
            tree = binary_xml.decode_manifest(binary_xml.encode_manifest(probe.manifest_tree(variant)))
            permissions = [node.attributes["name"] for node in tree.children if node.tag == "uses-permission"]
            receiver = probe.SPECS[variant][5]
            self.assertEqual(permissions, ["android.permission.INTERNET"] if receiver else [], variant)

    def test_label_has_a_tab_the_listing_turns_into_a_space(self) -> None:
        self.assertIn("\t", probe.LABEL)
        self.assertEqual(probe.LABEL.replace("\t", " "), probe.LISTED_LABEL)

    def test_stand_in_library_is_an_arm_elf32_shared_object(self) -> None:
        image = probe.stand_in_library()
        self.assertEqual(image[:7], b"\x7fELF\x01\x01\x01")
        e_type, machine, _, _, phoff, _, flags, ehsize, phentsize, phnum = struct.unpack_from("<HHIIIIIHHH", image, 16)
        self.assertEqual((e_type, machine, ehsize, phentsize, phnum), (3, 40, 52, 32, 2))
        self.assertEqual(flags >> 24, 5, "EABI version 5")
        kinds = [struct.unpack_from("<I", image, phoff + 32 * index)[0] for index in range(phnum)]
        self.assertEqual(kinds, [1, 2])
        _, offset, _, _, filesz, memsz, _, alignment = struct.unpack_from("<IIIIIIII", image, phoff)
        self.assertEqual((offset, filesz, memsz), (0, len(image), len(image)))
        self.assertEqual(alignment % 0x4000, 0, "aligned for a 16 KiB page as well")
        dynamic_offset, dynamic_size = struct.unpack_from("<II", image, phoff + 32 + 4)[0], struct.unpack_from(
            "<I", image, phoff + 32 + 16)[0]
        self.assertEqual(image[dynamic_offset:dynamic_offset + dynamic_size], bytes(8), "DT_NULL")

    def test_probe_script_reads_then_tries_to_set_the_eligible_property(self) -> None:
        name = probe.property_of("old")
        script = probe.probe_script(probe.package_of("modern"), name)
        self.assertIn(f"getprop {name}", script)
        self.assertIn(f"setprop {name} 7", script)
        self.assertLess(script.index("get="), script.index("setprop"))
        self.assertLess(script.index("setprop"), script.index("after="))
        self.assertIn(f"/data/data/{probe.package_of('modern')}/files", script)
        dex = property_probe_dex.build_dex(probe.package_of("modern"), name, name, shell_script=script)
        self.assertIn(script.encode(), dex)
        self.assertNotIn(property_probe_dex.script(probe.package_of("modern"), name, name).encode(), dex)
        default = property_probe_dex.build_dex(probe.package_of("modern"), name, name)
        self.assertIn(property_probe_dex.script(probe.package_of("modern"), name, name).encode(), default)

    def test_running_script_reports_the_process_and_the_layer(self) -> None:
        name = probe.property_of("running")
        script = probe.running_script(probe.package_of("running"), name)
        # The shell's parent is the application's process: its id and its memory map.
        self.assertIn("pid=$PPID", script)
        self.assertIn(f"grep -c {probe.LAYER_LIBRARY} /proc/$PPID/maps", script)
        self.assertEqual(probe.LAYER_LIBRARY, "libkernarium_oldapp.so")
        self.assertIn(f"getprop {name}", script)
        self.assertIn(f"setprop {name} 7", script)
        self.assertLess(script.index("get="), script.index("setprop"))
        self.assertLess(script.index("setprop"), script.index("after="))
        self.assertTrue(script.rstrip().endswith("mv $d/result.tmp $d/result"))

    def test_signed_packages_hold_the_files_the_manifest_needs(self) -> None:
        try:
            with tempfile.TemporaryDirectory(prefix="uml-oldswitch-test-") as scratch:
                directory = pathlib.Path(scratch)
                names = {variant: zipfile.ZipFile(probe.build_apk(directory / f"{variant}.apk", variant)).namelist()
                         for variant in probe.SPECS}
        except FileNotFoundError:
            self.skipTest("keytool and jarsigner are not on PATH")
        for variant in ("old", "modern", "shared", "raised", "oldshared", "running"):
            self.assertIn(probe.LIBRARY, names[variant], variant)
        self.assertNotIn(probe.LIBRARY, names["javaonly"])
        for variant in ("modern", "running"):
            self.assertIn("classes.dex", names[variant])
        for variant in ("old", "raised", "oldshared", "javaonly", "shared"):
            self.assertNotIn("classes.dex", names[variant])


class Parsers(unittest.TestCase):
    def test_listing(self) -> None:
        rows = runner.parse_listing("kernarium_oldapp 1\na.b\tA B\toff\t0\t5\nc.d\tC\ton\t17\t9\textra\n")
        self.assertEqual(rows, [runner.Row("a.b", "A B", "off", 0, 5), runner.Row("c.d", "C", "on", 17, 9)])
        self.assertEqual(runner.parse_listing("kernarium_oldapp 1\n"), [])
        for bad in ("", "kernarium_oldapp 2\n", "kernarium_oldapp 1", "kernarium_oldapp 1\na\tb\tc\n",
                    "kernarium_oldapp 1\na\tb\tmaybe\t0\t1\n", "kernarium_oldapp 1\na\tb\ton\tx\t1\n",
                    "kernarium_oldapp 1\na\tb\ton\t1\t-1\n", "hello\n"):
            with self.assertRaises(Failure, msg=repr(bad)):
                runner.parse_listing(bad)

    def test_probe_result(self) -> None:
        self.assertEqual(runner.parse_result("a=1\nb = two\nnoise\n=x\ndone\n"),
                         {"a": "1", "b": "two", "noise": "", "done": ""})
        good = {"context": "u:r:untrusted_app_29:s0:c1,c2", "get": "5", "set": "denied", "after": "5", "done": ""}
        self.assertIn("untrusted_app_29", runner.evaluate_probe(good, "5"))
        for key, value in (("context", "u:r:shell:s0"), ("get", "6"), ("set", "allowed"), ("after", "7")):
            with self.assertRaises(Failure, msg=key):
                runner.evaluate_probe({**good, key: value}, "5")
        with self.assertRaises(Failure):
            runner.evaluate_probe({k: v for k, v in good.items() if k != "done"}, "5")

    def test_process_result(self) -> None:
        good = {"context": HOOK_DOMAIN, "pid": "4001", "layer": "1", "get": "5", "set": "denied", "after": "5",
                "done": ""}
        self.assertEqual(runner.evaluate_process(good, "5", mapped=True), 4001)
        self.assertEqual(runner.evaluate_process({**good, "layer": "0", "get": "", "after": ""}, "", mapped=False), 4001)
        for key, value in (("context", "u:r:untrusted_app_29:s0"), ("get", "6"), ("set", "allowed"), ("after", "7"),
                           ("pid", ""), ("layer", ""), ("layer", "0")):
            with self.assertRaises(Failure, msg=f"{key}={value}"):
                runner.evaluate_process({**good, key: value}, "5", mapped=True)
        with self.assertRaises(Failure):
            runner.evaluate_process(good, "5", mapped=False)


class Steps(unittest.TestCase):
    def setUp(self) -> None:
        self.gap, self.avc_wait = runner.INSTALL_GAP_SECONDS, runner.AVC_SECONDS
        runner.INSTALL_GAP_SECONDS = 0
        runner.AVC_SECONDS = 0

    def tearDown(self) -> None:
        runner.INSTALL_GAP_SECONDS, runner.AVC_SECONDS = self.gap, self.avc_wait

    def guest(self) -> ScriptedGuest:
        return ScriptedGuest()

    def apks(self) -> dict[str, pathlib.Path]:
        return {variant: pathlib.Path(f"{variant}.apk") for variant in probe.SPECS}

    def test_command_present(self) -> None:
        guest = self.guest()
        self.assertIn(runner.WRAPPER_PATH, runner.command_present(guest))
        guest.path = "/system/bin/kernarium_oldapp"
        with self.assertRaises(Failure):
            runner.command_present(guest)
        guest = self.guest()
        guest.shell_context = "u:r:su:s0"
        with self.assertRaises(Failure):
            runner.command_present(guest)

    def test_listing(self) -> None:
        guest = self.guest()
        self.assertIn("1 application(s) listed", runner.check_listing(guest, guest.clock - 60))
        for change in ("label", "extra-refused", "extra-system", "version", "stale", "future"):
            guest = self.guest()
            installed_at = guest.clock - 60
            if change == "label":
                guest.label = "Old Switch\tProbe"
            elif change == "extra-refused":
                guest.list_extra = [f"{probe.package_of('modern')}\tM\toff\t0\t{guest.clock}"]
            elif change == "extra-system":
                guest.list_extra = [f"{runner.SYSTEM_PACKAGE}\tSettings\toff\t0\t1"]
            elif change == "version":
                guest.version_line = "kernarium_oldapp 2"
            elif change == "stale":
                installed_at = guest.clock + 1000
            elif change == "future":
                guest.packages[probe.package_of("old")] = ("old", guest.clock + 1000)
            with self.assertRaises(Failure, msg=change):
                runner.check_listing(guest, installed_at)
        guest = self.guest()
        del guest.packages[probe.package_of("old")]
        with self.assertRaises(Failure):
            runner.check_listing(guest, guest.clock - 60)

    def test_refusals(self) -> None:
        guest = self.guest()
        self.assertIn("status 64", runner.check_refusals(guest))
        self.assertIn("off accepted", runner.check_refusals(guest))
        guest = self.guest()
        guest.refuse_with = 1
        with self.assertRaises(Failure):
            runner.check_refusals(guest)
        # An image that lets a refused package through.
        guest = self.guest()
        guest.install_variant("old", probe.package_of("modern"))
        with self.assertRaises(Failure):
            runner.check_refusals(guest)

    def test_off_is_accepted_for_any_name(self) -> None:
        guest = self.guest()
        guest.off_needs_eligible = True
        with self.assertRaises(Failure) as problem:
            runner.check_refusals(guest)
        self.assertIn("off", str(problem.exception))

    def test_a_name_that_cannot_be_a_property_name_is_refused(self) -> None:
        guest = self.guest()
        guest.accept_unrepresentable = True
        with self.assertRaises(Failure) as problem:
            runner.check_refusals(guest)
        self.assertIn("cannot be a property name", str(problem.exception))

    def test_switching_and_the_application_view(self) -> None:
        guest = self.guest()
        note, value = runner.switch_on(guest)
        self.assertIn(str(value), note)
        self.assertEqual(guest.properties[probe.property_of("old")], str(value))
        self.assertIn("could not set it", runner.application_view(guest, value))
        self.assertIn("is empty", runner.switch_off(guest))
        self.assertEqual(guest.properties[probe.property_of("old")], "")

    def test_switch_on_failures(self) -> None:
        guest = self.guest()
        guest.on_time_offset = -1000
        with self.assertRaises(Failure):
            runner.switch_on(guest)

    def test_the_application_can_set(self) -> None:
        guest = self.guest()
        _, value = runner.switch_on(guest)
        guest.app_can_set = True
        with self.assertRaises(Failure):
            runner.application_view(guest, value)

    def test_the_application_cannot_read(self) -> None:
        guest = self.guest()
        _, value = runner.switch_on(guest)
        guest.app_view_value = ""
        with self.assertRaises(Failure):
            runner.application_view(guest, value)

    def test_the_application_is_not_an_ordinary_one(self) -> None:
        guest = self.guest()
        _, value = runner.switch_on(guest)
        guest.domain = "u:r:shell:s0"
        with self.assertRaises(Failure):
            runner.application_view(guest, value)

    def test_switch_off_failures(self) -> None:
        guest = self.guest()
        runner.switch_on(guest)
        guest.keep_on_off = False
        with self.assertRaises(Failure):
            runner.switch_off(guest)

    def test_lifecycle(self) -> None:
        guest = self.guest()
        note = runner.check_lifecycle(guest, self.apks())
        for what in ("reinstalled: cleared", "a raised target level: cleared", "a shared user ID: cleared",
                     "uninstalled: cleared"):
            self.assertIn(what, note)
        self.assertEqual(guest.properties[probe.property_of("old")], "")

    def test_lifecycle_fails_when_the_listing_does_not_clear(self) -> None:
        for cause in ("reinstalled", "ineligible", "absent"):
            guest = self.guest()
            guest.skip_cleanup = {cause}
            with self.assertRaises(Failure, msg=cause) as problem:
                runner.check_lifecycle(guest, self.apks())
            self.assertRegex(str(problem.exception), "holds|listing shows|is listed", cause)

    def test_lifecycle_needs_off_for_an_ineligible_package(self) -> None:
        guest = self.guest()
        guest.off_needs_eligible = True
        with self.assertRaises(Failure):
            runner.check_lifecycle(guest, self.apks())

    def test_lifecycle_notices_a_property_that_did_not_survive(self) -> None:
        """If an image dropped the choice at uninstall, the cleanup would not be exercised: the step says so."""
        guest = self.guest()
        original = guest.adb_run

        def dropping(*arguments: str, timeout: float = 30):
            if arguments[:1] == ("install",):
                guest.properties[probe.property_of("old")] = ""
            return original(*arguments, timeout=timeout)

        guest.adb_run = dropping  # type: ignore[method-assign]
        with self.assertRaises(Failure) as problem:
            runner.check_lifecycle(guest, self.apks())
        self.assertIn("not exercised", str(problem.exception))

    def hook(self, guest: ScriptedGuest) -> runner.Report:
        report = runner.Report()
        runner.check_hook(guest, self.apks(), report)
        return report

    def test_hook_steps_pass_on_an_image_that_follows_the_contract(self) -> None:
        guest = self.guest()
        report = self.hook(guest)
        self.assertEqual([status for _, status, _ in report.rows], ["PASS"] * 8, report.rows)
        self.assertNotIn(probe.package_of("running"), guest.packages, "the probe is uninstalled again")

    def test_hook_steps_notice_each_difference(self) -> None:
        for change, failing in (
            ("layer-never", "on, the new process maps the layer and reads the property"),
            ("layer-always", "off, the old application's process has no layer"),
            ("keeps-process", "set on ends the application's process"),
            ("running-can-set", "on, the new process maps the layer and reads the property"),
            ("wrong-domain", "off, the old application's process has no layer"),
            ("no-cleanup", "a stale choice is gone before the next process"),
        ):
            guest = self.guest()
            if change == "layer-never":
                guest.layer = "never"
            elif change == "layer-always":
                guest.layer = "always"
            elif change == "keeps-process":
                guest.keep_process = True
            elif change == "running-can-set":
                guest.running_can_set = True
            elif change == "wrong-domain":
                guest.hook_domain = "u:r:untrusted_app_29:s0"
            elif change == "no-cleanup":
                guest.skip_cleanup = {"reinstalled"}
            report = self.hook(guest)
            names = [name for name, status, _ in report.rows if status == "FAIL"]
            self.assertIn("hook: " + failing, names, f"{change}: {report.rows}")

    def test_hook_avc_line_is_a_note_when_the_log_shows_none(self) -> None:
        guest = self.guest()
        guest.avc_in_log = False
        report = self.hook(guest)
        statuses = {name: status for name, status, _ in report.rows}
        self.assertEqual(statuses["hook: the denied set leaves an avc line"], "SKIP")
        self.assertFalse(report.failed, report.rows)
        with_line = self.hook(self.guest())
        self.assertIn("kernarium_oldapp_prop", dict((n, d) for n, _, d in with_line.rows)[
            "hook: the denied set leaves an avc line"])

    def test_real_application(self) -> None:
        guest = self.guest()
        self.assertIn("'Real Game'", runner.check_real(guest, pathlib.Path("real.apk"), "org.example.real", "Real Game"))
        with self.assertRaises(Failure):
            runner.check_real(guest, pathlib.Path("real.apk"), "org.example.real", "Other")
        with self.assertRaises(Failure):
            runner.check_real(guest, pathlib.Path("real.apk"), "org.example.absent", "Real Game")
        # The application is uninstalled again, also after a failure.
        self.assertEqual(sum(1 for command in guest.commands if command.startswith("adb uninstall org.example.real")), 4)


if __name__ == "__main__":
    unittest.main()
