#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher-application shell command's logic on a plain JVM, and what the image wires of it.

android/lineage-24/overlay/app_shortcuts is a Java tool that the image runs through app_process as the adb
shell user (Andarium's Add to Home Screen lists the launcher applications and starts one through it). Its
grammar, the label escaping, the listing's rule and bounds, the framing of every answer, the icon records, the
launch's refusals and the exit statuses do not touch the platform: AppsCommandTest.java drives them against a
fake platform. The sources compile here without the Android glue (AndroidPlatform and Main, which need the
framework); the Android calls are compiled by the build host's request and exercised by the runner on an image.

The test also runs the Java rows on copies of the sources with one rule removed at a time (the escaping, a bound,
the launch refusal, the grammar, the digest's inputs, the PNG checks, the deadline), and requires the rows named
here to fail: a row that cannot tell the rule from its absence is no row. Skipped (status 77) where the host has
no JDK.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
LINEAGE = REPOSITORY / "android" / "lineage-24"
TOOL = LINEAGE / "overlay" / "app_shortcuts"
SOURCES = TOOL / "src"
TEST = HERE / "app_shortcuts" / "com" / "kernarium" / "apps" / "AppsCommandTest.java"
ANDROID_GLUE = ("AndroidPlatform.java", "Main.java")
PACKAGE_DIR = pathlib.Path("com") / "kernarium" / "apps"
SKIP = 77

# One rule removed per entry: (name, source file, the text that is replaced, its replacement, the rows that must fail).
MUTATIONS = [
    ("label-controls-pass", "LabelText.java",
     "if (type == Character.CONTROL || type == Character.LINE_SEPARATOR",
     "if (false || type == Character.LINE_SEPARATOR",
     {"label-controls-become-space", "list-hostile-labels"}),
    ("label-separators-pass", "LabelText.java",
     "|| type == Character.PARAGRAPH_SEPARATOR || type == Character.SPACE_SEPARATOR\n"
     "                || cp == 0x85 || cp == 0x1680 || Character.isWhitespace(cp)) {",
     "|| false) {",
     {"label-controls-become-space", "list-hostile-labels"}),
    ("label-bidi-passes", "LabelText.java",
     "        if (type == Character.FORMAT || cp == 0x061C || cp == 0x200E || cp == 0x200F\n"
     "                || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0xFEFF) {",
     "        if (false) {",
     {"label-bidi-and-format-dropped", "list-hostile-labels"}),
    ("label-not-escaped-in-the-listing", "AppsCommand.java",
     "LabelText.sanitize(label, entry.packageName)",
     "(label == null ? entry.packageName : label)",
     {"list-hostile-labels"}),
    ("label-surrogates-pass", "LabelText.java",
     "if (type == Character.SURROGATE || (cp & 0xFFFE) == 0xFFFE",
     "if (false || (cp & 0xFFFE) == 0xFFFE",
     {"label-surrogates-and-nonchars"}),
    ("label-not-cut", "LabelText.java",
     "static final int MAX_CODE_POINTS = 80;",
     "static final int MAX_CODE_POINTS = 8000;",
     {"label-bounds", "exit-statuses-are-the-contract"}),
    ("label-empty-not-replaced", "LabelText.java",
     "        if (text.isEmpty()) {\n            text = clean(fallback == null ? \"\" : fallback);\n        }\n",
     "",
     {"label-empty-uses-the-package"}),
    ("per-package-bound-removed", "Listing.java",
     "static final int MAX_PER_PACKAGE = 8;",
     "static final int MAX_PER_PACKAGE = 80000;",
     {"listing-per-package-bound", "exit-statuses-are-the-contract"}),
    ("row-bound-removed", "Listing.java",
     "static final int MAX_ROWS = 1024;",
     "static final int MAX_ROWS = 102400;",
     {"listing-row-bound", "list-sizes-within-the-bounds", "exit-statuses-are-the-contract"}),
    ("exported-not-required", "Entry.java",
     "return exported && enabled && permitted;",
     "return enabled && permitted;",
     {"listing-order-and-filter", "icons-not-launchable-is-missing", "launch-refuses-what-is-not-listed"}),
    ("enabled-not-required", "Entry.java",
     "return exported && enabled && permitted;",
     "return exported && permitted;",
     {"listing-order-and-filter", "icons-not-launchable-is-missing", "launch-refuses-what-is-not-listed"}),
    ("permission-not-required", "Entry.java",
     "return exported && enabled && permitted;",
     "return exported && enabled;",
     {"listing-order-and-filter", "icons-not-launchable-is-missing", "launch-refuses-what-is-not-listed"}),
    ("launch-refusal-removed", "AppsCommand.java",
     "if (find(platform.launcherActivities(user), component) == null) {",
     "if (false) {",
     {"launch-refuses-what-is-not-listed"}),
    ("launch-grammar-removed", "Component.java",
     "if (!isLetter(c) && c != '_' && c != '$' && (first || !isDigit(c))) {",
     "if (false) {",
     {"component-grammar-rejects", "launch-refuses-hostile-arguments-before-anything-runs"}),
    ("package-grammar-removed", "Component.java",
     "if (!isLetter(c) && !isDigit(c) && c != '_') {",
     "if (false) {",
     {"component-grammar-rejects", "launch-refuses-hostile-arguments-before-anything-runs"}),
    ("component-refusal-after-platform", "AppsCommand.java",
     "                if (component == null) {\n                    return refuse(err, \"not a component name\");\n                }\n",
     "                if (component == null) {\n                    continue;\n                }\n",
     {"launch-refuses-hostile-arguments-before-anything-runs", "icons-refuses-a-bad-component"}),
    ("launch-argv-through-a-shell", "AppsCommand.java",
     "\"-n\", component.token());",
     "\"-n\", \"'\" + component.token() + \"'\");",
     {"launch-starts-exactly-the-component"}),
    ("launch-error-line-ignored", "StartOutcome.java",
     "if (trimmed.startsWith(\"Error\") || trimmed.contains(\"Permission Denial\")",
     "if (false || trimmed.contains(\"Permission Denial\")",
     {"launch-failures"}),
    ("digest-ignores-language", "Listing.java",
     "        text.append(\"locales\\t\").append(locales).append('\\n');\n",
     "",
     {"digest-follows-the-set"}),
    ("digest-ignores-versions", "Listing.java",
     "'\\0' + entry.versionCode + '\\0' + entry.lastUpdateMillis + '\\0'",
     "'\\0' + '\\0' + '\\0'",
     {"digest-follows-the-set", "icon-key-follows-the-icon"}),
    ("digest-follows-the-order", "Listing.java",
     "        Collections.sort(eligible, ORDER);\n",
     "",
     {"listing-order-and-filter", "digest-follows-the-set"}),
    ("png-bound-removed", "AppsCommand.java",
     "if (icon.png.length <= MAX_PNG_BYTES) {",
     "if (true) {",
     {"icons-size-steps"}),
    ("png-bound-off-by-one", "AppsCommand.java",
     "if (icon.png.length <= MAX_PNG_BYTES) {",
     "if (icon.png.length < MAX_PNG_BYTES) {",
     {"icons-png-bound-is-exact"}),
    ("png-signature-unchecked", "AppsCommand.java",
     "if (icon == null || icon.size != size || !isPng(icon.png)) {",
     "if (icon == null || icon.size != size) {",
     {"icons-statuses"}),
    ("icon-size-unchecked", "AppsCommand.java",
     "if (icon == null || icon.size != size || !isPng(icon.png)) {",
     "if (icon == null || !isPng(icon.png)) {",
     {"icons-statuses"}),
    ("icon-deadline-removed", "AppsCommand.java",
     "if (platform.nowMillis() - begin > ICON_DEADLINE_MILLIS) {",
     "if (false) {",
     {"icons-deadline-defers-the-rest"}),
    ("icon-render-failure-escapes", "AppsCommand.java",
     "            } catch (Throwable problem) {\n                return null; // an icon",
     "            } catch (Exception problem) {\n                return null; // an icon",
     {"icons-statuses"}),
    ("icons-count-unbounded", "AppsCommand.java",
     "return args.length >= 2 && args.length <= 1 + MAX_ICONS;",
     "return args.length >= 2;",
     {"icons-count-and-duplicates"}),
    ("icons-duplicates-allowed", "AppsCommand.java",
     "if (new HashSet<>(components).size() != components.size()) {",
     "if (false) {",
     {"icons-count-and-duplicates"}),
    ("failure-prints-answer", "AppsCommand.java",
     "        if (status != EXIT_OK) {\n            return status;\n        }\n        try {\n            answer.writeTo(out);",
     "        try {\n            answer.writeTo(out);",
     {"launch-refuses-what-is-not-listed", "launch-failures"}),
    ("usage-accepts-extra-arguments", "AppsCommand.java",
     "            case \"list\":\n                return args.length == 1;",
     "            case \"list\":\n                return args.length >= 1;",
     {"usage"}),
    ("end-line-dropped", "AppsCommand.java",
     "        answer.line(\"end\\t\" + listing.rows.size());\n",
     "",
     {"list-format", "list-empty", "digest-answer"}),
    ("stderr-unbounded", "StartOutcome.java",
     "for (int i = 0; i < text.length() && out.length() < limit; i++) {",
     "for (int i = 0; i < text.length(); i++) {",
     {"stderr-is-one-bounded-line", "start-outcome"}),
    ("stderr-not-ascii", "StartOutcome.java",
     "out.append(c >= 0x20 && c < 0x7F ? c : '?');",
     "out.append(c);",
     {"stderr-is-one-bounded-line", "start-outcome"}),
]


def jdk_tool(name: str) -> str | None:
    """A working javac or java: the macOS /usr/bin stubs exist without a JDK, so ask it."""
    path = shutil.which(name)
    if path is None:
        return None
    probe = subprocess.run([path, "-version"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=60, check=False)
    return path if probe.returncode == 0 else None


def compile_and_run(javac: str, java: str, scratch: pathlib.Path, sources: list[pathlib.Path]) -> tuple[int, str]:
    classes = scratch / "classes"
    if classes.exists():
        shutil.rmtree(classes)
    classes.mkdir(parents=True)
    compiled = subprocess.run(
        [javac, "-Xlint:all", "-Werror", "-encoding", "UTF-8", "-d", str(classes),
         *(str(path) for path in sources), str(TEST)],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=240,
        check=False)
    if compiled.returncode != 0:
        return -1, f"javac:\n{compiled.stdout}"
    ran = subprocess.run([java, "-Dfile.encoding=UTF-8", "-cp", str(classes), "com.kernarium.apps.AppsCommandTest"],
                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         timeout=120, check=False)
    return ran.returncode, ran.stdout


def failed_rows(output: str) -> set[str]:
    return {m.group(1) for m in re.finditer(r"^FAIL ([a-z0-9-]+)", output, re.MULTILINE)}


def check_sources() -> int:
    """What the Java cannot tell on a JVM: the wiring, and that the tool writes and opens nothing."""
    problems: list[str] = []
    java_files = {path.name: path.read_text(encoding="utf-8") for path in SOURCES.rglob("*.java")}
    for name, text in java_files.items():
        if not text.startswith("// SPDX-License-Identifier: Apache-2.0\n"):
            problems.append(f"{name} does not carry the Apache-2.0 header of code that goes into the image")
        for forbidden in ("SystemProperties.set", "setProperty", "Settings.", "ContentResolver", "FileWriter",
                          "Files.write", "RandomAccessFile", "java.net.", "Socket", "Runtime.getRuntime",
                          "\"/system/bin/sh\"", "\"sh\"", "FileChannel", "Files.createFile",
                          "File.createTempFile", "Files.newOutputStream", "setComponentEnabledSetting",
                          "grantRuntimePermission", "installPackage"):
            if forbidden in text:
                problems.append(f"{name} uses {forbidden}: the command writes nothing and opens no network")
        if "FileOutputStream" in text and name != "Main.java":
            problems.append(f"{name} opens a FileOutputStream: only Main writes, to the descriptor of stdout")
        if "ProcessBuilder" in text and name != "AndroidPlatform.java":
            problems.append(f"{name} starts a process: only the platform's start does")
    main = java_files["Main.java"]
    if re.findall(r"new FileOutputStream\((.*?)\)", main) != ["FileDescriptor.out"]:
        problems.append("Main writes anything but the descriptor of stdout")
    platform = java_files["AndroidPlatform.java"]
    if platform.count("new ProcessBuilder(") != 1 or "new ProcessBuilder(AppsCommand.startArguments(" not in platform:
        problems.append("the platform starts a process other than the one with the command's argument vector")
    commands = java_files["AppsCommand.java"]
    if '"/system/bin/cmd", "activity", "start-activity"' not in commands:
        problems.append("the start is not cmd activity start-activity")
    for flag in ("--es", "--ei", "--ez", "-d", "--data", "--grant-read-uri-permission", "-W"):
        if f'"{flag}"' in commands:
            problems.append(f"the start passes {flag}: it takes no extra, data or flag from outside")
    bp = (TOOL / "Android.bp").read_text(encoding="utf-8")
    for expected in ('java_binary {', 'name: "kernarium_apps"', 'wrapper: "kernarium_apps"',
                     'srcs: ["src/**/*.java"]', 'platform_apis: true', 'system_ext_specific: true'):
        if expected not in bp:
            problems.append(f"Android.bp lacks {expected}")
    code = "\n".join(line for line in bp.splitlines() if not line.lstrip().startswith("//"))
    for forbidden in ("privileged", "certificate", "uses_libs", "permissions"):
        if forbidden in code:
            problems.append(f"Android.bp has {forbidden}: the tool holds nothing of its own")
    wrapper = (TOOL / "kernarium_apps").read_text(encoding="utf-8")
    for expected in ("#!/system/bin/sh", "base=/system_ext", "CLASSPATH=$base/framework/kernarium_apps.jar",
                     "exec app_process $base/bin com.kernarium.apps.Main \"$@\""):
        if expected not in wrapper:
            problems.append(f"the wrapper lacks {expected}")
    if not (TOOL / "kernarium_apps").stat().st_mode & 0o100:
        problems.append("the wrapper is not executable")
    makefile = (LINEAGE / "overlay" / "lineage_andarium.mk").read_text(encoding="utf-8")
    if not re.search(r"^PRODUCT_PACKAGES \+= kernarium_apps$", makefile, re.MULTILINE):
        problems.append("the product does not install kernarium_apps")
    build = (LINEAGE / "build" / "build-aosp.sh").read_text(encoding="utf-8")
    if '*" kernarium_apps "*' not in build:
        problems.append("the build does not check that kernarium_apps is in the product")
    verifier = (LINEAGE / "tools" / "verify-target-files.py").read_text(encoding="utf-8")
    for expected in ('"SYSTEM_EXT/bin/kernarium_apps"', '"SYSTEM_EXT/framework/kernarium_apps.jar"',
                     "check_apps_command("):
        if expected not in verifier:
            problems.append(f"the image verifier lacks {expected}")
    sepolicy = LINEAGE / "overlay" / "sepolicy"
    for path in sepolicy.rglob("*"):
        if path.is_file() and "kernarium_apps" in path.read_text(encoding="utf-8", errors="replace"):
            problems.append(f"{path.relative_to(LINEAGE)} names kernarium_apps: the tool needs no policy of its own")
    for problem in problems:
        print(f"FAIL: {problem}", file=sys.stderr)
    return 1 if problems else 0


def main() -> int:
    if check_sources() != 0:
        return 1
    javac, java = jdk_tool("javac"), jdk_tool("java")
    if javac is None or java is None:
        print("SKIP: no JDK (javac and java) on PATH")
        return SKIP
    package_dir = SOURCES / PACKAGE_DIR
    sources = sorted(path for path in package_dir.glob("*.java") if path.name not in ANDROID_GLUE)
    expected = {"AppsCommand.java", "Component.java", "Entry.java", "Icon.java", "LabelText.java", "Listing.java",
                "StartOutcome.java"}
    if {path.name for path in sources} != expected:
        print(f"FAIL: the platform-free sources are {sorted(path.name for path in sources)}, not {sorted(expected)}",
              file=sys.stderr)
        return 1
    for name in ANDROID_GLUE:
        if not (package_dir / name).is_file():
            print(f"FAIL: {name} is missing", file=sys.stderr)
            return 1
    with tempfile.TemporaryDirectory(prefix="uml-app-shortcuts-") as directory:
        scratch = pathlib.Path(directory)
        status, output = compile_and_run(javac, java, scratch / "base", sources)
        print(output, end="")
        if status != 0 or not output.rstrip().endswith("PASS"):
            print("FAIL: the command's test", file=sys.stderr)
            return 1
        rows = set(re.findall(r"^ok ([a-z0-9-]+)", output, re.MULTILINE))
        failures = 0
        for name, source, old, new, must_fail in MUTATIONS:
            unknown = must_fail - rows
            if unknown:
                print(f"FAIL: mutation {name} names rows that do not exist: {sorted(unknown)}", file=sys.stderr)
                failures += 1
                continue
            copy = scratch / f"src-{name}"
            shutil.copytree(package_dir, copy)
            target = copy / source
            text = target.read_text(encoding="utf-8")
            if text.count(old) != 1:
                print(f"FAIL: mutation {name}: the text to remove occurs {text.count(old)} times in {source}",
                      file=sys.stderr)
                failures += 1
                continue
            target.write_text(text.replace(old, new), encoding="utf-8")
            mutated = sorted(path for path in copy.glob("*.java") if path.name not in ANDROID_GLUE)
            mutant_status, mutant_output = compile_and_run(javac, java, scratch / f"run-{name}", mutated)
            if mutant_status == -1:
                print(f"FAIL: mutation {name} does not compile:\n{mutant_output}", file=sys.stderr)
                failures += 1
                continue
            missed = must_fail - failed_rows(mutant_output)
            if mutant_status == 0 or missed:
                print(f"FAIL: mutation {name} ({source}) was not caught by {sorted(missed) or 'any row'}",
                      file=sys.stderr)
                failures += 1
            else:
                print(f"mutation {name}: caught by {sorted(failed_rows(mutant_output) & must_fail)}")
            shutil.rmtree(copy)
        if failures:
            return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
