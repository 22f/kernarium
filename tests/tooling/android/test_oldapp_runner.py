#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The old-application runner without a guest: the probe's package, its code, the verdicts and the commands.

run_android_oldapp.py installs the probe application of oldapp_probe_apk.py in two variants and reads what each
one reports. Here the manifest and the DEX file of the package are read back with independent readers, the native
sources are built for armv7 when the host has the tools, and the runner's steps run against a fake guest that
records the commands it is given.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import importlib.util
import shutil
import struct
import sys
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path
from subprocess import CompletedProcess
from unittest import mock

TESTS = Path(__file__).resolve().parents[2]
SCRIPT = TESTS / "integration/android/run_android_oldapp.py"
SPEC = importlib.util.spec_from_file_location("run_android_oldapp", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
# Dataclasses resolve their annotations through sys.modules.
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)
probe = runner.probe
binary_xml = probe.binary_xml

ACC_STATIC, ACC_NATIVE, ACC_PUBLIC = 0x8, 0x100, 0x1


def uleb(data: bytes, offset: int) -> tuple[int, int]:
    value = shift = 0
    while True:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, offset
        shift += 7


class Dex:
    """A reader of the parts of a DEX file the probe uses, written from the format's description."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        header = struct.unpack_from("<8sI20sIIIIIIIIIIIIIIIIIIII", data, 0)
        (self.magic, self.checksum, self.signature, self.file_size, self.header_size, self.endian, _, _, self.map_off,
         self.string_count, self.string_off, self.type_count, self.type_off, self.proto_count, self.proto_off, _, _,
         self.method_count, self.method_off, self.class_count, self.class_off, self.data_size, self.data_off) = header
        self.strings = [self.string(i) for i in range(self.string_count)]
        self.types = [self.strings[struct.unpack_from("<I", data, self.type_off + 4 * i)[0]]
                      for i in range(self.type_count)]
        self.methods = []
        for index in range(self.method_count):
            owner, proto, name = struct.unpack_from("<HHI", data, self.method_off + 8 * index)
            shorty, return_type, parameters = struct.unpack_from("<III", data, self.proto_off + 12 * proto)
            self.methods.append((self.types[owner], self.strings[name], self.strings[shorty], self.types[return_type]))

    def string(self, index: int) -> str:
        (offset,) = struct.unpack_from("<I", self.data, self.string_off + 4 * index)
        _, offset = uleb(self.data, offset)
        return self.data[offset:self.data.index(b"\0", offset)].decode()

    def class_methods(self) -> dict[str, tuple[int, int]]:
        """name -> (access flags, code offset) over the one class's direct and virtual methods."""
        (_, _, _, _, _, _, class_data, _) = struct.unpack_from("<IIIIIIII", self.data, self.class_off)
        offset = class_data
        counts = []
        for _ in range(4):
            count, offset = uleb(self.data, offset)
            counts.append(count)
        found = {}
        for count in counts[2:]:
            index = 0
            for _ in range(count):
                step, offset = uleb(self.data, offset)
                access, offset = uleb(self.data, offset)
                code, offset = uleb(self.data, offset)
                index += step
                found[self.methods[index][1]] = (access, code)
        return found

    def instructions(self, code_offset: int) -> list[int]:
        registers, ins, outs, tries, debug, size = struct.unpack_from("<HHHHII", self.data, code_offset)
        return list(struct.unpack_from(f"<{size}H", self.data, code_offset + 16))


class ProbePackageTests(unittest.TestCase):
    def test_the_dex_file_is_well_formed(self) -> None:
        data = probe.oldapp_probe_dex.build_dex()
        dex = Dex(data)
        self.assertEqual(dex.magic, b"dex\n035\0")
        self.assertEqual(dex.file_size, len(data))
        self.assertEqual(dex.checksum, zlib.adler32(data[12:]) & 0xFFFFFFFF)
        self.assertEqual(dex.signature, hashlib.sha1(data[32:]).digest())
        self.assertEqual(dex.strings, sorted(dex.strings, key=lambda text: text.encode()))
        self.assertEqual(dex.class_count, 1)
        self.assertEqual(dex.types[struct.unpack_from("<I", data, dex.class_off)[0]],
                         "Lorg/example/andarium/oldappprobe/Probe;")

    def test_the_class_has_one_native_method_and_the_methods_the_native_code_calls(self) -> None:
        dex = Dex(probe.oldapp_probe_dex.build_dex())
        methods = dex.class_methods()
        self.assertEqual(set(methods), {"<init>", "onReceive", "run", "payload", "echo"})
        run_access, run_code = methods["run"]
        self.assertEqual(run_access, ACC_PUBLIC | ACC_STATIC | ACC_NATIVE)
        self.assertEqual(run_code, 0, "a native method has no code")
        for name in ("<init>", "onReceive", "payload", "echo"):
            self.assertNotEqual(methods[name][1], 0, name)
        self.assertEqual(methods["onReceive"][0], ACC_PUBLIC)
        self.assertEqual(methods["payload"][0], ACC_PUBLIC | ACC_STATIC)
        owner = "Lorg/example/andarium/oldappprobe/Probe;"
        listed = {(name, shorty, ret) for klass, name, shorty, ret in dex.methods if klass == owner}
        self.assertIn(("payload", "L", "[B"), listed)
        self.assertIn(("echo", "IL", "I"), listed)
        self.assertIn(("run", "LL", "Ljava/lang/String;"), listed)

    def test_echo_returns_the_length_and_payload_returns_the_bytes_of_the_string(self) -> None:
        dex = Dex(probe.oldapp_probe_dex.build_dex())
        methods = dex.class_methods()
        # array-length v0, v1; return v0
        self.assertEqual(dex.instructions(methods["echo"][1]), [0x1021, 0x000F])
        payload = dex.instructions(methods["payload"][1])
        self.assertEqual(payload[-1], 0x0011, "return-object v0")
        self.assertIn(probe.oldapp_probe_dex.PAYLOAD, dex.strings)

    def test_the_payload_matches_the_native_probe(self) -> None:
        source = probe.PROBE_SOURCE.read_text()
        self.assertIn(f'kPayload[] = "{probe.oldapp_probe_dex.PAYLOAD}"', source)
        self.assertIn("Java_org_example_andarium_oldappprobe_Probe_run", source)

    def test_the_variants_differ_in_package_and_target_level_only(self) -> None:
        old = binary_xml.decode_manifest(binary_xml.encode_manifest(probe.manifest_tree("old")))
        modern = binary_xml.decode_manifest(binary_xml.encode_manifest(probe.manifest_tree("modern")))
        self.assertEqual(old.attributes["package"], "org.example.andarium.oldappprobe")
        self.assertEqual(modern.attributes["package"], "org.example.andarium.oldappprobe.modern")
        old_sdk, modern_sdk = old.children[0], modern.children[0]
        self.assertEqual(old_sdk.attributes["targetSdkVersion"], 10)
        self.assertLess(old_sdk.attributes["targetSdkVersion"], 14)
        self.assertEqual(modern_sdk.attributes["targetSdkVersion"], 29)
        self.assertGreaterEqual(modern_sdk.attributes["targetSdkVersion"], 14)
        for tree in (old, modern):
            application = tree.children[1]
            self.assertNotIn("debuggable", application.attributes, "ART runs a debuggable application with CheckJNI")
            receiver = application.children[0]
            self.assertEqual(receiver.attributes["name"], probe.RECEIVER)
            self.assertIs(receiver.attributes["exported"], True)
            self.assertEqual(receiver.children, [], "no intent filter: only an explicit component starts it")

    def test_the_package_holds_the_code_and_the_32_bit_libraries(self) -> None:
        if shutil.which("jarsigner") is None or shutil.which("keytool") is None:
            self.skipTest("no JDK to sign the package")
        with tempfile.TemporaryDirectory(prefix="oldapp-test-") as scratch_name:
            scratch = Path(scratch_name)
            lib = scratch / "libkoldprobe.so"
            lib.write_bytes(b"\x7fELFprobe")
            shim = scratch / "libkernarium_oldapp.so"
            shim.write_bytes(b"\x7fELFshim")
            plain = probe.build_apk(scratch / "plain.apk", "old", lib)
            bundled = probe.build_apk(scratch / "bundled.apk", "modern", lib, shim)
            with zipfile.ZipFile(plain) as package:
                names = set(package.namelist())
            self.assertIn("classes.dex", names)
            self.assertIn("lib/armeabi-v7a/libkoldprobe.so", names)
            self.assertNotIn("lib/armeabi-v7a/libkernarium_oldapp.so", names)
            self.assertTrue(any(name.startswith("META-INF/") and name.endswith(".RSA") for name in names))
            with zipfile.ZipFile(bundled) as package:
                self.assertEqual(package.read("lib/armeabi-v7a/libkernarium_oldapp.so"), b"\x7fELFshim")

    def test_the_native_sources_build_freestanding_for_armv7(self) -> None:
        clang, lld = shutil.which("clang++"), shutil.which("ld.lld")
        if clang is None or lld is None:
            self.skipTest("no clang++ or ld.lld on PATH")
        with tempfile.TemporaryDirectory(prefix="oldapp-test-") as scratch_name:
            scratch = Path(scratch_name)
            library = probe.compile_library((probe.PROBE_SOURCE,), scratch / "libkoldprobe.so", "libkoldprobe.so",
                                            clang=clang, lld=lld, needed=probe.PROBE_NEEDED)
            shim = probe.compile_library(probe.SHIM_SOURCES, scratch / "libkernarium_oldapp.so",
                                         "libkernarium_oldapp.so", clang=clang, lld=lld, needed=probe.SHIM_NEEDED)
            for path, names, needed in ((library, (b"Java_org_example_andarium_oldappprobe_Probe_run", b"JNI_OnLoad"),
                                         probe.PROBE_NEEDED),
                                        (shim, (b"JNI_OnLoad",), probe.SHIM_NEEDED)):
                data = path.read_bytes()
                self.assertEqual(data[:4], b"\x7fELF")
                self.assertEqual(data[4], 1, "ELF32")
                self.assertEqual(struct.unpack_from("<HH", data, 16), (3, 40), "a shared object for ARM")
                for name in names:
                    self.assertIn(name, data)
                # bionic resolves an undefined symbol only in the libraries the object names.
                for name in needed:
                    self.assertIn(name.encode() + b"\0", data)


class VerdictTests(unittest.TestCase):
    SEMANTICS = {"write_through": "pass", "identity": "pass", "retained": "pass", "monitor": "pass", "pop": "pass",
                 "getline": "pass", "usable_size": "pass", "libc_block": "pass", "table_read_only": "pass",
                 "java_fill": "pass", "commit_chunks": "pass", "clone": "pass"}
    OLD = {"shim_mapped": "yes", "raw_read": "pass", "passback": "pass", "elements": "pass", "global": "pass",
           "thread_raw": "pass", **SEMANTICS, "java_fill_raw": "pass", "raw_write_java": "pass",
           "low_heap": "pass", "done": "1"}
    MODERN = {"shim_mapped": "no", "raw_read": "fail", "passback": "pass", "elements": "pass", "global": "pass",
              "thread_raw": "fail", **SEMANTICS, "java_fill_raw": "fail", "raw_write_java": "skipped",
              "low_heap": "pass", "done": "1"}

    def test_a_probe_that_reports_what_the_variant_must_passes(self) -> None:
        self.assertIn("raw_read=pass", runner.evaluate("old", self.OLD))
        self.assertIn("shim_mapped=no", runner.evaluate("modern", self.MODERN))

    def test_the_heap_of_a_modern_application_is_recorded_not_judged(self) -> None:
        runner.evaluate("modern", {**self.MODERN, "low_heap": "fail"})

    def test_every_difference_is_named(self) -> None:
        for key, value in (("shim_mapped", "no"), ("raw_read", "fail"), ("passback", "fail"), ("elements", "fail"),
                           ("global", "fail"), ("thread_raw", "fail"), ("low_heap", "fail"),
                           ("write_through", "fail"), ("identity", "fail"), ("retained", "fail"), ("monitor", "fail"),
                           ("pop", "fail"), ("getline", "fail"), ("usable_size", "fail"), ("libc_block", "fail"),
                           ("table_read_only", "fail"), ("java_fill", "fail"), ("java_fill_raw", "fail"), ("raw_write_java", "fail"),
                           ("commit_chunks", "fail"), ("clone", "fail"),
                           ("done", "")):
            with self.assertRaises(runner.Failure) as raised:
                runner.evaluate("old", {**self.OLD, key: value})
            self.assertIn(f"{key}={value}", str(raised.exception))

    def test_a_missing_last_line_fails_whatever_else_passed(self) -> None:
        partial = {key: value for key, value in self.OLD.items() if key != "done"}
        with self.assertRaises(runner.Failure) as raised:
            runner.evaluate("old", partial)
        self.assertIn("done=<missing>", str(raised.exception))

    def test_the_layer_in_a_modern_process_fails(self) -> None:
        with self.assertRaises(runner.Failure):
            runner.evaluate("modern", {**self.MODERN, "shim_mapped": "yes"})
        with self.assertRaises(runner.Failure):
            runner.evaluate("modern", {**self.MODERN, "raw_read": "pass"})

    def test_what_the_runtime_does_with_an_array_holds_with_and_without_the_layer(self) -> None:
        for variant, result in (("modern", self.MODERN), ("control", {**self.MODERN, "thread_raw": "fail"})):
            for key in self.SEMANTICS:
                with self.assertRaises(runner.Failure, msg=f"{variant} {key}"):
                    runner.evaluate(variant, {**result, key: "fail"})

    def test_a_probe_that_did_not_finish_carries_the_guests_log(self) -> None:
        with self.assertRaises(runner.Failure) as raised:
            runner.evaluate("old", {"log": "FATAL EXCEPTION: cannot locate symbol open"})
        self.assertIn("the log: FATAL EXCEPTION: cannot locate symbol open", str(raised.exception))

    def test_the_result_file_and_the_log_are_read(self) -> None:
        self.assertEqual(runner.parse_result("a=1\n\nb=\njunk\nc=x=y\n"), {"a": "1", "b": "", "c": "x=y"})
        self.assertEqual(runner.parse_result("a=1;b=pass;done=1;"), {"a": "1", "b": "pass", "done": "1"})
        self.assertEqual(runner.broadcast_data('Broadcast completed: result=0, data="x=1;y=2;"\n'), "x=1;y=2;")
        self.assertEqual(runner.broadcast_data("Broadcast completed: result=0\n"), "")
        log = ("11-15 kernarium_oldapp: region below 2 GiB at 0x30000000\n"
               "11-15 kernarium_oldapp: installed: jni=1 arena=1\n"
               "11-15 other: installed: nothing\n")
        self.assertEqual(runner.installs_in_log(log), 1)


class FakeGuest:
    """Records the commands; answers a result file that the test sets."""

    def __init__(self, result: str, installs: list[str] | None = None) -> None:
        self.commands: list[str] = []
        self.result = result
        self.logs = installs or ["", ""]
        self.log_reads = 0

    def adb_run(self, *arguments: str, timeout: float = 30) -> CompletedProcess:
        self.commands.append(" ".join(arguments))
        if arguments[0] == "logcat":
            text = self.logs[min(self.log_reads, len(self.logs) - 1)]
            self.log_reads += 1
            return CompletedProcess(arguments, 0, text)
        if arguments[0] == "install":
            return CompletedProcess(arguments, 0, "Performing Streamed Install\nSuccess\n")
        if "cat " in arguments[-1]:
            return CompletedProcess(arguments, 0, self.result)
        return CompletedProcess(arguments, 0, "")

    def shell(self, command: str, timeout: float = 30) -> str:
        self.commands.append("shell " + command)
        data = f', data="{self.result}"' if self.result else ""
        return f"Broadcast completed: result=0{data}\n"


class StepTests(unittest.TestCase):
    def test_an_application_is_installed_past_the_block_on_old_targets(self) -> None:
        guest = FakeGuest("")
        runner.install(guest, Path("/tmp/old.apk"), "old")
        self.assertEqual(guest.commands, ["uninstall org.example.andarium.oldappprobe",
                                          "install -r --bypass-low-target-sdk-block /tmp/old.apk"])

    def test_a_failed_install_names_the_package(self) -> None:
        class Refusing(FakeGuest):
            def adb_run(self, *arguments: str, timeout: float = 30) -> CompletedProcess:
                return CompletedProcess(arguments, 1, "Failure [INSTALL_FAILED_DEPRECATED_SDK_VERSION]")

        with self.assertRaises(runner.Failure) as raised:
            runner.install(Refusing(""), Path("/tmp/old.apk"), "old")
        self.assertIn("org.example.andarium.oldappprobe", str(raised.exception))

    def test_the_broadcast_names_the_component_and_the_extras(self) -> None:
        guest = FakeGuest("shim_mapped=yes;done=1;", ["", "k kernarium_oldapp: installed: jni=1 arena=1\n"])
        result, installs = runner.run_variant(guest, "old", shim=True)
        self.assertEqual(result, {"shim_mapped": "yes", "done": "1"})
        self.assertEqual(installs, 1)
        broadcasts = [c for c in guest.commands if "am broadcast" in c]
        self.assertEqual(len(broadcasts), 1)
        self.assertIn(f"-n {probe.component_of('old')}", broadcasts[0])
        self.assertIn("--es shim kernarium_oldapp", broadcasts[0])
        self.assertNotIn("--es opts", broadcasts[0])
        stop = next(i for i, c in enumerate(guest.commands) if "am force-stop" in c)
        self.assertLess(stop, next(i for i, c in enumerate(guest.commands) if "am broadcast" in c),
                        "a process of an earlier run is gone before the new one starts")

    def test_the_review_buttons_centre_is_found_in_a_dump(self) -> None:
        dump = ("<?xml version='1.0' encoding='UTF-8' standalone='yes' ?><hierarchy rotation=\"0\">"
                "<node index=\"0\" text=\"Cancel\" resource-id=\"com.android.permissioncontroller:id/cancel_button\" "
                "bounds=\"[500,1144][600,1216]\" />"
                "<node index=\"1\" text=\"Continue\" resource-id=\"com.android.permissioncontroller:id/continue_button\" "
                "bounds=\"[609,1144][720,1216]\" /></hierarchy>")
        self.assertEqual(runner.continue_button(dump), (664, 1180))
        self.assertIsNone(runner.continue_button("<hierarchy/>"))
        self.assertIsNone(runner.continue_button("not xml"))

    def test_a_review_is_completed_by_pressing_continue(self) -> None:
        flagged = "REVIEW_REQUIRED|USER_SENSITIVE_WHEN_GRANTED"
        clear = "USER_SET|USER_SENSITIVE_WHEN_GRANTED"
        button = ("<hierarchy><node resource-id=\"com.android.permissioncontroller:id/continue_button\" "
                  "bounds=\"[0,100][200,200]\" /></hierarchy>")

        class Reviewing(FakeGuest):
            pending = True

            def adb_run(self, *arguments: str, timeout: float = 30) -> CompletedProcess:
                self.commands.append(" ".join(arguments))
                if "cat /data/local/tmp/oldapp-ui.xml" in arguments[-1]:
                    return CompletedProcess(arguments, 0, button)
                return CompletedProcess(arguments, 0, "")

            def shell(self, command: str, timeout: float = 30) -> str:
                self.commands.append("shell " + command)
                if command.startswith("dumpsys package"):
                    return flagged if self.pending else clear
                if command.startswith("input tap"):
                    self.pending = False
                return ""

        guest = Reviewing("")
        with mock.patch.object(runner.time, "sleep"):
            self.assertEqual(runner.complete_review(guest, "org.example.andarium.oldappprobe"),
                             "permission review completed")
        self.assertIn("shell input tap 100 150", guest.commands)
        self.assertTrue(any("android.intent.action.REVIEW_PERMISSIONS" in c for c in guest.commands))
        with mock.patch.object(runner.time, "sleep"):
            self.assertEqual(runner.complete_review(guest, "org.example.andarium.oldappprobe"), "no review pending")

    def test_the_library_in_the_image_carries_the_label_an_application_may_execute(self) -> None:
        class Listing(FakeGuest):
            text = ""

            def shell(self, command: str, timeout: float = 30) -> str:
                self.commands.append("shell " + command)
                return self.text

        guest = Listing("")
        guest.text = f"-rw-r--r-- 1 root root {probe.SHIM_IMAGE_LABEL} 95816 2026-11-15 {probe.SHIM_IMAGE_PATH}\n"
        self.assertIn(probe.SHIM_IMAGE_LABEL, runner.layer_in_image(guest))
        guest.text = f"-rw-r--r-- 1 root root u:object_r:system_file:s0 95816 2026-11-15 {probe.SHIM_IMAGE_PATH}\n"
        with self.assertRaises(runner.Failure) as raised:
            runner.layer_in_image(guest)
        self.assertIn(probe.SHIM_IMAGE_LABEL, str(raised.exception))

    def test_an_application_is_switched_on_and_off_and_the_guest_must_hold_it(self) -> None:
        class Properties(FakeGuest):
            def __init__(self) -> None:
                super().__init__("")
                self.value = ""

            def shell(self, command: str, timeout: float = 30) -> str:
                self.commands.append("shell " + command)
                if command.startswith("su 0 setprop"):
                    self.value = command.split("'")[1]
                    return ""
                return self.value + "\n"

        guest = Properties()
        package = probe.package_of("old")
        name = f"persist.sys.kernarium.oldapp.{package}"
        self.assertIn("switched on", runner.switch(guest, package, True))
        self.assertTrue(guest.value.isdigit(), "the value is the turn-on time in epoch seconds")
        self.assertTrue(any(c.startswith(f"shell su 0 setprop {name} '") for c in guest.commands))
        self.assertIn("switched off", runner.switch(guest, package, False))
        self.assertEqual(guest.value, "")

        class Refusing(Properties):
            def shell(self, command: str, timeout: float = 30) -> str:
                self.commands.append("shell " + command)
                return ""

        with self.assertRaises(runner.Failure):
            runner.switch(Refusing(), package, True)

    def test_a_modern_run_asks_for_nothing_extra(self) -> None:
        guest = FakeGuest("done=1;")
        runner.run_variant(guest, "modern", shim=False)
        broadcast = next(c for c in guest.commands if "am broadcast" in c)
        self.assertNotIn("--es", broadcast)
        self.assertIn(probe.package_of("modern"), broadcast)

    def test_the_parser_defaults_to_the_orientation_runners_port(self) -> None:
        options = runner.build_parser().parse_args([])
        self.assertEqual(options.adb_port, runner.orient.slot_ports.slot_port(
            runner.orient.slot_ports.ADB_OFFSET, runner.orient.DEFAULT_ADB_PORT))
        self.assertFalse(options.host_shim)


if __name__ == "__main__":
    unittest.main()
