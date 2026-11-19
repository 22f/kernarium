#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher-application runner without a guest.

run_android_app_shortcuts.py drives the image's `kernarium_apps` command and checks what it answers. Here a
scripted guest plays an image that follows the contract (it reads the manifests of the probe packages the runner
installs, keeps a foreground activity and a marker file, and prints the command's answers), and each way the image
can differ makes the step that owns that rule fail: the probe packages (manifests read back), the strict parsers
(each deviation of a header, a row, a label, an icon record), and every step against the scripted guest. It
skips (77) where the host has no keytool and jarsigner (the packages are signed).
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import contextlib
import hashlib
import io
import pathlib
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
import zipfile
import zlib

HERE = pathlib.Path(__file__).resolve().parent
INTEGRATION = HERE.parents[1] / "integration" / "android"
sys.path.insert(0, str(INTEGRATION))
import appsprobe_apk as probe  # noqa: E402
import orientation_probe_apk as binary_xml  # noqa: E402
import run_android_app_shortcuts as runner  # noqa: E402

Failure = runner.Failure
SKIP = 77
SETTINGS = "com.android.settings"
CALCULATOR = "com.android.calculator2"


def make_png(size: int, opaque_corners: bool = True, alpha: bool = True) -> bytes:
    """A PNG of two colours (a square with a disc), RGBA or RGB."""
    channels = 4 if alpha else 3
    rows = bytearray()
    centre = size / 2
    for y in range(size):
        rows.append(0)
        for x in range(size):
            inside = (x - centre) ** 2 + (y - centre) ** 2 < (size / 3) ** 2
            colour = (255, 255, 255) if inside else (30, 144, 255)
            corner = (x < 3 or x >= size - 3) and (y < 3 or y >= size - 3)
            rows += bytes(colour) + (bytes([0 if (corner and not opaque_corners) else 255]) if alpha else b"")

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", size, size, 8, 6 if alpha else 2, 0, 0, 0)
    return runner.PNG_SIGNATURE + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(rows))) + chunk(b"IEND", b"")


class Package:
    def __init__(self, name: str, version: int, installed: int, builtin: bool, activities: list[dict]) -> None:
        self.name, self.version, self.installed, self.builtin = name, version, installed, builtin
        self.activities = activities
        self.enabled = True


class ScriptedImage:
    """An image that follows the contract. Attributes switch one way of differing on."""

    def __init__(self) -> None:
        self.clock = 1_700_000_000_000
        self.packages: dict[str, Package] = {}
        self.add(Package(SETTINGS, 100, 1, True, [
            {"name": "com.android.settings.Settings", "exported": True, "launcher": True, "label": "Settings",
             "icon": "adaptive"},
            {"name": "com.android.settings.SubSettings", "exported": False, "launcher": False, "label": "x",
             "icon": None}]))
        self.add(Package(CALCULATOR, 200, 1, True, [
            {"name": "com.android.calculator2.Calculator", "exported": True, "launcher": True,
             "label": "Calculator", "icon": "legacy"}]))
        self.resumed: str | None = None
        self.files: set[str] = set()
        self.denials: list[str] = []
        self.locales = "en-US"
        self.commands: list[str] = []
        # Ways to differ.
        self.deviation: str | None = None

    def add(self, package: Package) -> None:
        self.packages[package.name] = package

    # -- adb
    def adb_run(self, *arguments: str, timeout: float = 30) -> subprocess.CompletedProcess:
        if arguments[0] == "uninstall":
            self.packages.pop(arguments[1], None)
            return subprocess.CompletedProcess(arguments, 0, b"Success\n", b"")
        if arguments[0] == "install":
            apk = pathlib.Path(arguments[-1])
            with zipfile.ZipFile(apk) as archive:
                manifest = binary_xml.decode_manifest(archive.read("AndroidManifest.xml"))
            name = dict(manifest.attributes)["package"]
            version = dict(manifest.attributes)["versionCode"]
            old = self.packages.get(name)
            self.clock += 5_000
            activities = []
            for application in (child for child in manifest.children if child.tag == "application"):
                for activity in application.children:
                    attributes = dict(activity.attributes)
                    activities.append({
                        "name": attributes["name"], "exported": bool(attributes.get("exported")),
                        "launcher": any(child.tag == "intent-filter" for child in activity.children),
                        "label": attributes.get("label", "Apps Probe Application"), "icon": None})
            self.add(Package(name, version, old.installed if old else self.clock, False, activities))
            if old:
                self.packages[name].version = version
                self.packages[name].installed = self.clock  # lastUpdateTime moves
            return subprocess.CompletedProcess(arguments, 0, b"Performing Streamed Install\nSuccess\n", b"")
        raise AssertionError(arguments)

    def run(self, command: str, timeout: float = 30) -> "runner.Run":
        self.commands.append(command)
        words = shlex.split(command)
        out, status, err = self.dispatch(command, words)
        return runner.Run(status, out if isinstance(out, bytes) else out.encode(), err)

    def shell(self, command: str, timeout: float = 30) -> str:
        result = self.run(command, timeout)
        if result.status != 0:
            raise Failure(f"'{command}' exited {result.status}: {result.text.strip()[-300:]}")
        return result.text

    def dispatch(self, command: str, words: list[str]):
        if words[0] == "ls" and words[1] == "-lZ":
            return f"-rwxr-xr-x 1 root shell u:object_r:system_file:s0 1 {runner.WRAPPER_PATH}\n" \
                   f"-rw-r--r-- 1 root root u:object_r:system_file:s0 1 {runner.JAR_PATH}\n", 0, ""
        if words[:3] == ["command", "-v", runner.COMMAND]:
            return runner.WRAPPER_PATH + "\n", 0, ""
        if words == ["id", "-Z"]:
            return "u:r:shell:s0\n", 0, ""
        if words == ["am", "get-current-user"]:
            return "0\n", 0, ""
        if words[0] == "getprop":
            return ("\n" if words[1] == "persist.sys.locale" else "en-US\n"), 0, ""
        if words[0] == "dumpsys":
            line = f"  mResumedActivity: ActivityRecord{{abc u0 {self.resumed} t1}}\n" if self.resumed else ""
            if self.deviation == "launch-not-resumed":
                line = ""
            return line, 0, ""
        if words[:2] == ["input", "keyevent"]:
            return "", 0, ""
        if words[:2] == ["pm", "disable-user"]:
            self.packages[words[-1]].enabled = False
            return "", 0, ""
        if words[:2] == ["pm", "enable"]:
            self.packages[words[-1]].enabled = True
            return "", 0, ""
        if words[:2] == ["pm", "uninstall"]:
            self.packages.pop(words[-1], None)
            return "Success\n", 0, ""
        if words[:2] == ["rm", "-f"]:
            self.files.discard(words[2])
            return "", 0, ""
        if words[0] == "ls" and len(words) == 2:
            return "", (0 if words[1] in self.files else 1), ""
        if words[:2] == ["ls", "-A"]:
            return "".join(f"{name.rsplit('/', 1)[1]}\n" for name in sorted(self.files)), 0, ""
        if words[:3] == ["logcat", "-d", "-b"]:
            return "\n".join(self.denials) + "\n", 0, ""
        if words[0] == runner.COMMAND:
            return self.tool(command, words[1:])
        raise AssertionError(f"the scripted guest does not know {command!r}")

    # -- the command
    def rows(self) -> list[tuple[str, str, str, int, str]]:
        """The listing the command prints: launcher activities of the enabled packages, in order."""
        found = []
        for package in self.packages.values():
            if not package.enabled:
                continue
            for activity in package.activities:
                listed = activity["launcher"] and (activity["exported"] or self.deviation == "hidden-listed")
                if not listed:
                    continue
                label = activity["label"]
                label = {probe.HOSTILE_LABEL: probe.LISTED_HOSTILE_LABEL, probe.LONG_LABEL: probe.LISTED_LONG_LABEL}.get(label, label)
                if self.deviation == "label-raw" and activity["label"] == probe.HOSTILE_LABEL:
                    label = "Apps\tProbe"
                if self.deviation == "label-uncut" and activity["label"] == probe.LONG_LABEL:
                    label = probe.LONG_LABEL
                if self.deviation == "label-bidi" and activity["label"] == probe.HOSTILE_LABEL:
                    label = probe.LISTED_HOSTILE_LABEL + "‮"
                found.append((package.name, activity["name"], label, 1 if package.builtin else 0, self.key(package, activity)))
        found.sort(key=lambda row: (row[0], row[1]))
        if self.deviation == "unsorted":
            found.reverse()
        return found

    def key(self, package: Package, activity: dict) -> str:
        text = f"{package.name}\0{activity['name']}\0{'' if self.deviation == 'static-keys' else package.version}\0" \
               f"{'' if self.deviation == 'static-keys' else package.installed}\0{int(package.builtin)}"
        return hashlib.sha256(text.encode()).hexdigest()[:16]

    def digest(self, rows) -> str:
        if self.deviation == "digest-constant":
            return "0" * 64
        if self.deviation == "digest-short":
            return "0" * 63
        text = "".join(f"{row[0]}\t{row[1]}\t{row[3]}\t{row[4]}\n" for row in rows) + f"locales\t{self.locales}\nuser\t0\n"
        return hashlib.sha256(text.encode()).hexdigest()

    def header(self, rows, digest=None) -> str:
        return (f"kernarium_apps 1\ndigest\t{digest or self.digest(rows)}\ncount\t{len(rows)}\nskipped\t0\n"
                f"locales\t{self.locales}\nuser\t0\n")

    def tool(self, command: str, words: list[str]):
        if self.deviation == "writes-file" and words[:1] == ["list"]:
            self.files.add(f"{runner.SHELL_DATA}/kernarium_apps.cache")
        if self.deviation == "denial" and words[:1] == ["list"]:
            self.denials.append(f'11-19 11:00:{len(self.denials):02d}.000   500   500 W auditd : avc:  denied  {{ read }} '
                                'for comm="app_process64" scontext=u:r:shell:s0 tcontext=u:object_r:apk_data_file:s0 '
                                'tclass=file permissive=0')
        known = {"digest": 1, "list": 1}
        if not words or (words[0] in known and len(words) != 1) or words[0] not in ("digest", "list", "icons", "launch") \
                or (words[0] == "icons" and not 2 <= len(words) <= 9) or (words[0] == "launch" and len(words) != 2):
            return "", (2 if self.deviation == "usage-status" else 64), "usage: kernarium_apps ...\n"
        rows = self.rows()
        if words[0] == "digest":
            return self.header(rows), 0, ""
        if words[0] == "list":
            digest = self.digest(rows)
            header = self.header(rows, "f" * 64 if self.deviation == "header-differs" else digest)
            body = "rows\n" + "".join("\t".join(map(str, row)) + "\n" for row in rows)
            return header + body + ("" if self.deviation == "list-no-end" else f"end\t{len(rows)}\n"), 0, ""
        if words[0] == "icons":
            return self.icons(rows, words[1:]), 0, ""
        return self.launch(rows, words[1])

    def icons(self, rows, tokens: list[str]) -> bytes:
        if self.deviation == "icons-order":
            tokens = list(reversed(tokens))
        out = b"kernarium_apps 1\n" + f"icons\t{len(tokens)}\n".encode()
        for token in tokens:
            package, _, activity = token.partition("/")
            found = None
            if package in self.packages:
                for candidate in self.packages[package].activities:
                    if candidate["name"] == activity and candidate["launcher"] and candidate["exported"]:
                        found = candidate
            if found is None or not self.packages[package].enabled:
                out += f"icon\t{token}\tmissing\t0\t0\t-\t0\t0\t-\n".encode()
            elif found["icon"] is None:
                out += f"icon\t{token}\tnoicon\t0\t0\t-\t0\t0\t-\n".encode()
            else:
                alpha = self.deviation == "icon-corner"
                data = make_png(192, opaque_corners=not alpha)
                size = 128 if self.deviation == "icon-size" else 192
                sha = hashlib.sha256(data).hexdigest()
                if self.deviation == "icon-sha":
                    sha = "0" * 64
                out += (f"icon\t{token}\tok\t{size}\t{size}\t{found['icon']}\t0\t{len(data)}\t{sha}\n"
                        .encode() + data + b"\n")
        if self.deviation != "icons-no-end":
            out += f"end\t{len(tokens)}\n".encode()
        return out

    def launch(self, rows, token: str):
        package, _, activity = token.partition("/")
        marker = f"{runner.MARKER}"
        grammar_ok = runner.PACKAGE_NAME.fullmatch(package) and runner.ACTIVITY_NAME.fullmatch(activity) and "/" not in activity
        found = None
        if grammar_ok and package in self.packages and self.packages[package].enabled:
            for candidate in self.packages[package].activities:
                if candidate["name"] == activity and candidate["launcher"] and (candidate["exported"]
                                                                              or self.deviation == "launch-hidden"):
                    found = candidate
        if self.deviation == "injection" and not grammar_ok and "touch" in token:
            self.files.add(marker)
        if self.deviation == "disabled-launch" and package in self.packages and not self.packages[package].enabled:
            found = self.packages[package].activities[0]
        if found is None:
            err = "kernarium_apps: refused: not a launcher activity of the foreground user\n"
            if self.deviation == "refusal-multiline":
                err += "second line\n"
            return "", 2, err
        self.resumed = f"{package}/{activity}"
        return f"launched\t{token}\n", 0, ""


class Fixture(unittest.TestCase):
    apks: dict[str, pathlib.Path] = {}
    scratch: pathlib.Path

    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("keytool") is None or shutil.which("jarsigner") is None:
            raise unittest.SkipTest("no keytool and jarsigner")
        probe_ok = subprocess.run(["keytool", "-help"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, check=False).returncode
        if probe_ok not in (0, 1):
            raise unittest.SkipTest("keytool does not run")
        cls.scratch = pathlib.Path(tempfile.mkdtemp(prefix="uml-appshort-test-"))
        keystore = cls.scratch / "family.p12"
        for variant in (*probe.VARIANTS, *probe.SUCCESSORS):
            cls.apks[variant] = probe.build_apk(cls.scratch / f"{variant}.apk", variant, keystore=keystore)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.scratch, ignore_errors=True)

    def build(self, destination: pathlib.Path, variant: str, keystore=None) -> pathlib.Path:
        shutil.copy(self.apks[variant], destination)
        return destination


class ProbePackages(Fixture):
    def test_manifests_read_back(self) -> None:
        for variant in (*probe.VARIANTS, *probe.SUCCESSORS):
            with zipfile.ZipFile(self.apks[variant]) as archive:
                manifest = binary_xml.decode_manifest(archive.read("AndroidManifest.xml"))
                self.assertIn("META-INF/MANIFEST.MF", archive.namelist())
            attributes = dict(manifest.attributes)
            self.assertEqual(probe.package_of(variant), attributes["package"])
            self.assertEqual(2 if variant.endswith("-v2") else 1, attributes["versionCode"])
            application = next(child for child in manifest.children if child.tag == "application")
            self.assertEqual(len(probe.activity_names(variant)), len(application.children))
            for activity, expected in zip(application.children, probe.activity_names(variant)):
                values = dict(activity.attributes)
                self.assertEqual(probe.CLASSES[expected], values["name"])
                self.assertEqual(expected != "hidden", values["exported"])
                launcher = [child for child in activity.children if child.tag == "intent-filter"]
                self.assertEqual(0 if expected == "plain" else 1, len(launcher))
                if expected == "listed":
                    self.assertEqual(probe.HOSTILE_LABEL, values["label"])
                if expected == "long":
                    self.assertEqual(probe.LONG_LABEL, values["label"])

    def test_the_variants_of_a_package_are_one_package(self) -> None:
        self.assertEqual(probe.package_of("first"), probe.package_of("first-v2"))
        self.assertNotEqual(probe.package_of("first"), probe.package_of("second"))
        self.assertEqual(len(set(probe.CLASSES[name] for name in probe.activity_names("first"))), 4)

    def test_expected_labels_follow_the_rule(self) -> None:
        self.assertIsNone(runner.label_problem(probe.LISTED_HOSTILE_LABEL))
        self.assertIsNone(runner.label_problem(probe.LISTED_LONG_LABEL))
        self.assertEqual(80, len(probe.LISTED_LONG_LABEL))
        self.assertIsNotNone(runner.label_problem(probe.HOSTILE_LABEL))
        self.assertIsNotNone(runner.label_problem(probe.LONG_LABEL))


class Parsers(unittest.TestCase):
    DIGEST = "ab" * 32

    def header(self, **changes: str) -> bytes:
        fields = {"digest": self.DIGEST, "count": "1", "skipped": "0", "locales": "en-US", "user": "0"}
        fields.update(changes)
        return ("kernarium_apps 1\n" + "".join(f"{key}\t{value}\n" for key, value in fields.items())).encode()

    ROW = "com.a.app\tcom.a.app.Main\tA label\t0\t0123456789abcdef\n"

    def listing(self, header: bytes | None = None, rows: str | None = None, end: str = "end\t1\n") -> bytes:
        return (header or self.header()) + b"rows\n" + (self.ROW if rows is None else rows).encode() + end.encode()

    def test_accepts_the_contract(self) -> None:
        self.assertEqual(self.DIGEST, runner.parse_digest(self.header()).digest)
        listed = runner.parse_list(self.listing())
        self.assertEqual(["com.a.app/com.a.app.Main"], [row.token for row in listed.rows])
        extra = self.ROW.replace("\n", "\tfuture\n")
        self.assertEqual(1, len(runner.parse_list(self.listing(rows=extra)).rows))
        unknown = self.header().replace(b"user\t0\n", b"user\t0\nfuture\tx\n")
        # A header line this reader does not know is not skipped by this strict reader: the host must, but the runner
        # asserts the version-1 block exactly.
        with self.assertRaises(Failure):
            runner.parse_digest(unknown)

    def test_rejects_each_deviation(self) -> None:
        bad_headers = [
            self.header(digest="AB" * 32), self.header(digest="ab" * 31), self.header(count="x"),
            self.header(locales="en US"), self.header(locales="a,b,c,d,e,f"), self.header(user="-1"),
            b"kernarium_apps 2\n" + self.header()[17:], self.header().replace(b"\n", b"\r\n"),
            self.header().rstrip(b"\n"),
        ]
        for data in bad_headers:
            with self.assertRaises(Failure, msg=data):
                runner.parse_digest(data)
        for rows, end in [
            (self.ROW, "end\t2\n"), (self.ROW, ""), (self.ROW, "end\t1\nextra\n"), ("", "end\t1\n"),
            (self.ROW.replace("\t0\t0123", "\t2\t0123"), "end\t1\n"), (self.ROW.replace("0123456789abcdef", "XYZ"), "end\t1\n"),
            ("com.a\n", "end\t1\n"),
            (self.ROW.replace("A label", "A‮label"), "end\t1\n"), (self.ROW.replace("A label", "A  label"), "end\t1\n"),
            (self.ROW.replace("A label", " A label"), "end\t1\n"), (self.ROW.replace("A label", ""), "end\t1\n"),
            (self.ROW.replace("A label", "x" * 81), "end\t1\n"), (self.ROW.replace("A label", "A​label"), "end\t1\n"),
            (self.ROW.replace("com.a.app\t", "bad name\t"), "end\t1\n"), (self.ROW.replace("com.a.app.Main", "-x"), "end\t1\n"),
        ]:
            with self.assertRaises(Failure, msg=(rows, end)):
                runner.parse_list(self.listing(rows=rows, end=end))
        two = "com.b.app\tcom.b.app.Main\tB\t0\t0123456789abcdef\n"
        with self.assertRaises(Failure):
            runner.parse_list(self.listing(header=self.header(count="2"), rows=two + self.ROW, end="end\t2\n"))
        with self.assertRaises(Failure):
            runner.parse_list(self.listing(header=self.header(count="2"), rows=self.ROW + self.ROW, end="end\t2\n"))

    def record(self, data: bytes, **changes: str) -> bytes:
        fields = {"status": "ok", "width": "192", "height": "192", "kind": "adaptive", "alpha": "0",
                  "length": str(len(data)), "sha": hashlib.sha256(data).hexdigest()}
        fields.update(changes)
        return (f"icon\ta.b/C\t{fields['status']}\t{fields['width']}\t{fields['height']}\t{fields['kind']}\t"
                f"{fields['alpha']}\t{fields['length']}\t{fields['sha']}\n").encode() + data + b"\n"

    def test_icons(self) -> None:
        data = make_png(192)
        head = b"kernarium_apps 1\nicons\t1\n"
        good = head + self.record(data) + b"end\t1\n"
        records = runner.parse_icons(good, ["a.b/C"])
        self.assertEqual("ok", records[0].status)
        self.assertEqual("decoded", runner.check_picture(records[0]))
        for broken in [
            good[:-6], good + b"x", good.replace(b"end\t1", b"end\t2"), head + self.record(data, sha="0" * 64) + b"end\t1\n",
            head + self.record(data, width="128", height="128") + b"end\t1\n",
            head + self.record(data, length=str(len(data) + 1)) + b"end\t1\n",
            head + self.record(data, kind="round") + b"end\t1\n", head + self.record(data, status="weird") + b"end\t1\n",
            head + self.record(b"GIF89a" + data[6:]) + b"end\t1\n",
            head + self.record(make_png(192), width="200", height="200") + b"end\t1\n",
            head + self.record(b"", status="missing") + b"end\t1\n",
            good.replace(b"icon\ta.b/C", b"icon\ta.b/D"),
        ]:
            with self.assertRaises(Failure, msg=broken[:80]):
                runner.parse_icons(broken, ["a.b/C"])
        missing = head + b"icon\ta.b/C\tmissing\t0\t0\t-\t0\t0\t-\n" + b"end\t1\n"
        self.assertEqual("missing", runner.parse_icons(missing, ["a.b/C"])[0].status)
        transparent = runner.IconRecord("a.b/C", "ok", 192, 192, "adaptive", "0", 1, "x", make_png(192, opaque_corners=False))
        with self.assertRaises(Failure):
            runner.check_picture(transparent)

    def test_png_decoder(self) -> None:
        width, height, channels, rows = runner.decode_png(make_png(24))
        self.assertEqual((24, 24, 4), (width, height, channels))
        self.assertEqual((30, 144, 255, 255), runner.pixel(rows, channels, 0, 0))
        self.assertEqual((255, 255, 255, 255), runner.pixel(rows, channels, 12, 12))
        self.assertEqual(3, runner.decode_png(make_png(24, alpha=False))[2])


class Steps(Fixture):
    def run_steps(self, deviation: str | None) -> dict[str, str]:
        image = ScriptedImage()
        image.deviation = deviation
        report = runner.Report()
        options = types_namespace()
        with contextlib.redirect_stdout(io.StringIO()), \
                unittest.mock.patch.object(runner.time, "sleep", lambda seconds: None), \
                unittest.mock.patch.object(runner, "open_guest", lambda options, adb, private: (image, lambda: None)), \
                unittest.mock.patch.object(runner.probe, "build_apk", self.build):
            runner.run(options, "adb", pathlib.Path(tempfile.mkdtemp(prefix="uml-appshort-run-")), report)
        self.image = image
        return {name: status for name, status, _ in report.rows}

    def failing(self, deviation: str) -> set[str]:
        return {name for name, status in self.run_steps(deviation).items() if status == "FAIL"}

    def test_a_conforming_image_passes_every_step(self) -> None:
        statuses = self.run_steps(None)
        self.assertEqual({"PASS"}, set(statuses.values()), statuses)
        self.assertEqual(11, len(statuses))
        self.assertEqual(set(), self.image.files)

    def test_each_deviation_fails_its_step(self) -> None:
        header = "digest and list agree on the header block"
        rows = "list names the built-in and the probe's applications, escaped and bounded"
        icons = "icons are PNG records that check out"
        launch = "launch starts a launcher activity"
        refusals = "launch refuses all else, and runs nothing it is given"
        usage = "usage errors are status 64"
        clean = "the command writes nothing and is denied nothing"
        sets = "the digest follows the set of applications"
        expected = {
            "digest-short": header, "header-differs": header, "unsorted": header, "label-raw": header,
            "label-uncut": header, "label-bidi": header, "list-no-end": header,
            "hidden-listed": rows,
            "icon-sha": icons, "icon-size": icons, "icon-corner": icons, "icons-no-end": icons, "icons-order": icons,
            "launch-not-resumed": launch,
            "launch-hidden": refusals, "injection": refusals, "refusal-multiline": refusals, "disabled-launch": refusals,
            "usage-status": usage,
            "writes-file": clean, "denial": clean,
            "digest-constant": sets, "static-keys": sets,
        }
        for deviation, step in expected.items():
            with self.subTest(deviation=deviation):
                failed = self.failing(deviation)
                self.assertIn(step, failed, f"{deviation}: failed steps {sorted(failed)}")


def types_namespace():
    import argparse
    return argparse.Namespace(serial="x", release=None, emulator=None, adb_port=1, cpus=1, memory="1G", boot_timeout=1)


if __name__ == "__main__":
    if shutil.which("keytool") is None or shutil.which("jarsigner") is None:
        print("SKIP: no keytool and jarsigner on PATH")
        sys.exit(SKIP)
    unittest.main(argv=[sys.argv[0], "-v"], exit=True)
