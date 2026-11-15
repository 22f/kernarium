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

    /** The first install is before every turn-on of the fixtures unless a test says otherwise. */
    private static final long FIRST_INSTALL = 50L;

    private static Candidate app(String name, boolean system, String abi, int target, String sharedUser,
            long updated) {
        return new Candidate(name, system, abi, target, sharedUser, FIRST_INSTALL, updated, name);
    }

    private static Candidate installedAt(String name, long firstInstall) {
        return new Candidate(name, false, "armeabi-v7a", 10, null, firstInstall, firstInstall, name);
    }

    private static String key(String packageName) {
        return OldAppEligibility.propertyName(packageName);
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
        public Map<String, String> storedChoices() {
            Map<String, String> choices = new HashMap<>();
            for (Map.Entry<String, String> property : properties.entrySet()) {
                if (property.getKey().startsWith(OldAppEligibility.STORE_PREFIX) && !property.getValue().isEmpty()) {
                    choices.put(property.getKey().substring(OldAppEligibility.STORE_PREFIX.length()), property.getValue());
                }
            }
            return choices;
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
        // A name the property service cannot take as it is makes the package not eligible.
        for (String good : new String[] {"a.b", "org.example.Foo_1", "com.a1.b2", "single", "1a.b", "a-b.c", "a@b:c"}) {
            check(OldAppEligibility.isPropertyKeyPart(good), "property key part accepted: " + good);
            check(OldAppEligibility.isEligible(app(good, false, "armeabi-v7a", 10, null, 0), ABIS32),
                    "eligible by its name: " + good);
        }
        for (String bad : new String[] {"", ".a", "a.", "a..b", "a b", "a.b/c", "a.b\n", "../x", "a.b;c",
                "org.example..oldapp", "org.example.oldapp."}) {
            check(!OldAppEligibility.isPropertyKeyPart(bad), "property key part refused: <" + bad + ">");
            check(!OldAppEligibility.isEligible(app(bad, false, "armeabi-v7a", 10, null, 0), ABIS32),
                    "not eligible by its name: <" + bad + ">");
        }
        // outlived: the rule for a stored turn-on.
        Candidate current = installedAt("a.b", 100L);
        same(false, OldAppEligibility.outlived(current, ABIS32, "100"), "installed in the second of the turn-on");
        same(false, OldAppEligibility.outlived(current, ABIS32, "101"), "installed before the turn-on");
        same(true, OldAppEligibility.outlived(current, ABIS32, "99"), "installed after the turn-on");
        same(false, OldAppEligibility.outlived(current, ABIS32, "yesterday"), "a value that is not a time stays");
        same(true, OldAppEligibility.outlived(null, ABIS32, "101"), "not installed");
        same(true, OldAppEligibility.outlived(app("a.b", false, "armeabi-v7a", 14, null, 0), ABIS32, "101"), "target raised");
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

        // On is refused for an application that is not eligible or not installed.
        for (String refused : new String[] {"org.example.modern", "org.example.sixty_four", "org.example.javaonly",
                "org.example.system", "org.example.shared", "org.example.unknown"}) {
            fake.events.clear();
            Result result = run(fake, "set", refused, "on");
            same(2, result.status, "refused status for " + refused);
            same("", result.out, "refused stdout for " + refused);
            same(1, result.err.split("\n").length, "one line on stderr for " + refused);
            check(result.err.contains(refused), "the refusal names " + refused + ": " + result.err);
            same(0, fake.events.size(), "a refusal writes and stops nothing: " + refused);
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
            {"set", "org.example.alpha", "ON"}, {"set", "--help"},
            {"set", "org.example.alpha", "on", "extra"}, {"LIST"}, {"get", "org.example.alpha"},
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

    /** A name that cannot be a property name is not usage but a refusal, for on and for off. */
    private static void names() {
        for (String name : new String[] {"", ".a", "a.", "a..b", "a b", "../x", "a.b\n", "a.b;c",
                "org.example..oldapp", "org.example.oldapp."}) {
            for (String state : new String[] {"on", "off"}) {
                Fake fake = fixture();
                Result result = run(fake, "set", name, state);
                same(2, result.status, "refused status for <" + name + "> " + state);
                same("", result.out, "refused stdout for <" + name + "> " + state);
                same(0, fake.events.size(), "nothing is written for <" + name + "> " + state);
            }
        }
        // The reviewer's two installed names are not listed either.
        Fake fake = fixture();
        fake.installed.add(app("org.example..oldapp", false, "armeabi-v7a", 10, null, 9L));
        fake.installed.add(app("org.example.oldapp.", false, "armeabi-v7a", 10, null, 9L));
        String listing = run(fake, "list").out;
        check(!listing.contains("org.example..oldapp") && !listing.contains("org.example.oldapp."),
                "unrepresentable names are not listed: " + listing);
    }

    /** The cases of the amendment: a choice does not outlive the installation it was made for. */
    private static void lifecycle() {
        // Uninstalled and installed again: first installed after the turn-on time.
        Fake fake = new Fake();
        fake.installed.add(installedAt("org.example.again", 1700000500L));
        fake.properties.put(key("org.example.again"), "1700000000");
        Result result = run(fake, "list");
        same("kernarium_oldapp 1\norg.example.again\torg.example.again\toff\t0\t1700000500\n", result.out,
                "a reinstalled application is listed off");
        same(Arrays.asList("set " + key("org.example.again") + "="), fake.events, "and its choice is cleared");
        same("", fake.getProperty(key("org.example.again")), "the property is empty");
        fake.events.clear();
        run(fake, "list");
        same(0, fake.events.size(), "a second listing writes nothing");

        // An update that raised the target level, a reinstall with a shared user ID, a system
        // application, an application that is gone, a name that cannot be a property name: each
        // choice is cleared and none is listed.
        Fake gone = new Fake();
        gone.installed.add(new Candidate("org.example.raised", false, "armeabi-v7a", 14, null, 10L, 10L, "x"));
        gone.installed.add(new Candidate("org.example.shared", false, "armeabi-v7a", 10, "org.example.uid", 10L, 10L, "x"));
        gone.installed.add(new Candidate("org.example.system", true, "armeabi-v7a", 10, null, 10L, 10L, "x"));
        gone.installed.add(new Candidate("org.example.sixtyfour", false, "arm64-v8a", 10, null, 10L, 10L, "x"));
        gone.installed.add(new Candidate("org.example.keeps", false, "armeabi-v7a", 10, null, 10L, 10L, "x"));
        for (String name : new String[] {"org.example.raised", "org.example.shared", "org.example.system",
                "org.example.sixtyfour", "org.example.absent", "org.example..odd", "org.example.keeps"}) {
            gone.properties.put(key(name), "1700000000");
        }
        Result cleaned = run(gone, "list");
        same(0, cleaned.status, "cleanup status");
        same("kernarium_oldapp 1\norg.example.keeps\torg.example.keeps\ton\t1700000000\t10\n", cleaned.out,
                "only the choice that still belongs to its application stays");
        same(6, gone.events.size(), "six choices are cleared");
        for (String event : gone.events) {
            check(event.startsWith("set ") && event.endsWith("="), "a clearing write: " + event);
        }
        same("1700000000", gone.getProperty(key("org.example.keeps")), "the other choice is not touched");
        for (String name : new String[] {"org.example.raised", "org.example.absent", "org.example..odd"}) {
            same("", gone.getProperty(key(name)), "cleared: " + name);
        }

        // A choice that cannot be cleared is not hidden: the listing fails and prints nothing.
        Fake stuck = new Fake();
        stuck.installed.add(installedAt("org.example.again", 1700000500L));
        stuck.properties.put(key("org.example.again"), "1700000000");
        stuck.keepWrites = false;
        Result failed = run(stuck, "list");
        same(1, failed.status, "a choice that stays fails the listing");
        same("", failed.out, "and prints no listing");
        check(failed.err.contains("did not keep"), "and says why: " + failed.err);

        // A value that is not a time is kept and listed on.
        Fake odd = new Fake();
        odd.installed.add(installedAt("org.example.odd", 1700000500L));
        odd.properties.put(key("org.example.odd"), "on");
        check(run(odd, "list").out.contains("org.example.odd\torg.example.odd\ton\ton\t1700000500\n"),
                "a value that is not a time stays on");
        same(0, odd.events.size(), "and is not cleared");
    }

    /** Turning off is accepted for any name that can be a property name. */
    private static void turningOff() {
        // Not installed, nothing stored: done, and no property is made for it.
        Fake fake = fixture();
        Result absent = run(fake, "set", "org.example.never", "off");
        same(0, absent.status, "off for a package that is not installed");
        same("org.example.never\toff\n", absent.out, "its line");
        same(0, fake.events.size(), "nothing is written or stopped");

        // Not installed, a choice stored: cleared, nothing to stop.
        fake.properties.put(key("org.example.gone"), "1700000000");
        Result gone = run(fake, "set", "org.example.gone", "off");
        same(0, gone.status, "off for a package that is gone");
        same(Arrays.asList("set " + key("org.example.gone") + "="), fake.events, "only the property is cleared");

        // Installed and not eligible any more: cleared and stopped.
        fake.events.clear();
        fake.properties.put(key("org.example.modern"), "1700000000");
        Result modern = run(fake, "set", "org.example.modern", "off");
        same(0, modern.status, "off for an application that is not eligible");
        same("org.example.modern\toff\n", modern.out, "its line");
        same(Arrays.asList("set " + key("org.example.modern") + "=", "stop org.example.modern"), fake.events,
                "cleared, then stopped");

        // Installed and never switched on: stopped only.
        fake.events.clear();
        Result idle = run(fake, "set", "org.example.alpha", "off");
        same(0, idle.status, "off for an application that is off");
        same(Arrays.asList("stop org.example.alpha"), fake.events, "nothing to clear, so it is only stopped");

        // A write the system does not keep is a failure.
        Fake dropped = fixture();
        dropped.keepWrites = false;
        same(1, run(dropped, "set", "org.example.zed", "off").status, "a clearing the system did not keep");
    }

    /** The platform is made after the arguments are judged. */
    private static void factory() {
        final int[] made = {0};
        OldAppCommand.PlatformFactory broken = () -> {
            made[0]++;
            throw new IllegalStateException("the package manager is not running");
        };
        StringWriter err = new StringWriter();
        StringWriter out = new StringWriter();
        int status = OldAppCommand.run(new String[] {"set", "x"}, broken, new PrintWriter(out), new PrintWriter(err));
        same(64, status, "a usage error is 64 although there is no package manager");
        same(0, made[0], "and the platform was not made");
        status = OldAppCommand.run(new String[] {"list"}, broken, new PrintWriter(out), new PrintWriter(err));
        same(1, status, "a good command fails with 1 without the package manager");
        same(1, made[0], "the platform was asked for once");
        check(err.toString().contains("the package manager is not running"), "and the reason is said: " + err);
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
        names();
        lifecycle();
        turningOff();
        factory();
        oneLine();
        if (failures != 0) {
            System.out.println(failures + " failure(s)");
            System.exit(1);
        }
        System.out.println("PASS");
    }
}
