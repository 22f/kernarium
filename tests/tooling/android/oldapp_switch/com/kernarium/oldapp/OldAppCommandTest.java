// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Old app compatibility shell command (android/lineage-24/overlay/oldapp_switch) against a fake
// platform, on a plain JVM: the eligibility rule, the listing's format, the writes, the order of the
// stop after the write and the exit statuses. The package is the command's, so that the test reaches
// what the command keeps to itself. test_oldapp_switch_command.py compiles and runs it.

package com.kernarium.oldapp;

import java.io.PrintWriter;
import java.io.StringWriter;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class OldAppCommandTest {
    private static int failures;

    private static void check(boolean condition, String what) {
        if (!condition) {
            failures++;
            System.out.println("FAIL: " + what);
        }
    }

    private static void same(Object expected, Object actual, String what) {
        check(expected.equals(actual), what + ": expected <" + expected + "> but got <" + actual + ">");
    }

    private static final List<String> ABIS32 = Arrays.asList("armeabi-v7a", "armeabi");

    private static Candidate app(String name, boolean system, String abi, int target, String sharedUser,
            long updated) {
        return new Candidate(name, system, abi, target, sharedUser, updated, name);
    }

    private static final class Fake implements OldAppCommand.Platform {
        final List<Candidate> installed = new ArrayList<>();
        final Map<String, String> labels = new HashMap<>();
        final Map<String, String> properties = new HashMap<>();
        final List<String> events = new ArrayList<>();
        boolean keepWrites = true;
        boolean stopFails;
        boolean listFails;
        long now = 1700000000L;

        @Override
        public List<Candidate> installedPackages() throws Exception {
            if (listFails) {
                throw new IllegalStateException("the package manager died");
            }
            return installed;
        }

        @Override
        public Candidate find(String packageName) {
            for (Candidate candidate : installed) {
                if (candidate.packageName.equals(packageName)) {
                    return candidate;
                }
            }
            return null;
        }

        @Override
        public List<String> abis32() {
            return ABIS32;
        }

        @Override
        public String label(Candidate candidate) throws Exception {
            String label = labels.get(candidate.packageName);
            if (label != null && label.equals("<throws>")) {
                throw new IllegalStateException("no resources");
            }
            return label;
        }

        @Override
        public String getProperty(String name) {
            return properties.getOrDefault(name, "");
        }

        @Override
        public void setProperty(String name, String value) {
            events.add("set " + name + "=" + value);
            if (keepWrites) {
                properties.put(name, value);
            }
        }

        @Override
        public void forceStop(String packageName) {
            events.add("stop " + packageName);
            if (stopFails) {
                throw new IllegalStateException("cmd activity force-stop exited 255");
            }
        }

        @Override
        public long nowSeconds() {
            return now;
        }
    }

    private static final class Result {
        int status;
        String out;
        String err;
    }

    private static Result run(Fake platform, String... args) {
        StringWriter out = new StringWriter();
        StringWriter err = new StringWriter();
        Result result = new Result();
        PrintWriter outWriter = new PrintWriter(out);
        PrintWriter errWriter = new PrintWriter(err);
        result.status = OldAppCommand.run(args, platform, outWriter, errWriter);
        outWriter.flush();
        errWriter.flush();
        result.out = out.toString();
        result.err = err.toString();
        return result;
    }

    private static Fake fixture() {
        Fake fake = new Fake();
        fake.installed.add(app("org.example.zed", false, "armeabi-v7a", 10, null, 300L));
        fake.installed.add(app("org.example.alpha", false, "armeabi", 13, null, 100L));
        fake.installed.add(app("org.example.modern", false, "armeabi-v7a", 14, null, 5L));
        fake.installed.add(app("org.example.sixty_four", false, "arm64-v8a", 10, null, 5L));
        fake.installed.add(app("org.example.javaonly", false, null, 10, null, 5L));
        fake.installed.add(app("org.example.system", true, "armeabi-v7a", 10, null, 5L));
        fake.installed.add(app("org.example.shared", false, "armeabi-v7a", 10, "org.example.uid", 5L));
        fake.installed.add(app("org.example.nolabel", false, "armeabi-v7a", 4, null, 7L));
        fake.installed.add(app("org.example.broken", false, "armeabi-v7a", 4, null, 8L));
        fake.labels.put("org.example.zed", "Zed");
        fake.labels.put("org.example.alpha", "Alpha\tGame\nTwo\r\nLines End");
        fake.labels.put("org.example.broken", "<throws>");
        fake.properties.put("persist.sys.kernarium.oldapp.org.example.zed", "1699999000");
        return fake;
    }

    private static void eligibility() {
        same(true, OldAppEligibility.isEligible(app("a.b", false, "armeabi-v7a", 13, null, 0), ABIS32), "armeabi-v7a, 13");
        same(true, OldAppEligibility.isEligible(app("a.b", false, "armeabi", 0, null, 0), ABIS32), "armeabi, 0");
        same(false, OldAppEligibility.isEligible(app("a.b", false, "armeabi-v7a", 14, null, 0), ABIS32), "target 14");
        same(false, OldAppEligibility.isEligible(app("a.b", true, "armeabi-v7a", 10, null, 0), ABIS32), "system");
        same(false, OldAppEligibility.isEligible(app("a.b", false, "armeabi-v7a", 10, "uid", 0), ABIS32), "shared user");
        same(false, OldAppEligibility.isEligible(app("a.b", false, "arm64-v8a", 10, null, 0), ABIS32), "64-bit");
        same(false, OldAppEligibility.isEligible(app("a.b", false, null, 10, null, 0), ABIS32), "no native code");
        same(false, OldAppEligibility.isEligible(app("a.b", false, "x86", 10, null, 0), ABIS32), "x86 on an arm device");
        same("persist.sys.kernarium.oldapp.a.b", OldAppEligibility.propertyName("a.b"), "property name");
        for (String good : new String[] {"a.b", "org.example.Foo_1", "com.a1.b2", "single"}) {
            check(OldAppEligibility.isPackageName(good), "package name accepted: " + good);
        }
        for (String bad : new String[] {"", ".a", "a.", "a..b", "1a.b", "a b", "a-b.c", "a.b/c", "a.b\n", "../x", "a.b;c"}) {
            check(!OldAppEligibility.isPackageName(bad), "package name refused: <" + bad + ">");
        }
    }

    private static void listing() {
        Fake fake = fixture();
        Result result = run(fake, "list");
        same(0, result.status, "list status");
        same("", result.err, "list stderr");
        String[] lines = result.out.split("\n", -1);
        same("kernarium_oldapp 1", lines[0], "list header");
        // Sorted by package name; only the eligible ones.
        same("org.example.alpha\tAlpha Game Two  Lines End\toff\t0\t100", lines[1], "alpha line");
        same("org.example.broken\torg.example.broken\toff\t0\t8", lines[2], "label that cannot be read");
        same("org.example.nolabel\torg.example.nolabel\toff\t0\t7", lines[3], "label that is null");
        same("org.example.zed\tZed\ton\t1699999000\t300", lines[4], "zed line");
        same("", lines[5], "list ends with a newline");
        same(6, lines.length, "list line count");
        for (String line : lines) {
            if (!line.isEmpty() && !line.startsWith("kernarium_oldapp")) {
                same(5, line.split("\t", -1).length, "five fields: " + line);
            }
        }
        same(0, fake.events.size(), "list writes nothing");

        Fake none = new Fake();
        Result empty = run(none, "list");
        same("kernarium_oldapp 1\n", empty.out, "an empty listing is the header");
        same(0, empty.status, "empty listing status");

        Fake failing = fixture();
        failing.listFails = true;
        Result failed = run(failing, "list");
        same(1, failed.status, "list failure status");
        same("", failed.out, "list failure prints no listing");
        check(failed.err.startsWith("kernarium_oldapp: list failed:"), "list failure message: " + failed.err);
    }

    private static void switching() {
        Fake fake = fixture();
        Result on = run(fake, "set", "org.example.alpha", "on");
        same(0, on.status, "set on status");
        same("org.example.alpha\ton\n", on.out, "set on output");
        same("", on.err, "set on stderr");
        same(Arrays.asList("set persist.sys.kernarium.oldapp.org.example.alpha=1700000000", "stop org.example.alpha"),
                fake.events, "write before stop");
        // The listing now shows it.
        check(run(fake, "list").out.contains("org.example.alpha\tAlpha Game Two  Lines End\ton\t1700000000\t100\n"),
                "listing after set on");

        fake.events.clear();
        Result off = run(fake, "set", "org.example.zed", "off");
        same(0, off.status, "set off status");
        same("org.example.zed\toff\n", off.out, "set off output");
        same(Arrays.asList("set persist.sys.kernarium.oldapp.org.example.zed=", "stop org.example.zed"), fake.events,
                "off clears then stops");
        check(run(fake, "list").out.contains("org.example.zed\tZed\toff\t0\t300\n"), "listing after set off");

        for (String refused : new String[] {"org.example.modern", "org.example.sixty_four", "org.example.javaonly",
                "org.example.system", "org.example.shared", "org.example.unknown"}) {
            fake.events.clear();
            Result result = run(fake, "set", refused, "on");
            same(2, result.status, "refused status for " + refused);
            same("", result.out, "refused stdout for " + refused);
            same(1, result.err.split("\n").length, "one line on stderr for " + refused);
            check(result.err.contains(refused), "the refusal names " + refused + ": " + result.err);
            same(0, fake.events.size(), "a refusal writes and stops nothing: " + refused);
            Result off2 = run(fake, "set", refused, "off");
            same(2, off2.status, "refused off status for " + refused);
        }

        Fake dropped = fixture();
        dropped.keepWrites = false;
        Result notKept = run(dropped, "set", "org.example.alpha", "on");
        same(1, notKept.status, "a write the system did not keep");
        same("", notKept.out, "no success line");
        same(Arrays.asList("set persist.sys.kernarium.oldapp.org.example.alpha=1700000000"), dropped.events,
                "nothing is stopped after a write that was not kept");

        Fake stuck = fixture();
        stuck.stopFails = true;
        Result stopFailed = run(stuck, "set", "org.example.alpha", "on");
        same(1, stopFailed.status, "a stop that failed");
        check(stopFailed.err.contains("could not be stopped"), "the stop failure is said: " + stopFailed.err);
        same("", stopFailed.out, "no success line after a failed stop");
    }

    private static void usage() {
        String[][] bad = {
            {}, {"help"}, {"list", "extra"}, {"set"}, {"set", "org.example.alpha"}, {"set", "org.example.alpha", "maybe"},
            {"set", "org.example.alpha", "ON"}, {"set", "bad..name", "on"}, {"set", "--help", "on"},
            {"set", "org.example.alpha", "on", "extra"}, {"LIST"}, {"set", "a b", "on"}, {"get", "org.example.alpha"},
        };
        for (String[] args : bad) {
            Fake fake = fixture();
            Result result = run(fake, args);
            String shown = String.join(" ", args);
            same(64, result.status, "usage status for <" + shown + ">");
            check(result.err.startsWith("usage: kernarium_oldapp list\n"), "usage text for <" + shown + ">: " + result.err);
            same("", result.out, "usage stdout for <" + shown + ">");
            same(0, fake.events.size(), "usage writes nothing for <" + shown + ">");
        }
    }

    private static void oneLine() {
        same("a b c d e f g h i", OldAppCommand.oneLine("a\tb\nc\rd\u000Be\ff\u0085g h i"), "separators");
        same("plain text", OldAppCommand.oneLine("plain text"), "plain");
        same("", OldAppCommand.oneLine(""), "empty");
        same("日本語 ラベル", OldAppCommand.oneLine("日本語\tラベル"), "non-ASCII text is kept");
    }

    public static void main(String[] args) {
        eligibility();
        listing();
        switching();
        usage();
        oneLine();
        if (failures != 0) {
            System.out.println(failures + " failure(s)");
            System.exit(1);
        }
        System.out.println("PASS");
    }
}
