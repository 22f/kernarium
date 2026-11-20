#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Andarium/scripts/check-release-archive.py holds a Release archive to what any archive needs.

Each case builds a fixture application bundle, changes one thing and runs the
script on it. The Release archive's build phase runs the script; Debug builds,
the Preview and an ordinary Release build do not, so a fixture with the example
handle stays usable everywhere else. The rows for the executable and the
frameworks link small libraries with the iPhone SDK's compiler: what the build
host leaves in them (a linker debug map, a home, volume or temporary path), a
guest section that carries such paths on purpose, the test bundles and
frameworks that run tests, and a linked test-support library.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[3] / "Andarium" / "scripts" / "check-release-archive.py"
HANDLE_KEY = "AndariumFollowXHandle"


def run(app: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(SCRIPT), str(app)], check=False, stdin=subprocess.DEVNULL,
        capture_output=True, text=True, timeout=120)


class Fixtures(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.count = 0

    def app(self, **info: object) -> Path:
        """A bundle with an Info.plist of `info`; a value of None leaves the key out."""
        self.count += 1
        app = self.root / f"fixture-{self.count}" / "Andarium.app"
        app.mkdir(parents=True)
        keys = {"CFBundleIdentifier": "org.example.Fixture", **info}
        (app / "Info.plist").write_bytes(plistlib.dumps({k: v for k, v in keys.items() if v is not None}))
        return app

    def passes(self, app: Path) -> None:
        done = run(app)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def refuses(self, app: Path, expect: str) -> None:
        done = run(app)
        self.assertNotEqual(done.returncode, 0, done.stdout)
        self.assertIn(expect, done.stderr)
        self.assertNotIn("App Store", done.stdout + done.stderr)


class FollowXHandle(Fixtures):
    """The profile link the row opens is the developer's own, or there is none."""

    def test_a_bundle_with_no_handle_has_no_link(self) -> None:
        self.passes(self.app())

    def test_an_empty_handle_has_no_link(self) -> None:
        self.passes(self.app(**{HANDLE_KEY: ""}))

    def test_a_well_formed_handle_passes(self) -> None:
        for handle in ("real_handle", "a", "A1_b2", "abcdefghijklmno", "_underscore_"):
            with self.subTest(handle=handle):
                self.passes(self.app(**{HANDLE_KEY: handle}))

    def test_a_malformed_handle_is_refused(self) -> None:
        for handle in ("abcdefghijklmnop", "with.dot", "with space", "-dash", "x/y", "@name", "héllo", " ", "a\nb"):
            with self.subTest(handle=handle):
                self.refuses(self.app(**{HANDLE_KEY: handle}), HANDLE_KEY)

    def test_a_handle_that_is_not_a_string_is_refused(self) -> None:
        for value in (7, True, ["real_handle"]):
            with self.subTest(value=value):
                self.refuses(self.app(**{HANDLE_KEY: value}), HANDLE_KEY)

    def test_the_example_handle_is_refused_in_every_case(self) -> None:
        # What Developer.local.xcconfig.example names: copied unchanged, it would ship a link to it.
        for handle in ("example_handle", "EXAMPLE_HANDLE", "Example_Handle"):
            with self.subTest(handle=handle):
                self.refuses(self.app(**{HANDLE_KEY: handle}), "example")

    def test_a_bundle_without_an_info_plist_is_refused(self) -> None:
        app = self.root / "bare" / "Andarium.app"
        app.mkdir(parents=True)
        self.refuses(app, "Info.plist")


# Libraries of one thing each, linked for the iPhone SDK. A path in a string is
# what a dependency compiles in from the prefix it was built under.
CLEAN = "int fixture(int value) { return value + 1; }\n"
HOME_PATH = 'const char *fixture(void) { return "/Users/NAME/work/project/file.c"; }\n'
VOLUME_PATH = 'const char *fixture(void) { return "/Volumes/NAME/prefix/share/locale"; }\n'
VOLUME_NAME_WITH_SPACE = 'const char *fixture(void) { return "/Volumes/NAME 2/prefix/lib"; }\n'
TEMPORARY_PATH = (
    "const char *fixture(void) {"
    ' return "/private" "/var/folders/NAME/T/source-build.abc123/stage"; }\n'
)
# Strings that are no path of a build: the volumes directory alone, a format for
# one, a different directory of a temporary tree, and the neutral directory the
# dependency builds compile in.
HARMLESS_PATHS = (
    "const char *const fixture[] = {\n"
    '  "/Volumes/", "/Volumes/%s", "/var/folders/", "/tmp/mesa_%s",\n'
    '  "/nonexistent/share/locale",\n'
    "};\n"
)
# A symbol of the test-support library, as Swift mangles a type of its module.
TEST_SUPPORT_SYMBOL = "int _s23AndariumCoreTestSupport13GamepadScriptV7controllerSayAA0A10ControllerVGvgZ(void) { return 1; }\n"
SDK = ["xcrun", "--sdk", "iphoneos", "clang", "-target", "arm64-apple-ios27.0"]


def tool(command: list[str], cwd: Path | None = None) -> None:
    done = subprocess.run(command, cwd=cwd, check=False, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, timeout=120)
    if done.returncode != 0:
        raise AssertionError(f"{' '.join(command[:4])} failed:\n{done.stdout}")


class Libraries(Fixtures):
    """Application bundles around compiled libraries: the executable, and a framework named Runtime."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.libraries = Path(tempfile.mkdtemp(prefix="andarium-archive-"))
        cls.addClassCleanup(shutil.rmtree, cls.libraries, True)

    @classmethod
    def build(cls, name: str, source: str, *extra: str) -> Path:
        output = cls.libraries / f"lib{name}.dylib"
        if not output.exists():
            (cls.libraries / f"{name}.c").write_text(source, encoding="utf-8")
            tool([*SDK, "-dynamiclib", str(cls.libraries / f"{name}.c"), "-o", str(output),
                  f"-Wl,-install_name,@rpath/{output.name}", *extra])
        return output

    @classmethod
    def build_with_debug_map(cls, name: str, directory: str | None = None) -> Path:
        """A library linked from an object compiled with -g: the linker keeps a
        debug map whose N_OSO entry names the object file, which sits in a
        directory that looks like a mounted volume. `directory` is the
        compilation directory the map's N_SO entries name."""
        output = cls.libraries / f"lib{name}.dylib"
        if not output.exists():
            work = cls.libraries / "Volumes" / "NAME"
            work.mkdir(parents=True, exist_ok=True)
            (work / f"{name}.c").write_text(CLEAN, encoding="utf-8")
            tool([*SDK, "-g", *([f"-fdebug-compilation-dir={directory}"] if directory else []),
                  "-c", f"{name}.c" if directory else str(work / f"{name}.c"), "-o", str(work / f"{name}.o")],
                 cwd=work)
            tool([*SDK, "-dynamiclib", str(work / f"{name}.o"), "-o", str(output),
                  f"-Wl,-install_name,@rpath/{output.name}"])
            assert b"/Volumes/NAME/" in output.read_bytes(), "the debug map names no object under a volume"
        return output

    @classmethod
    def strip_debug_map(cls, library: Path) -> Path:
        """The same library after `strip -S`, as the packaging leaves it."""
        stripped = library.with_name(f"{library.stem}-stripped.dylib")
        if not stripped.exists():
            shutil.copyfile(library, stripped)
            tool(["xcrun", "--sdk", "iphoneos", "strip", "-S", str(stripped)])
        return stripped

    def bundle(self, executable: Path, framework: Path | None = None) -> Path:
        """An application whose executable is `executable` and which embeds `framework` as Runtime."""
        self.count += 1
        app = self.root / f"bundle-{self.count}" / "Andarium.app"
        app.mkdir(parents=True)
        shutil.copyfile(executable, app / "Andarium")
        (app / "Info.plist").write_bytes(plistlib.dumps(
            {"CFBundleIdentifier": "org.example.Fixture", "CFBundleExecutable": "Andarium"}))
        if framework is not None:
            runtime = app / "Frameworks" / "Runtime.framework"
            runtime.mkdir(parents=True)
            shutil.copyfile(framework, runtime / "Runtime")
            (runtime / "Info.plist").write_bytes(plistlib.dumps(
                {"CFBundleIdentifier": "org.example.Runtime", "CFBundleExecutable": "Runtime"}))
        return app


class HostTraces(Libraries):
    """A build leaves nothing of the machine that built it in the host code."""

    def test_clean_code_passes_as_the_executable_and_as_a_framework(self) -> None:
        clean = self.build("clean", CLEAN)
        self.passes(self.bundle(clean, framework=clean))

    def test_a_guest_section_may_carry_the_build_hosts_paths(self) -> None:
        # The kernel image rides in a section of its own, built elsewhere.
        guest = self.libraries / "guest.bin"
        guest.write_bytes(b"\0UML_ROOTFS\0/Users/NAME/built/here.c\0/Volumes/NAME/built/here.c\0")
        carrier = self.build("carrier", CLEAN, "-Wl,-sectcreate,__TEXT,__uml_elf," + str(guest))
        self.assertIn(b"/Users/NAME/built", carrier.read_bytes())
        self.passes(self.bundle(self.build("clean", CLEAN), framework=carrier))

    def test_a_home_directory_path_is_refused(self) -> None:
        homed, clean = self.build("homed", HOME_PATH), self.build("clean", CLEAN)
        self.refuses(self.bundle(homed), "/Users/NAME/work/project/file.c")
        self.refuses(self.bundle(clean, framework=homed),
                     "Runtime.framework/Runtime: names a path under a home directory")

    def test_a_volume_path_is_refused(self) -> None:
        volumed, spaced = self.build("volumed", VOLUME_PATH), self.build("spaced", VOLUME_NAME_WITH_SPACE)
        clean = self.build("clean", CLEAN)
        self.refuses(self.bundle(volumed), "/Volumes/NAME/prefix/share/locale")
        self.refuses(self.bundle(clean, framework=volumed),
                     "Runtime.framework/Runtime: names a path on a mounted volume")
        self.refuses(self.bundle(clean, framework=spaced), "/Volumes/NAME 2/prefix/lib")

    def test_a_temporary_directory_path_is_refused(self) -> None:
        temporary, clean = self.build("temporary", TEMPORARY_PATH), self.build("clean", CLEAN)
        self.refuses(self.bundle(temporary), "names a path in a temporary build directory")
        self.refuses(self.bundle(clean, framework=temporary), "/var/folders/NAME/T/source-build.abc123/stage")

    def test_strings_that_are_no_path_of_a_build_pass(self) -> None:
        harmless = self.build("harmless", HARMLESS_PATHS)
        self.passes(self.bundle(harmless, framework=harmless))

    def test_a_framework_with_a_debug_map_is_refused_and_the_executable_is_not(self) -> None:
        # Xcode strips the application's own executable when it archives, so
        # only an embedded framework's map is the check's concern.
        mapped, clean = self.build_with_debug_map("mapped"), self.build("clean", CLEAN)
        self.passes(self.bundle(mapped))
        self.refuses(self.bundle(clean, framework=mapped),
                     "Runtime.framework/Runtime: carries the linker's debug map")
        # The map is reported once, as the map: its object paths are no path of
        # the code.
        done = run(self.bundle(clean, framework=mapped))
        self.assertNotIn("mounted volume", done.stderr)

    def test_the_source_directory_of_a_debug_map_is_not_reported_as_a_path(self) -> None:
        mapped_home, clean = self.build_with_debug_map("mapped-home", "/Users/NAME/src"), self.build("clean", CLEAN)
        self.passes(self.bundle(mapped_home))
        app = self.bundle(clean, framework=mapped_home)
        self.refuses(app, "Runtime.framework/Runtime: carries the linker's debug map")
        self.assertNotIn("home directory", run(app).stderr)

    def test_a_framework_stripped_of_its_debug_map_passes(self) -> None:
        stripped = self.strip_debug_map(self.build_with_debug_map("mapped"))
        self.passes(self.bundle(stripped, framework=stripped))


class TestBundles(Libraries):
    """An application that tests hosted carries none of what ran them."""

    MEMBERS = (
        ("PlugIns/FixtureTests.xctest", True),
        ("Frameworks/XCTest.framework", True),
        ("Frameworks/XCUIAutomation.framework", True),
        ("Frameworks/Testing.framework", True),
        ("Frameworks/_Testing_Foundation.framework", True),
        ("Frameworks/AndariumCoreTestSupport.framework", True),
        ("Frameworks/libXCTestSwiftSupport.dylib", False),
        ("Frameworks/libAndariumCoreTestSupport.dylib", False),
    )

    def test_a_test_bundle_or_test_framework_is_refused(self) -> None:
        clean = self.build("clean", CLEAN)
        for relative, is_directory in self.MEMBERS:
            with self.subTest(relative):
                app = self.bundle(clean, framework=clean)
                member = app / relative
                if is_directory:
                    member.mkdir(parents=True)
                    (member / "Info.plist").write_bytes(plistlib.dumps({"CFBundleIdentifier": "org.example.Test"}))
                else:
                    shutil.copyfile(clean, member)
                self.refuses(app, f"{relative}: a test bundle or test framework")

    def test_the_applications_own_frameworks_are_not_test_support(self) -> None:
        clean = self.build("clean", CLEAN)
        app = self.bundle(clean, framework=clean)
        (app / "Frameworks" / "Support.framework").mkdir()
        (app / "PlugIns").mkdir()
        self.passes(app)


class LinkedTestSupport(Libraries):
    """No library that only tests link is linked into the application."""

    def test_a_linked_test_support_library_is_refused(self) -> None:
        linked, clean = self.build("linked", TEST_SUPPORT_SYMBOL), self.build("clean", CLEAN)
        self.refuses(self.bundle(linked), "Andarium: links AndariumCoreTestSupport")
        self.refuses(self.bundle(clean, framework=linked), "Runtime.framework/Runtime: links AndariumCoreTestSupport")


if __name__ == "__main__":
    unittest.main(verbosity=2)
