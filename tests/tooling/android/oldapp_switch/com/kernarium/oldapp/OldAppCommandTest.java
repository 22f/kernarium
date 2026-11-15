// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Old app compatibility shell command (android/lineage-24/overlay/oldapp_switch) against a fake
// platform, on a plain JVM: the eligibility rule, the listing's format, the stored value that names
// an installation, the cleanup and its lock, the writes, the order of the stop after the write and the
// exit statuses. The package is the command's, so that the test reaches
// what the command keeps to itself. test_oldapp_switch_command.py compiles and runs it.

package com.kernarium.oldapp;

import java.io.PrintWriter;
import java.io.StringWriter;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.ReentrantLock;

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
    private static final long FIRST_INSTALL = 50_000L;

    private static Candidate app(String name, boolean system, String abi, int target, String sharedUser,
            long updated) {
        return new Candidate(name, system, abi, target, sharedUser, FIRST_INSTALL, updated, name);
    }

    /** An eligible installation first installed at the given epoch milliseconds. */
    private static Candidate installedAt(String name, long firstInstallMillis) {
        return new Candidate(name, false, "armeabi-v7a", 10, null, firstInstallMillis, firstInstallMillis / 1000, name);
    }

    /** The stored value of a choice made at a time, for the installation first installed at another. */
    private static String choice(long turnedOnSeconds, long firstInstallMillis) {
        return turnedOnSeconds + " " + firstInstallMillis;
    }

    private static String letters(int count) {
        StringBuilder text = new StringBuilder();
        for (int i = 0; i < count; i++) {
            text.append('a');
        }
        return text.toString();
    }

    private static String key(String packageName) {
        return OldAppEligibility.propertyName(packageName);
    }

    private static final class Fake implements OldAppCommand.Platform {
        final List<Candidate> installed = new ArrayList<>();
        final Map<String, String> labels = new HashMap<>();
        final Map<String, String> properties = new ConcurrentHashMap<>();
        final List<String> events = Collections.synchronizedList(new ArrayList<String>());
        final ReentrantLock monitor = new ReentrantLock();
        boolean keepWrites = true;
        boolean lockTimesOut;
        boolean lockFails;
        int locksTaken;
        int locksHeld;
        int writesOutsideTheLock;
        /** The list command stops after its snapshot of the choices until the test lets it go on. */
        CountDownLatch snapshotTaken;
        CountDownLatch resume;
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
        public Map<String, String> storedChoices() throws Exception {
            Map<String, String> choices = new HashMap<>();
            for (Map.Entry<String, String> property : properties.entrySet()) {
                if (property.getKey().startsWith(OldAppEligibility.STORE_PREFIX) && !property.getValue().isEmpty()) {
                    choices.put(property.getKey().substring(OldAppEligibility.STORE_PREFIX.length()), property.getValue());
                }
            }
            if (snapshotTaken != null) {
                snapshotTaken.countDown();
                if (!resume.await(10, TimeUnit.SECONDS)) {
                    throw new IllegalStateException("the test did not let the listing go on");
                }
            }
            return choices;
        }

        @Override
        public OldAppCommand.Lock lock(long timeoutMillis) throws Exception {
            if (lockFails) {
                throw new IllegalStateException("cannot create the lock file");
            }
            if (lockTimesOut || !monitor.tryLock(Math.min(timeoutMillis, 2000), TimeUnit.MILLISECONDS)) {
                return null;
            }
            locksTaken++;
            locksHeld++;
            return new OldAppCommand.Lock() {
                @Override
                public void release() {
                    locksHeld--;
                    monitor.unlock();
                }
            };
        }

        @Override
        public void setProperty(String name, String value) {
            if (!monitor.isHeldByCurrentThread()) {
                writesOutsideTheLock++;
            }
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
        fake.properties.put("persist.sys.kernarium.oldapp.org.example.zed", choice(1699999000L, FIRST_INSTALL));
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
        // The whole key is bounded: 29 bytes of prefix and at most 226 of name.
        check(OldAppEligibility.isPropertyKeyPart(letters(226)), "226 characters make a 255-byte key");
        check(!OldAppEligibility.isPropertyKeyPart(letters(227)), "227 characters make a 256-byte key");
        check(OldAppEligibility.isPropertyKeyPart(letters(113) + "." + letters(112)), "dots count in the bound");
        check(!OldAppEligibility.isPropertyKeyPart(letters(113) + "." + letters(113)), "dots count in the bound, over");
        check(!OldAppEligibility.isPropertyKeyPart(letters(100_000)), "a huge name is refused without the pattern");
        same(255, OldAppEligibility.STORE_PREFIX.length() + 226, "the prefix and the longest name");
        check(OldAppEligibility.isEligible(app(letters(226), false, "armeabi-v7a", 10, null, 0), ABIS32), "eligible at the bound");
        check(!OldAppEligibility.isEligible(app(letters(227), false, "armeabi-v7a", 10, null, 0), ABIS32), "not eligible over it");
    }

    /** outlived: a stored choice belongs to the installation whose first-install time equals the stored one. */
    private static void installations() {
        Candidate current = installedAt("a.b", 1_000_007L);
        same(false, OldAppEligibility.outlived(current, ABIS32, choice(1000L, 1_000_007L)), "the same installation");
        same(false, OldAppEligibility.outlived(current, ABIS32, choice(5L, 1_000_007L)),
                "the turn-on time is not compared with anything");
        same(false, OldAppEligibility.outlived(current, ABIS32, choice(2_000_000_000L, 1_000_007L)),
                "not even a turn-on time in the future");
        same(true, OldAppEligibility.outlived(current, ABIS32, choice(1000L, 1_000_008L)), "another millisecond");
        same(true, OldAppEligibility.outlived(current, ABIS32, choice(1000L, 999_000L)), "an earlier installation");
        same(true, OldAppEligibility.outlived(current, ABIS32, choice(1000L, 5_000_000L)), "a later installation");
        for (String value : new String[] {"1700000000", "on", "", "1 ", " 1 2", "1  2", "1 2 3", "-1 5", "1 -5", "a b",
                "1\t2", "1 2\n", "9999999999999999999 5", "5 9999999999999999999"}) {
            same(true, OldAppEligibility.outlived(current, ABIS32, value), "a value that names no installation: <" + value + ">");
        }
        same("1000 1000007", OldAppEligibility.choiceValue(1000L, current), "the stored form");
        OldAppEligibility.Choice read = OldAppEligibility.parseChoice("1000 1000007");
        check(read != null && read.turnedOnSeconds == 1000L && read.installationMillis == 1_000_007L, "read back");
        same(true, OldAppEligibility.outlived(null, ABIS32, choice(1000L, 1_000_007L)), "not installed");
        same(true, OldAppEligibility.outlived(app("a.b", false, "armeabi-v7a", 14, null, 0), ABIS32,
                choice(1000L, FIRST_INSTALL)), "target raised");
        same(true, OldAppEligibility.outlived(new Candidate("a.b", false, "armeabi-v7a", 10, "uid", 1_000_007L, 1L, "a.b"),
                ABIS32, choice(1000L, 1_000_007L)), "shared user ID");
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
        same(Arrays.asList("set persist.sys.kernarium.oldapp.org.example.alpha=" + choice(1700000000L, FIRST_INSTALL),
                "stop org.example.alpha"),
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
        same(Arrays.asList("set persist.sys.kernarium.oldapp.org.example.alpha=" + choice(1700000000L, FIRST_INSTALL)),
                dropped.events,
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

    /** The cases of the amendments: a choice does not outlive the installation it was made for. */
    private static void lifecycle() {
        // Codex's first sequence: enabled at second 1000, the clock goes back, the package is uninstalled and
        // installed again at second 500. Another installation: cleared, whatever the clock says.
        Fake back = new Fake();
        back.installed.add(installedAt("org.example.again", 500_003L));
        back.properties.put(key("org.example.again"), choice(1000L, 1_000_007L));
        Result result = run(back, "list");
        same("kernarium_oldapp 1\norg.example.again\torg.example.again\toff\t0\t500\n", result.out,
                "a reinstall after the clock went back is listed off");
        same(Arrays.asList("set " + key("org.example.again") + "="), back.events, "and its choice is cleared");
        same("", back.getProperty(key("org.example.again")), "the property is empty");
        back.events.clear();
        run(back, "list");
        same(0, back.events.size(), "a second listing writes nothing");

        // The second sequence: enabled at second 101, reinstalled within the same second.
        Fake same = new Fake();
        same.installed.add(installedAt("org.example.again", 101_900L));
        same.properties.put(key("org.example.again"), choice(101L, 101_250L));
        check(run(same, "list").out.contains("org.example.again\torg.example.again\toff\t0\t101\n"),
                "a reinstall in the same second is another installation");
        same("", same.getProperty(key("org.example.again")), "and its choice is cleared");

        // The third: the same installation, the clock corrected backwards, then a new enable.
        Fake corrected = new Fake();
        corrected.installed.add(installedAt("org.example.zone", 1_000_007L));
        corrected.now = 900L;
        same(0, run(corrected, "set", "org.example.zone", "on").status, "enable after the clock went back");
        same(choice(900L, 1_000_007L), corrected.getProperty(key("org.example.zone")), "the value names the installation");
        corrected.events.clear();
        same("kernarium_oldapp 1\norg.example.zone\torg.example.zone\ton\t900\t1000\n", run(corrected, "list").out,
                "the new choice is kept and its turn-on time is the one the person made");
        same(0, corrected.events.size(), "and nothing is cleared");

        // An in-place update keeps the first-install time (the last update moves): the choice stays.
        Fake updated = new Fake();
        updated.installed.add(new Candidate("org.example.upd", false, "armeabi-v7a", 10, null, 1_000_007L, 77_777L, "x"));
        updated.properties.put(key("org.example.upd"), choice(1000L, 1_000_007L));
        same("kernarium_oldapp 1\norg.example.upd\torg.example.upd\ton\t1000\t77777\n", run(updated, "list").out,
                "an update keeps the choice");
        same(0, updated.events.size(), "and writes nothing");

        // The earlier one-number form, and anything else that does not name an installation, is cleared.
        for (String value : new String[] {"1700000000", "on", "1 2 3"}) {
            Fake old = new Fake();
            old.installed.add(installedAt("org.example.old", 1_000_007L));
            old.properties.put(key("org.example.old"), value);
            same("kernarium_oldapp 1\norg.example.old\torg.example.old\toff\t0\t1000\n", run(old, "list").out,
                    "a value <" + value + "> is listed off");
            same("", old.getProperty(key("org.example.old")), "and cleared: <" + value + ">");
        }

        // An update that raised the target level, a reinstall with a shared user ID, a system application, an
        // application that is gone, a name that cannot be a property name: each choice is cleared and none is
        // listed; the one that still belongs to its installation stays.
        Fake gone = new Fake();
        gone.installed.add(new Candidate("org.example.raised", false, "armeabi-v7a", 14, null, 10_000L, 10L, "x"));
        gone.installed.add(new Candidate("org.example.shared", false, "armeabi-v7a", 10, "org.example.uid", 10_000L, 10L, "x"));
        gone.installed.add(new Candidate("org.example.system", true, "armeabi-v7a", 10, null, 10_000L, 10L, "x"));
        gone.installed.add(new Candidate("org.example.sixtyfour", false, "arm64-v8a", 10, null, 10_000L, 10L, "x"));
        gone.installed.add(new Candidate("org.example.keeps", false, "armeabi-v7a", 10, null, 10_000L, 10L, "x"));
        for (String name : new String[] {"org.example.raised", "org.example.shared", "org.example.system",
                "org.example.sixtyfour", "org.example.absent", "org.example..odd", "org.example.keeps"}) {
            gone.properties.put(key(name), choice(1700000000L, 10_000L));
        }
        Result cleaned = run(gone, "list");
        same(0, cleaned.status, "cleanup status");
        same("kernarium_oldapp 1\norg.example.keeps\torg.example.keeps\ton\t1700000000\t10\n", cleaned.out,
                "only the choice that still belongs to its application stays");
        same(6, gone.events.size(), "six choices are cleared");
        for (String event : gone.events) {
            check(event.startsWith("set ") && event.endsWith("="), "a clearing write: " + event);
        }
        same(choice(1700000000L, 10_000L), gone.getProperty(key("org.example.keeps")), "the other choice is not touched");
        same(0, gone.writesOutsideTheLock, "every write was under the lock");
        same(0, gone.locksHeld, "and the lock is released");

        // A choice that cannot be cleared is not hidden: the listing fails and prints nothing.
        Fake stuck = new Fake();
        stuck.installed.add(installedAt("org.example.again", 500_003L));
        stuck.properties.put(key("org.example.again"), choice(1000L, 1_000_007L));
        stuck.keepWrites = false;
        Result failed = run(stuck, "list");
        same(1, failed.status, "a choice that stays fails the listing");
        same("", failed.out, "and prints no listing");
        check(failed.err.contains("did not keep"), "and says why: " + failed.err);
        same(0, stuck.locksHeld, "the lock is released after the failure");
    }

    /** list and set are serialised, and the cleanup clears only the value it examined. */
    private static void locking() throws Exception {
        // Codex's interleaving. The listing takes its snapshot of the choices (a stale value for the current
        // installation, first installed at 200 s) and stops; another command enables the installation; the
        // listing goes on, finds its saved value stale and must not erase the new choice.
        final Fake fake = new Fake();
        fake.installed.add(installedAt("org.example.race", 200_000L));
        fake.properties.put(key("org.example.race"), choice(100L, 100_000L));
        fake.snapshotTaken = new CountDownLatch(1);
        fake.resume = new CountDownLatch(1);
        final Result[] listed = new Result[1];
        Thread lister = new Thread(new Runnable() {
            @Override
            public void run() {
                listed[0] = OldAppCommandTest.run(fake, "list");
            }
        });
        lister.start();
        check(fake.snapshotTaken.await(10, TimeUnit.SECONDS), "the listing reached its snapshot");
        fake.now = 500L;
        Result enabled = run(fake, "set", "org.example.race", "on");
        same(0, enabled.status, "the enable succeeded meanwhile");
        fake.resume.countDown();
        lister.join(10_000);
        check(!lister.isAlive(), "the listing finished");
        same(0, listed[0].status, "the listing succeeded");
        same(choice(500L, 200_000L), fake.getProperty(key("org.example.race")), "the new choice is still there");
        check(listed[0].out.contains("org.example.race\torg.example.race\ton\t500\t200\n"),
                "and the listing shows it on: " + listed[0].out);
        check(!fake.events.contains("set " + key("org.example.race") + "="), "no clearing write was made");
        same(0, fake.writesOutsideTheLock, "every write was under the lock");
        same(0, fake.locksHeld, "the lock is released");

        // The same snapshot when the value changed to another stale one: it is not the value examined and stays.
        final Fake other = new Fake();
        other.installed.add(installedAt("org.example.race", 200_000L));
        other.properties.put(key("org.example.race"), choice(100L, 100_000L));
        other.snapshotTaken = new CountDownLatch(1);
        other.resume = new CountDownLatch(1);
        final Result[] second = new Result[1];
        Thread again = new Thread(new Runnable() {
            @Override
            public void run() {
                second[0] = OldAppCommandTest.run(other, "list");
            }
        });
        again.start();
        check(other.snapshotTaken.await(10, TimeUnit.SECONDS), "the second listing reached its snapshot");
        other.properties.put(key("org.example.race"), choice(150L, 150_000L));
        other.resume.countDown();
        again.join(10_000);
        same(choice(150L, 150_000L), other.getProperty(key("org.example.race")), "a changed value is not the examined one");
        check(second[0].out.contains("org.example.race\torg.example.race\toff\t0\t200\n"),
                "and the listing shows it off: " + second[0].out);

        // A set waits for a listing's lock and then goes on.
        Fake held = new Fake();
        held.installed.add(installedAt("org.example.race", 200_000L));
        check(held.monitor.tryLock(), "the test holds the lock");
        final Result[] waiting = new Result[1];
        final Fake heldRef = held;
        Thread setter = new Thread(new Runnable() {
            @Override
            public void run() {
                waiting[0] = OldAppCommandTest.run(heldRef, "set", "org.example.race", "on");
            }
        });
        setter.start();
        Thread.sleep(300);
        same(0, held.events.size(), "nothing is written while another process holds the lock");
        held.monitor.unlock();
        setter.join(10_000);
        same(0, waiting[0].status, "the set goes on once the lock is free");
        same(2, held.events.size(), "the write and the stop were made after the wait");
    }

    /** A lock that cannot be had: set fails and changes nothing, the listing lists without cleaning. */
    private static void lockUnavailable() {
        for (int how = 0; how < 2; how++) {
            Fake fake = fixture();
            fake.installed.add(installedAt("org.example.again", 500_003L));
            fake.properties.put(key("org.example.again"), choice(1000L, 1_000_007L));
            if (how == 0) {
                fake.lockTimesOut = true;
            } else {
                fake.lockFails = true;
            }
            for (String state : new String[] {"on", "off"}) {
                fake.events.clear();
                Result result = run(fake, "set", "org.example.zed", state);
                same(1, result.status, "set " + state + " without the lock (" + how + ")");
                same("", result.out, "no success line without the lock");
                check(result.err.length() > 0, "and the reason is said");
                same(0, fake.events.size(), "nothing is written or stopped without the lock");
            }
            Result listed = run(fake, "list");
            same(0, listed.status, "the listing succeeds without the lock (" + how + ")");
            same(0, fake.events.size(), "and cleans nothing");
            same(choice(1000L, 1_000_007L), fake.getProperty(key("org.example.again")), "the stale choice stays");
            // The same lines as with the lock.
            Fake clean = fixture();
            clean.installed.add(installedAt("org.example.again", 500_003L));
            clean.properties.put(key("org.example.again"), choice(1000L, 1_000_007L));
            same(run(clean, "list").out, listed.out, "the listing says nothing different on stdout (" + how + ")");
            check(listed.out.contains("org.example.again\torg.example.again\toff\t0\t500\n"), "the package is listed off");
        }
        // A usage error or a refusal needs no lock.
        Fake fake = fixture();
        fake.lockTimesOut = true;
        same(2, run(fake, "set", "org.example..oldapp", "off").status, "a refusal of the name needs no lock");
        same(64, run(fake, "set", "org.example.alpha").status, "usage needs no lock");
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
        fake.properties.put(key("org.example.gone"), choice(1700000000L, 10_000L));
        Result gone = run(fake, "set", "org.example.gone", "off");
        same(0, gone.status, "off for a package that is gone");
        same(Arrays.asList("set " + key("org.example.gone") + "="), fake.events, "only the property is cleared");

        // Installed and not eligible any more: cleared and stopped.
        fake.events.clear();
        fake.properties.put(key("org.example.modern"), choice(1700000000L, FIRST_INSTALL));
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
        same(0, dropped.locksHeld, "and the lock is released");

        // The bound of the name: 226 characters are a legal key, 227 are not.
        Fake bound = fixture();
        same(0, run(bound, "set", letters(226), "off").status, "off at the bound");
        same(2, run(bound, "set", letters(227), "off").status, "off over the bound");
        same(2, run(bound, "set", letters(227), "on").status, "on over the bound");
        same(2, run(bound, "set", letters(100_000), "off").status, "a huge name");
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

    public static void main(String[] args) throws Exception {
        eligibility();
        installations();
        listing();
        switching();
        usage();
        names();
        lifecycle();
        locking();
        lockUnavailable();
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
