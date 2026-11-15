#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Old app compatibility runner without a guest.

run_android_oldapp_switch.py drives the image's `kernarium_oldapp` command and checks the property's policy
from an ordinary application. Here a scripted guest plays an image that behaves as the contract says, and
each way the image can differ makes one step fail: the probe packages (manifests, the stand-in library, the
probe's script), the parsers of the listing and of the probe's result, and every step against the scripted
guest.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import pathlib
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


class ScriptedGuest:
    """An image that follows the contract. Attributes switch one way of differing on."""

    def __init__(self) -> None:
        self.properties: dict[str, str] = {}
        self.clock = 1_700_000_000
        self.installed = {probe.package_of(variant) for variant in probe.VARIANTS}
        self.eligible = {probe.package_of("old"): ("Old Switch\tProbe", 1_700_000_000 - 30)}
        self.commands: list[str] = []
        # Ways to differ.
        self.list_extra: list[str] = []
        self.label: str | None = None
        self.refuse_with = 2
        self.app_can_set = False
        self.app_view_value: str | None = None
        self.domain = "u:r:untrusted_app_29:s0:c512,c768"
        self.shell_context = "u:r:shell:s0"
        self.path = runner.WRAPPER_PATH
        self.keep_on_off = True
        self.result_file: str | None = None
        self.version_line = runner.VERSION_LINE
        self.on_time_offset = 0
        self.real_label = "Real Game"

    def adb_run(self, *arguments: str, timeout: float = 30):
        self.commands.append("adb " + " ".join(arguments))
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
        if words[:3] == ["am", "broadcast", "-n"]:
            return runner.Run(0, "Broadcast completed: result=0\n", "")
        if words[:1] == ["run-as"]:
            if words[2] == "rm":
                return runner.Run(0, "", "")
            if self.result_file is not None:
                return runner.Run(0, self.result_file, "")
            name = probe.property_of("old")
            value = self.properties.get(name, "")
            after = "7" if self.app_can_set else value
            return runner.Run(0, f"context={self.domain}\nget={value if self.app_view_value is None else self.app_view_value}\n"
                              f"set={'allowed' if self.app_can_set else 'denied'}\nafter={after}\ndone\n", "")
        if words[:1] == [runner.COMMAND]:
            return self.command(words[1:])
        raise AssertionError(f"unscripted command: {command}")

    def command(self, arguments: list[str]) -> runner.Run:
        usage = runner.Run(64, "", "usage: kernarium_oldapp list\n       kernarium_oldapp set PACKAGE on|off\n")
        if arguments == ["list"]:
            lines = [self.version_line]
            for package, (label, updated) in sorted(self.eligible.items()):
                value = self.properties.get(probe.STORE_PROPERTY_PREFIX + package, "")
                shown = self.label if self.label is not None else label.replace("\t", " ")
                lines.append(f"{package}\t{shown}\t{'on' if value else 'off'}\t{value or 0}\t{updated}")
            lines += self.list_extra
            return runner.Run(0, "\n".join(lines) + "\n", "")
        if len(arguments) == 3 and arguments[0] == "set" and arguments[2] in ("on", "off"):
            package, mode = arguments[1], arguments[2]
            if package not in self.eligible:
                return runner.Run(self.refuse_with, "", f"kernarium_oldapp: {package} is not an application it applies to\n")
            name = probe.STORE_PROPERTY_PREFIX + package
            if mode == "on":
                self.properties[name] = str(self.clock + self.on_time_offset)
            elif self.keep_on_off:
                self.properties[name] = ""
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

    def test_manifests_read_back(self) -> None:
        for variant in probe.VARIANTS:
            tree = binary_xml.decode_manifest(binary_xml.encode_manifest(probe.manifest_tree(variant)))
            package, minimum, target, _, shared, receiver = probe.VARIANTS[variant]
            self.assertEqual(tree.attributes["package"], package)
            self.assertEqual(tree.attributes.get("sharedUserId"), shared)
            sdk = next(node for node in tree.children if node.tag == "uses-sdk")
            self.assertEqual(sdk.attributes, {"minSdkVersion": minimum, "targetSdkVersion": target})
            application = next(node for node in tree.children if node.tag == "application")
            self.assertEqual(application.attributes["label"],
                             probe.LABEL if variant == "old" else f"Switch Probe {variant}")
            self.assertEqual(application.attributes.get("debuggable") is True, receiver)
            self.assertEqual([node.attributes["name"] for node in application.children],
                             [probe.RECEIVER] if receiver else [])

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

    def test_signed_packages_hold_the_files_the_manifest_needs(self) -> None:
        try:
            with tempfile.TemporaryDirectory(prefix="uml-oldswitch-test-") as scratch:
                directory = pathlib.Path(scratch)
                names = {variant: zipfile.ZipFile(probe.build_apk(directory / f"{variant}.apk", variant)).namelist()
                         for variant in probe.VARIANTS}
        except FileNotFoundError:
            self.skipTest("keytool and jarsigner are not on PATH")
        self.assertIn(probe.LIBRARY, names["old"])
        self.assertIn(probe.LIBRARY, names["modern"])
        self.assertIn(probe.LIBRARY, names["shared"])
        self.assertNotIn(probe.LIBRARY, names["javaonly"])
        self.assertIn("classes.dex", names["modern"])
        self.assertNotIn("classes.dex", names["old"])


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


class Steps(unittest.TestCase):
    def guest(self) -> ScriptedGuest:
        return ScriptedGuest()

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
                guest.eligible[probe.package_of("old")] = ("Old Switch\tProbe", guest.clock + 1000)
            with self.assertRaises(Failure, msg=change):
                runner.check_listing(guest, installed_at)
        guest = self.guest()
        guest.eligible.clear()
        with self.assertRaises(Failure):
            runner.check_listing(guest, guest.clock - 60)

    def test_refusals(self) -> None:
        guest = self.guest()
        self.assertIn("status 64", runner.check_refusals(guest))
        guest = self.guest()
        guest.refuse_with = 1
        with self.assertRaises(Failure):
            runner.check_refusals(guest)
        # An image that lets a refused package through.
        guest = self.guest()
        guest.eligible[probe.package_of("modern")] = ("Modern", 1)
        with self.assertRaises(Failure):
            runner.check_refusals(guest)

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

    def test_real_application(self) -> None:
        guest = self.guest()
        guest.eligible["org.example.real"] = ("Real Game", 1)
        self.assertIn("'Real Game'", runner.check_real(guest, pathlib.Path("real.apk"), "org.example.real", "Real Game"))
        with self.assertRaises(Failure):
            runner.check_real(guest, pathlib.Path("real.apk"), "org.example.real", "Other")
        with self.assertRaises(Failure):
            runner.check_real(guest, pathlib.Path("real.apk"), "org.example.absent", "Real Game")
        # The application is uninstalled again, also after a failure.
        self.assertEqual(sum(1 for command in guest.commands if command.startswith("adb uninstall org.example.real")), 4)


if __name__ == "__main__":
    unittest.main()
