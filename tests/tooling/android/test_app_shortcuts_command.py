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
the launch refusal, the grammar, the digest's inputs, the PNG checks, the deadlines, the child process's drain
and reaping, the icon source's bound), and requires the rows named here to fail: a row that cannot tell the rule
from its absence is no row. The Android glue (AndroidPlatform) cannot run on a JVM: what it must not do (take a
substitute for a lookup that failed, read the manifest's enabled flag, use the wall clock, draw an icon for the
device's own configuration, add resource files its own way, leave the image's build time out of a built-in
application's icon key) is checked on its text, and each of those checks has its own mutant. The Java rows run on a
64 MiB heap, so that a label of megabytes does not fit a copy of itself (the cleaning's bound is a row). Skipped (status 77) where the host has no JDK.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import concurrent.futures
import os
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
     "return LabelText.sanitize(platform.label(entry), entry.packageName);",
     "String raw = platform.label(entry);\n            return raw == null ? entry.packageName : raw;",
     {"list-hostile-labels"}),
    ("label-cleaned-whole", "LabelText.java",
     "            if (out.length() > CLEAN_LIMIT) {\n"
     "                break; // past both bounds already: cut() keeps less than this, so the rest cannot change the label\n"
     "            }\n",
     "",
     {"label-huge-is-cut-without-copying-it", "list-huge-label-is-one-row"}),
    ("label-cleaning-stops-too-soon", "LabelText.java",
     "static final int CLEAN_LIMIT = 2 * MAX_BYTES;",
     "static final int CLEAN_LIMIT = 10;",
     {"label-cleaning-stops-without-changing-the-label", "label-huge-is-cut-without-copying-it"}),
    ("label-out-of-memory-not-contained", "AppsCommand.java",
     "} catch (Exception | OutOfMemoryError problem) {",
     "} catch (Exception problem) {",
     {"list-label-out-of-memory-uses-the-package"}),
    ("label-failure-fails-the-listing", "AppsCommand.java",
     "            return LabelText.sanitize(null, entry.packageName);\n        }\n    }",
     "            throw new IllegalStateException(problem);\n        }\n    }",
     {"list-label-unreadable-uses-the-package", "list-label-out-of-memory-uses-the-package"}),
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
     "boolean error = line.startsWith(\"Error\");",
     "boolean error = false;",
     {"launch-failures", "start-outcome-kinds"}),
    ("error-matched-anywhere-in-a-line", "StartOutcome.java",
     "boolean error = line.startsWith(\"Error\");",
     "boolean error = line.contains(\"Error\");",
     {"launch-component-named-like-an-error", "start-outcome-kinds"}),
    ("exception-matched-anywhere-in-a-line", "StartOutcome.java",
     "if (error || line.startsWith(\"Exception\")) {",
     "if (error || line.contains(\"Exception\")) {",
     {"launch-component-named-like-an-error", "start-outcome"}),
    ("start-reason-copies-the-commands-text", "StartOutcome.java",
     "return ascii(\"cmd activity start-activity: \" + reason, MAX_REASON);",
     "return ascii(\"cmd activity start-activity: \" + reason + output, MAX_REASON);",
     {"start-outcome-kinds", "launch-failures"}),
    ("framework-message-printed", "AppsCommand.java",
     "return \"unexpected \" + kind;",
     "return String.valueOf(problem.getMessage());",
     {"failure-reasons-are-fixed"}),
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
     "            } catch (Throwable problem) {\n                made[0] = null;",
     "            } catch (Exception problem) {\n                made[0] = null;",
     {"icons-failing-render-is-silent"}),
    ("icon-render-time-limit-removed", "AppsCommand.java",
     "worker.join(platform.renderLimitMillis());",
     "worker.join(3_000);",
     {"icons-render-time-limit"}),
    ("stuck-render-does-not-stop-the-rest", "AppsCommand.java",
     "stuck = render.timedOut;",
     "stuck = false;",
     {"icons-render-time-limit"}),
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
     {"stderr-is-one-bounded-line"}),
    ("stderr-not-ascii", "StartOutcome.java",
     "out.append(c >= 0x20 && c < 0x7F ? c : '?');",
     "out.append(c);",
     {"stderr-is-one-bounded-line"}),
    ("locale-tag-limited-to-35", "Listing.java",
     "if (tag == null || tag.isEmpty() || !isTag(tag)) {",
     "if (tag == null || tag.isEmpty() || tag.length() > 35 || !isTag(tag)) {",
     {"locale-line"}),
    ("digest-ignores-the-rest-of-the-locales", "Listing.java",
     "if (!restOfLocales.isEmpty()) {",
     "if (false) {",
     {"digest-follows-the-whole-locale-list"}),
    ("icon-key-ignores-the-resource-files", "Listing.java",
     "(entry.builtin ? '1' : '0') + '\\0' + entry.resources;",
     "(entry.builtin ? '1' : '0');",
     {"icon-key-follows-the-icon", "digest-follows-the-set"}),
    ("render-version-not-bumped", "Listing.java",
     "static final int RENDER_VERSION = 2;",
     "static final int RENDER_VERSION = 1;",
     {"icon-key-follows-the-icon"}),
    ("source-bitmap-bound-removed", "SourceBound.java",
     "static final long MAX_BITMAP_PIXELS = 16L * 1024 * 1024;",
     "static final long MAX_BITMAP_PIXELS = 16L * 1024 * 1024 * 1024;",
     {"source-bound-one-bitmap", "source-bound-follows-the-density"}),
    ("source-total-bound-removed", "SourceBound.java",
     "static final long MAX_TOTAL_PIXELS = 32L * 1024 * 1024;",
     "static final long MAX_TOTAL_PIXELS = 32L * 1024 * 1024 * 1024;",
     {"source-bound-layers-add-up"}),
    ("source-density-ignored", "SourceBound.java",
     "if (node.density != DENSITY_NONE) {",
     "if (false) {",
     {"source-bound-follows-the-density"}),
    ("source-resource-count-removed", "SourceBound.java",
     "static final int MAX_RESOURCES = 32;",
     "static final int MAX_RESOURCES = 3200;",
     {"source-bound-walk-is-bounded"}),
    ("source-depth-removed", "SourceBound.java",
     "static final int MAX_DEPTH = 6;",
     "static final int MAX_DEPTH = 600;",
     {"source-bound-walk-is-bounded"}),
    ("source-cycle-not-noticed", "SourceBound.java",
     "if (id == 0 || !seen.add(id)) {",
     "if (id == 0) {",
     {"source-bound-walk-is-bounded"}),
    ("source-counts-colours", "SourceBound.java",
     "            seen.remove(id); // a colour or a string costs nothing: only the files count towards the bound\n",
     "",
     {"source-bound-colours-are-free"}),
    ("source-unreadable-bitmap-passes", "SourceBound.java",
     "if (node.width <= 0 || node.height <= 0) {",
     "if (false) {",
     {"source-bound-unreadable-is-an-error"}),
    ("resource-overlays-dropped", "ResourcePaths.java",
     "            items.add(new Item(Kind.OVERLAY, overlay));\n",
     "",
     {"resource-paths-follow-the-framework"}),
    ("resource-libraries-not-only-apks", "ResourcePaths.java",
     "if (present(library) && library.endsWith(\".apk\")) {",
     "if (present(library)) {",
     {"resource-paths-follow-the-framework"}),
    ("resource-base-is-the-private-source-dir", "ResourcePaths.java",
     "String base = present(publicSourceDir) ? publicSourceDir : sourceDir;",
     "String base = sourceDir;",
     {"resource-paths-follow-the-framework"}),
    ("resource-splits-dropped", "ResourcePaths.java",
     "String[] splits = splitPublicSourceDirs != null ? splitPublicSourceDirs : splitSourceDirs;",
     "String[] splits = null;",
     {"resource-paths-follow-the-framework"}),
    ("resource-overlays-not-deduplicated", "ResourcePaths.java",
     "if (present(overlay) && !overlays.contains(overlay)) {",
     "if (present(overlay)) {",
     {"resource-paths-follow-the-framework"}),
    ("child-output-not-drained", "ChildProcess.java",
     "            reader.start();\n",
     "",
     {"child-output-is-drained"}),
    ("child-not-terminated", "ChildProcess.java",
     "                terminate(process);\n",
     "",
     {"child-deadline-terminates-and-reaps", "child-is-reaped-when-the-run-is-interrupted",
      "child-that-ignores-termination-is-killed"}),
    ("child-not-killed", "ChildProcess.java",
     "                    process.destroyForcibly();\n                    process.waitFor(GRACE_MILLIS, TimeUnit.MILLISECONDS);\n",
     "",
     {"child-that-ignores-termination-is-killed"}),
    ("child-deadline-is-not-a-failure", "ChildProcess.java",
     "throw new PlatformException(\"cmd activity start-activity did not finish\");",
     "return new Result(-1, \"\");",
     {"child-deadline-terminates-and-reaps"}),
]

# What the Android glue must and must not do, as text (it cannot run on a JVM): (message, test of the comment-free text).
# One mutant for each: (name, text replaced, replacement, the message that must be reported).
PLATFORM_RULES = [
    ("no substitute for the foreground user", lambda t: "UserHandle" not in t),
    ("no process default for the locales", lambda t: "LocaleList.getDefault" not in t),
    ("the foreground user's lookup fails the command", lambda t: 'throw new PlatformException("the foreground user is not known"' in t),
    ("a failed permission check fails the command", lambda t: 'throw new PlatformException("a permission could not be checked"' in t),
    ("a failed configuration read fails the command", lambda t: t.count('throw new PlatformException("the system\'s configuration cannot be read"') == 2),
    ("a package gone since the query is left out",
     lambda t: re.search(r"if \(packageInfo == null\) \{\s*continue;", t) is not None and "packageInfo == null ?" not in t),
    ("no catch swallows a failure",
     lambda t: re.search(r"catch \((?:Exception|RuntimeException|Throwable) \w+\) \{\s*\}", t) is None),
    ("enabled is the resolver's answer, not the manifest flag", lambda t: "isEnabled(" not in t and ".enabled" not in t
     and re.search(r"info\.exported,\s*true,\s*permitted\(", t) is not None),
    ("the icon deadline uses the monotonic clock", lambda t: "SystemClock.elapsedRealtime()" in t and "currentTimeMillis" not in t),
    ("the icon source is bounded before it is inflated",
     lambda t: "SourceBound.within(" in t and t.index("SourceBound.within(") < t.index("getDrawableForDensity(")),
    ("icons are drawn for a fixed configuration", lambda t: "UI_MODE_NIGHT_NO" in t and "ICON_LOCALE" in t
     and "configuration()" not in t.split("private static Configuration iconConfiguration()")[1].split("\n    }\n")[0]),
    ("a built-in application that was never updated carries the image's build time in its icon key",
     lambda t: re.search(r"FLAG_SYSTEM\) != 0;", t) is not None
     and re.search(r"builtIn && \(application\.flags & ApplicationInfo\.FLAG_UPDATED_SYSTEM_APP\) == 0\) \{\s*"
                   r"updated = Math\.max\(updated, Build\.TIME\);", t) is not None
     and re.search(r"info\.exported,\s*true,\s*permitted\(info\.permission\), packageInfo\.getLongVersionCode\(\),\s*"
                   r"updated,", t) is not None),
    ("every resource file the framework adds is added",
     lambda t: all(word in t for word in ("addAssetPathAsSharedLibrary(", "addOverlayPath(", "ResourcePaths.of(",
                                          "application.publicSourceDir", "application.splitPublicSourceDirs",
                                          "application.sharedLibraryFiles", "application.resourceDirs",
                                          "application.overlayPaths"))),
    ("a base, split or library that cannot be added leaves the resources unopened",
     lambda t: "complete = cookie != 0 || item.kind == ResourcePaths.Kind.OVERLAY;" in t),
    ("the start is run by ChildProcess", lambda t: "ChildProcess.run(AppsCommand.startArguments(" in t
     and "ProcessBuilder" not in t and "waitFor" not in t),
]

PLATFORM_MUTATIONS = [
    ("foreground-user-substituted",
     "throw new PlatformException(\"the foreground user is not known\", problem);",
     "return android.os.UserHandle.myUserId();",
     "no substitute for the foreground user"),
    ("locales-default-substituted",
     "throw new PlatformException(\"the system's configuration cannot be read\", problem);",
     "return new Configuration();",
     "a failed configuration read fails the command"),
    ("permission-check-swallowed",
     "throw new PlatformException(\"a permission could not be checked\", problem);",
     "known = false;",
     "a failed permission check fails the command"),
    ("vanished-package-listed",
     "            if (packageInfo == null) {\n                continue; // removed since the query\n            }\n",
     "",
     "a package gone since the query is left out"),
    ("manifest-enabled-flag-read",
     "info.exported, true, permitted(info.permission)",
     "info.exported, info.isEnabled(), permitted(info.permission)",
     "enabled is the resolver's answer, not the manifest flag"),
    ("wall-clock-for-the-deadline",
     "return SystemClock.elapsedRealtime();",
     "return System.currentTimeMillis();",
     "the icon deadline uses the monotonic clock"),
    ("icon-source-unbounded",
     "        if (!SourceBound.within(new ResourceGraph(resources), entry.iconResource, ICON_DENSITY)) {\n"
     "            throw new PlatformException(\"the icon's source is over the bound\");\n        }\n",
     "",
     "the icon source is bounded before it is inflated"),
    ("icon-drawn-for-the-device-configuration",
     "        Configuration configuration = new Configuration();\n        configuration.densityDpi = ICON_DENSITY;",
     "        Configuration configuration = new Configuration(configuration());\n        configuration.densityDpi = ICON_DENSITY;",
     "icons are drawn for a fixed configuration"),
    ("image-build-left-out-of-the-icon-key",
     "updated = Math.max(updated, Build.TIME);",
     "updated = updated + 0;",
     "a built-in application that was never updated carries the image's build time in its icon key"),
    ("updated-built-in-also-takes-the-image-build",
     "if (builtIn && (application.flags & ApplicationInfo.FLAG_UPDATED_SYSTEM_APP) == 0) {",
     "if (builtIn) {",
     "a built-in application that was never updated carries the image's build time in its icon key"),
    ("overlays-not-added",
     "cookie = assets.addOverlayPath(item.path);",
     "cookie = 1;",
     "every resource file the framework adds is added"),
    ("split-failure-ignored",
     "complete = cookie != 0 || item.kind == ResourcePaths.Kind.OVERLAY;",
     "complete = true;",
     "a base, split or library that cannot be added leaves the resources unopened"),
    ("start-waits-before-reading",
     "ChildProcess.run(AppsCommand.startArguments(user, component),",
     "new ProcessBuilder(AppsCommand.startArguments(user, component)).start().waitFor(); ChildProcess.run(AppsCommand.startArguments(user, component),",
     "the start is run by ChildProcess"),
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
    ran = subprocess.run([java, "-Xmx64m", "-Dfile.encoding=UTF-8", "-cp", str(classes), "com.kernarium.apps.AppsCommandTest"],
                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         timeout=120, check=False)
    return ran.returncode, ran.stdout


def failed_rows(output: str) -> set[str]:
    return {m.group(1) for m in re.finditer(r"^FAIL ([a-z0-9-]+)", output, re.MULTILINE)}


def strip_comments(text: str) -> str:
    """Java source without its comments (the glue has no string with a slash pair in it)."""
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", text, flags=re.S))


def platform_problems(text: str) -> list[str]:
    """The messages of the rules the Android glue's text breaks."""
    code = strip_comments(text)
    return [message for message, holds in PLATFORM_RULES if not holds(code)]


def check_platform_rules(platform: str) -> int:
    """The rules hold for the glue, and each rule fails for a copy of it that breaks it."""
    failures = 0
    for message in platform_problems(platform):
        print(f"FAIL: AndroidPlatform: {message}", file=sys.stderr)
        failures += 1
    for name, old, new, message in PLATFORM_MUTATIONS:
        if platform.count(old) != 1:
            print(f"FAIL: platform mutation {name}: the text to replace occurs {platform.count(old)} times", file=sys.stderr)
            failures += 1
        elif message not in platform_problems(platform.replace(old, new)):
            print(f"FAIL: platform mutation {name} was not caught by the rule '{message}'", file=sys.stderr)
            failures += 1
        else:
            print(f"platform mutation {name}: caught by '{message}'")
    return failures


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
        if "ProcessBuilder" in text and name != "ChildProcess.java":
            problems.append(f"{name} starts a process: only ChildProcess does")
    main = java_files["Main.java"]
    if re.findall(r"new FileOutputStream\((.*?)\)", main) != ["FileDescriptor.out"]:
        problems.append("Main writes anything but the descriptor of stdout")
    platform = java_files["AndroidPlatform.java"]
    child = java_files["ChildProcess.java"]
    if child.count("new ProcessBuilder(argv)") != 1 or "redirectErrorStream(true)" not in child:
        problems.append("ChildProcess starts a process other than the one with the caller's argument vector")
    if "finally" not in child or "terminate(process);" not in child:
        problems.append("ChildProcess does not terminate its child in a finally")
    if check_platform_rules(platform):
        problems.append("the Android glue breaks a rule it must keep")
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
    expected = {"AppsCommand.java", "ChildProcess.java", "Component.java", "Entry.java", "Icon.java", "LabelText.java",
                "Listing.java", "PlatformException.java", "ResourcePaths.java", "SourceBound.java", "StartOutcome.java"}
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

        def mutate(mutation: tuple) -> tuple[bool, str]:
            """One mutant: (caught, the line to print). Each has a scratch directory of its own."""
            name, source, old, new, must_fail = mutation
            unknown = must_fail - rows
            if unknown:
                return False, f"FAIL: mutation {name} names rows that do not exist: {sorted(unknown)}"
            copy = scratch / f"src-{name}"
            shutil.copytree(package_dir, copy)
            target = copy / source
            text = target.read_text(encoding="utf-8")
            if text.count(old) != 1:
                return False, f"FAIL: mutation {name}: the text to remove occurs {text.count(old)} times in {source}"
            target.write_text(text.replace(old, new), encoding="utf-8")
            mutated = sorted(path for path in copy.glob("*.java") if path.name not in ANDROID_GLUE)
            mutant_status, mutant_output = compile_and_run(javac, java, scratch / f"run-{name}", mutated)
            shutil.rmtree(copy)
            if mutant_status == -1:
                return False, f"FAIL: mutation {name} does not compile:\n{mutant_output}"
            missed = must_fail - failed_rows(mutant_output)
            if mutant_status == 0 or missed:
                return False, f"FAIL: mutation {name} ({source}) was not caught by {sorted(missed) or 'any row'}"
            return True, f"mutation {name}: caught by {sorted(failed_rows(mutant_output) & must_fail)}"

        # The mutants are independent processes: a few at a time keeps the whole under the test's timeout.
        workers = max(1, min(3, (os.cpu_count() or 1) - 1))
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            results = list(pool.map(mutate, MUTATIONS))
        failures = 0
        for caught, line in results:
            print(line, file=sys.stdout if caught else sys.stderr)
            failures += 0 if caught else 1
        if failures:
            return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
