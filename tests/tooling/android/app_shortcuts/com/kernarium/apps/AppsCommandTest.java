// SPDX-License-Identifier: GPL-2.0-or-later
//
// The launcher-application shell command (android/lineage-24/overlay/app_shortcuts) against a fake
// platform, on a plain JVM: the component grammar, the label's escaping and bounds, the listing's
// rule (order, filters, bounds, digest and icon keys), the framing of every answer read back by a
// parser of the test's own, the icon records (statuses, size steps, the PNG bound, the deadline),
// the launch's refusals and the argument vector that reaches the process, how a start's output is read, the
// child process's drain, deadline and reaping (real processes), the icon source's bound, the resource files'
// order, and the exit statuses.
// Each row prints "ok <name>" or "FAIL <name>: why"; the last line is PASS when no row failed.
// The package is the command's, so that the test reaches what the command keeps to itself.
// test_app_shortcuts_command.py compiles and runs it, and runs it again on sources with one rule
// removed at a time, where the rows named there must fail.

package com.kernarium.apps;

import java.io.ByteArrayOutputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

public final class AppsCommandTest {
    private static int failures;
    private static String current;

    /** An assertion that failed in the current row. */
    private static final class Mismatch extends RuntimeException {
        private static final long serialVersionUID = 1L;

        Mismatch(String message) {
            super(message);
        }
    }

    private interface Row {
        void run() throws Exception;
    }

    private static void row(String name, Row body) {
        current = name;
        try {
            body.run();
            System.out.println("ok " + name);
        } catch (Mismatch problem) {
            failures++;
            System.out.println("FAIL " + name + ": " + problem.getMessage());
        } catch (Throwable problem) {
            failures++;
            System.out.println("FAIL " + name + ": " + problem);
        }
    }

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new Mismatch(what);
        }
    }

    private static void same(Object expected, Object actual, String what) {
        check(expected.equals(actual), what + ": expected <" + show(expected) + "> but got <" + show(actual) + ">");
    }

    private static String show(Object value) {
        return String.valueOf(value).replace("\n", "\\n").replace("\t", "\\t");
    }

    // ---------------------------------------------------------------------------------------------
    // The fake platform and the answers' reader.

    private static final byte[] PNG_HEAD = AppsCommand.PNG_SIGNATURE;

    /** A PNG-looking byte string of exactly {@code length} bytes (the commands check the signature, not the image). */
    private static byte[] png(int length, int seed) {
        byte[] bytes = new byte[length];
        for (int i = 0; i < length; i++) {
            bytes[i] = i < PNG_HEAD.length ? PNG_HEAD[i] : (byte) (seed + i * 31);
        }
        return bytes;
    }

    private interface IconSource {
        Icon icon(Entry entry, int size) throws Exception;
    }

    private static class Fake implements AppsCommand.Platform {
        int user = 0;
        List<Entry> entries = new ArrayList<>();
        List<String> locales = new ArrayList<>(Arrays.asList("en-US", "ja-JP"));
        Map<String, String> labels = new HashMap<>();
        Set<String> labelFails = new HashSet<>();
        IconSource icons = (entry, size) -> new Icon(size, true, false, png(300, 1));
        AppsCommand.StartResult startResult = new AppsCommand.StartResult(0, "Starting: Intent { }\n");
        final List<List<String>> started = new ArrayList<>();
        long clock;
        long clockStep;
        long renderLimit = AppsCommand.ICON_RENDER_MILLIS;
        final List<Integer> iconSizes = new ArrayList<>();

        Fake with(Entry... more) {
            entries.addAll(Arrays.asList(more));
            return this;
        }

        @Override
        public int currentUser() throws Exception {
            return user;
        }

        @Override
        public List<Entry> launcherActivities(int forUser) throws Exception {
            check(forUser == user, "the listing is asked for the foreground user");
            return new ArrayList<>(entries);
        }

        @Override
        public List<String> localeTags() throws Exception {
            return locales;
        }

        @Override
        public String label(Entry entry) throws Exception {
            if (labelFails.contains(entry.packageName)) {
                throw new IllegalStateException("no resources");
            }
            return labels.get(entry.packageName + "/" + entry.activity);
        }

        @Override
        public Icon icon(Entry entry, int size) throws Exception {
            iconSizes.add(size);
            return icons.icon(entry, size);
        }

        @Override
        public AppsCommand.StartResult start(int forUser, Component component) {
            started.add(AppsCommand.startArguments(forUser, component));
            return startResult;
        }

        @Override
        public long nowMillis() {
            long now = clock;
            clock += clockStep;
            return now;
        }

        @Override
        public long renderLimitMillis() {
            return renderLimit;
        }
    }

    private static Entry app(String packageName, String activity) {
        return new Entry(packageName, activity, false, true, true, true, 10, 1_000, 0x7f010000, "", null);
    }

    private static Entry builtin(String packageName, String activity) {
        return new Entry(packageName, activity, true, true, true, true, 10, 1_000, 0x7f010000, "", null);
    }

    private static Entry entry(String packageName, String activity, boolean builtin, boolean exported,
            boolean enabled, boolean permitted, long version, long updated, int icon) {
        return new Entry(packageName, activity, builtin, exported, enabled, permitted, version, updated, icon, "", null);
    }

    /** What one run of the command gave. */
    private static final class Result {
        int status;
        byte[] out;
        String err;
        boolean platformMade;

        String text() {
            return new String(out, StandardCharsets.UTF_8);
        }
    }

    private static Result run(Fake platform, String... args) {
        Result result = new Result();
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        StringWriter err = new StringWriter();
        PrintWriter errors = new PrintWriter(err);
        result.status = AppsCommand.run(args, () -> {
            result.platformMade = true;
            return platform;
        }, out, errors);
        errors.flush();
        result.out = out.toByteArray();
        result.err = err.toString();
        return result;
    }

    /** The lines of a text answer. */
    private static List<String> lines(Result result) {
        String text = result.text();
        check(text.isEmpty() || text.endsWith("\n"), "every line ends with LF");
        List<String> lines = new ArrayList<>();
        for (String line : text.split("\n", -1)) {
            lines.add(line);
        }
        if (!lines.isEmpty() && lines.get(lines.size() - 1).isEmpty()) {
            lines.remove(lines.size() - 1);
        }
        return lines;
    }

    /** A listing as the host reads it: strictly, every shape checked. */
    private static final class Parsed {
        final Map<String, String> header = new HashMap<>();
        final List<String[]> rows = new ArrayList<>();
    }

    private static Parsed parseList(Result result, boolean withRows) {
        same(0, result.status, "exit status");
        check(result.out.length > 0, "an answer");
        check(!result.text().contains("\r"), "no CR in the answer");
        List<String> lines = lines(result);
        same("kernarium_apps 1", lines.get(0), "first line");
        Parsed parsed = new Parsed();
        int i = 1;
        String[] keys = {"digest", "count", "skipped", "locales", "user"};
        for (String key : keys) {
            String[] parts = lines.get(i++).split("\t", -1);
            same(2, parts.length, "header line " + key);
            same(key, parts[0], "header key");
            parsed.header.put(key, parts[1]);
        }
        check(parsed.header.get("digest").matches("[0-9a-f]{64}"), "digest is 64 hex");
        if (!withRows) {
            same(i, lines.size(), "the digest answer has nothing after the header");
            return parsed;
        }
        same("rows", lines.get(i++), "rows marker");
        int count = Integer.parseInt(parsed.header.get("count"));
        for (int r = 0; r < count; r++) {
            String[] fields = lines.get(i++).split("\t", -1);
            same(5, fields.length, "row " + r + " has five fields");
            check(Component.of(fields[0], fields[1]) != null, "row " + r + " names a component");
            check(fields[3].equals("0") || fields[3].equals("1"), "builtin flag");
            check(fields[4].matches("[0-9a-f]{16}"), "icon key");
            check(fields[2].length() > 0, "label is not empty");
            parsed.rows.add(fields);
        }
        same("end\t" + count, lines.get(i++), "end line");
        same(i, lines.size(), "nothing after the end line");
        return parsed;
    }

    /** One record of an icons answer. */
    private static final class IconRecord {
        String token;
        String status;
        int width;
        int height;
        String kind;
        String alpha;
        int length;
        String sha;
        byte[] data;
    }

    private static List<IconRecord> parseIcons(Result result) throws Exception {
        same(0, result.status, "exit status");
        byte[] bytes = result.out;
        int[] at = {0};
        same("kernarium_apps 1", readLine(bytes, at), "first line");
        String[] head = readLine(bytes, at).split("\t", -1);
        same(2, head.length, "icons line");
        same("icons", head[0], "icons key");
        int count = Integer.parseInt(head[1]);
        List<IconRecord> records = new ArrayList<>();
        for (int r = 0; r < count; r++) {
            String[] f = readLine(bytes, at).split("\t", -1);
            same(9, f.length, "record header fields");
            same("icon", f[0], "record marker");
            IconRecord record = new IconRecord();
            record.token = f[1];
            check(Component.parse(f[1]) != null, "record names a component");
            record.status = f[2];
            record.width = Integer.parseInt(f[3]);
            record.height = Integer.parseInt(f[4]);
            record.kind = f[5];
            record.alpha = f[6];
            record.length = Integer.parseInt(f[7]);
            record.sha = f[8];
            if (record.status.equals("ok")) {
                check(at[0] + record.length + 1 <= bytes.length, "the data and its LF are all there");
                record.data = Arrays.copyOfRange(bytes, at[0], at[0] + record.length);
                at[0] += record.length;
                same((int) '\n', (int) bytes[at[0]++], "LF after the data");
                same(Listing.sha256Hex(record.data), record.sha, "digest of the data");
                check(AppsCommand.isPng(record.data), "PNG signature");
                same(record.width, record.height, "square");
                check(record.kind.equals("adaptive") || record.kind.equals("legacy"), "kind");
                check(record.alpha.equals("0") || record.alpha.equals("1"), "alpha flag");
                check(record.length <= AppsCommand.MAX_PNG_BYTES, "within the PNG bound");
            } else {
                same("0", String.valueOf(record.length), "no data");
                same("0", String.valueOf(record.width), "no width");
                same("-", record.kind, "no kind");
                same("-", record.sha, "no digest");
                check(Arrays.asList("missing", "noicon", "error", "deferred").contains(record.status),
                        "a known status: " + record.status);
            }
            records.add(record);
        }
        same("end\t" + count, readLine(bytes, at), "end line");
        same(bytes.length, at[0], "nothing after the end line");
        return records;
    }

    private static String readLine(byte[] bytes, int[] at) {
        int end = at[0];
        while (end < bytes.length && bytes[end] != '\n') {
            end++;
        }
        check(end < bytes.length, "a line ends with LF");
        String line = new String(bytes, at[0], end - at[0], StandardCharsets.UTF_8);
        at[0] = end + 1;
        return line;
    }

    private static String repeat(String text, int times) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < times; i++) {
            out.append(text);
        }
        return out.toString();
    }

    private static String sanitize(String raw) {
        return LabelText.sanitize(raw, "fallback.pkg");
    }

    // ---------------------------------------------------------------------------------------------

    public static void main(String[] args) {
        grammar();
        labels();
        listing();
        listAnswer();
        icons();
        launch();
        usageAndFailures();
        starts();
        children();
        sources();
        sizes();
        System.out.println(failures == 0 ? "PASS" : "FAILED " + failures);
        System.exit(failures == 0 ? 0 : 1);
    }

    private static void grammar() {
        row("component-grammar-accepts", () -> {
            String[][] good = {
                {"com.android.settings", "com.android.settings.Settings"},
                {"a.b", "A"},
                {"org.example.app_1", "org.example.app_1.Main$Launcher"},
                {"org.example.app", "_x.$y.Z9"},
                {"com.Example.APP", "com.Example.APP.MainActivity"},
            };
            for (String[] pair : good) {
                Component c = Component.parse(pair[0] + "/" + pair[1]);
                check(c != null, "accepted: " + pair[0] + "/" + pair[1]);
                same(pair[0], c.packageName, "package");
                same(pair[1], c.activity, "activity");
                same(pair[0] + "/" + pair[1], c.token(), "token");
            }
            check(Component.of(repeat("a", 127) + "." + repeat("b", 127), "A") != null, "package of 255 bytes");
            check(Component.of("a.b", repeat("A", 255)) != null, "activity of 255 bytes");
        });
        row("component-grammar-rejects", () -> {
            String[] bad = {
                "", "/", "a.b", "a.b/", "/A", "a/b", "singlesegment/Main", "-n/x", "a.b/c/d",
                "a.b/.Main", "a.b/Main.", "a.b/Ma..in", "a..b/Main", ".a.b/Main", "a.b./Main",
                "a.b/-Main", "--es.x/y", "a.b/9Main", "a.1b/Main", "a.b-c/Main",
                "a.b/Main'", "a.b/Main\"", "a.b/Main`", "a.b/Main;x", "a.b/Main|x", "a.b/Main&x", "a.b/Main<x",
                "a.b/Main>x", "a.b/Main(x)", "a.b/Main x", "a.b /Main", "a.b/Main\n", "a.b/Main\tx",
                "a.b/Main\r", "a.b/Main\u0000", "a.b/Main\\x", "a.b/Main$(id)", "a.b/${HOME}", "a.b/Main/x",
                "a.b/Ma\u00e9in", "a.\u00e9b/Main", "a.b/Main\u202e", "a.b/Main\uff0f", "a.b/Main%20",
                "a.b/Main#x", "a.b/Main=x", "a.b/Main:x", "a.b/Main*", "a.b/Main?", "a.b/Main~", "a.b/Main!",
                repeat("a", 128) + "." + repeat("b", 127) + "/Main",
                "a.b/" + repeat("A", 256),
            };
            for (String token : bad) {
                check(Component.parse(token) == null, "refused: <" + show(token) + ">");
            }
            check(Component.parse(null) == null, "null is refused");
            check(Component.of("a.b", null) == null, "null activity");
        });
    }

    private static void labels() {
        row("label-controls-become-space", () -> {
            same("a b c", sanitize("a\tb\nc"), "TAB and LF");
            same("a b", sanitize("a\r\nb"), "CRLF collapses");
            same("a b", sanitize("a\u000B\u000C\u0085\u2028\u2029b"), "VT FF NEL LS PS");
            same("a b", sanitize("a\u0001\u001F\u007F\u0080\u009Fb"), "C0 DEL C1");
            same("a b", sanitize("a\u00A0\u2003\u3000b"), "NBSP and other space characters");
            same("x", sanitize("  \t x \n "), "ends trimmed");
        });
        row("label-bidi-and-format-dropped", () -> {
            same("ab", sanitize("a\u202Eb"), "right-to-left override");
            same("ab", sanitize("\u202Aa\u202B\u202C\u202Db"), "embeddings and pop");
            same("ab", sanitize("a\u2066\u2067\u2068\u2069b"), "isolates");
            same("ab", sanitize("a\u200E\u200F\u061Cb"), "marks");
            same("ab", sanitize("\uFEFFa\u200Bb\u2060\u00AD"), "BOM, zero-width space, word joiner, soft hyphen");
            same("a\u200Db\u200Cc", sanitize("a\u200Db\u200Cc"), "the joiners stay");
        });
        row("label-surrogates-and-nonchars", () -> {
            same("a\uFFFDb", sanitize("a\uD800b"), "unpaired high surrogate");
            same("a\uFFFDb", sanitize("a\uDC00b"), "unpaired low surrogate");
            same("a\uFFFDb", sanitize("a\uFFFFb"), "non-character");
            same("a\uFFFDb", sanitize("a\uFDD0b"), "non-character in the Arabic block");
            same("a\uD83D\uDE00b", sanitize("a\uD83D\uDE00b"), "a paired surrogate is a character");
        });
        row("label-text-is-kept", () -> {
            same("Calculator", sanitize("Calculator"), "ASCII");
            same("\u8A08\u7B97\u6A5F \u2022 \u0645\u0631\u062D\u0628\u0627", sanitize("\u8A08\u7B97\u6A5F \u2022 \u0645\u0631\u062D\u0628\u0627"),
                    "CJK, Arabic, bullet");
            same("A B", sanitize("A \u0001 B"), "controls among spaces collapse to one");
            same("a b", sanitize("a    b"), "runs of spaces collapse");
        });
        row("label-bounds", () -> {
            String exact = repeat("x", 80);
            same(exact, sanitize(exact), "80 code points are kept");
            String longer = sanitize(repeat("x", 81));
            same(80, longer.codePointCount(0, longer.length()), "81 are cut to 80");
            check(longer.endsWith("\u2026"), "cut text ends with an ellipsis");
            check(sanitize(repeat("y", 5000)).length() <= 80, "very long text is cut");
            String emoji = repeat("\uD83D\uDE00", 80);
            same(emoji, sanitize(emoji), "80 four-byte characters are 320 bytes and kept");
            String cut = sanitize(repeat("\uD83D\uDE00", 200));
            check(cut.getBytes(StandardCharsets.UTF_8).length <= 320, "320 bytes at most");
            check(cut.codePointCount(0, cut.length()) <= 80, "80 code points at most");
            check(cut.endsWith("\u2026"), "ellipsis");
            String spaced = sanitize(repeat("ab ", 60));
            check(!spaced.contains("\u2026 ") && !spaced.endsWith(" \u2026"), "no space before the ellipsis");
        });
        row("label-empty-uses-the-package", () -> {
            same("fallback.pkg", LabelText.sanitize(null, "fallback.pkg"), "null");
            same("fallback.pkg", LabelText.sanitize("", "fallback.pkg"), "empty");
            same("fallback.pkg", LabelText.sanitize("\t\n\u202E\u200B", "fallback.pkg"), "nothing printable is left");
        });
    }

    private static void listing() {
        row("listing-order-and-filter", () -> {
            Fake fake = new Fake().with(
                    app("org.zed.app", "org.zed.app.Main"),
                    app("com.alpha.app", "com.alpha.app.B"),
                    app("com.alpha.app", "com.alpha.app.A"),
                    entry("com.hidden.app", "com.hidden.app.Main", false, false, true, true, 1, 1, 1),
                    entry("com.off.app", "com.off.app.Main", false, true, false, true, 1, 1, 1),
                    entry("com.perm.app", "com.perm.app.Main", false, true, true, false, 1, 1, 1),
                    app("bad name", "x.Y"),
                    app("com.ok.app", "com.ok.app.Main"),
                    app("com.ok.app", "com.ok.app.Main"));
            Listing listing = Listing.of(0, fake.locales, fake.entries);
            List<String> tokens = new ArrayList<>();
            for (Entry e : listing.rows) {
                tokens.add(e.packageName + "/" + e.activity);
            }
            same(Arrays.asList("com.alpha.app/com.alpha.app.A", "com.alpha.app/com.alpha.app.B",
                    "com.ok.app/com.ok.app.Main", "org.zed.app/org.zed.app.Main"), tokens,
                    "sorted, without the not exported, disabled, not permitted, misnamed and doubled ones");
            same(1, listing.skipped, "only the misnamed launchable one is counted as skipped");
        });
        row("listing-per-package-bound", () -> {
            Fake fake = new Fake();
            for (int i = 0; i < 9; i++) {
                fake.with(app("com.many.app", "com.many.app.A" + i));
            }
            Listing listing = Listing.of(0, fake.locales, fake.entries);
            same(8, listing.rows.size(), "8 per package");
            same(1, listing.skipped, "the ninth is skipped");
            same("com.many.app.A7", listing.rows.get(7).activity, "the first eight in order");
        });
        row("listing-row-bound", () -> {
            Fake fake = new Fake();
            for (int i = 0; i < 1100; i++) {
                fake.with(app("com.bulk.p" + String.format("%04d", i), "com.bulk.Main"));
            }
            Listing listing = Listing.of(0, fake.locales, fake.entries);
            same(1024, listing.rows.size(), "1024 rows");
            same(76, listing.skipped, "the rest are skipped");
            same("com.bulk.p1023", listing.rows.get(1023).packageName, "the first 1024 in order");
        });
        row("digest-follows-the-set", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"), builtin("com.b.app", "com.b.app.Main"));
            String base = Listing.of(0, fake.locales, fake.entries).digest;
            List<Entry> shuffled = new ArrayList<>(fake.entries);
            Collections.reverse(shuffled);
            same(base, Listing.of(0, fake.locales, shuffled).digest, "the order the platform reports in does not matter");
            same(base, Listing.of(0, fake.locales, fake.entries).digest, "the same set, the same digest");
            check(!base.equals(Listing.of(0, fake.locales, withAdded(fake.entries,
                    app("com.c.app", "com.c.app.Main"))).digest), "an installed app changes it");
            check(!base.equals(Listing.of(0, fake.locales, fake.entries.subList(0, 1)).digest), "a removed app changes it");
            check(!base.equals(Listing.of(0, fake.locales, withReplaced(fake.entries, 0,
                    entry("com.a.app", "com.a.app.Main", false, true, true, true, 11, 1_000, 0x7f010000))).digest),
                    "an update (version code) changes it");
            check(!base.equals(Listing.of(0, fake.locales, withReplaced(fake.entries, 0,
                    entry("com.a.app", "com.a.app.Main", false, true, true, true, 10, 2_000, 0x7f010000))).digest),
                    "a reinstall (install time) changes it");
            check(!base.equals(Listing.of(0, fake.locales, withReplaced(fake.entries, 0,
                    entry("com.a.app", "com.a.app.Main", false, true, true, true, 10, 1_000, 0x7f020000))).digest),
                    "a new icon resource changes it");
            check(!base.equals(Listing.of(0, fake.locales, withReplaced(fake.entries, 0,
                    entry("com.a.app", "com.a.app.Main", false, true, false, true, 10, 1_000, 0x7f010000))).digest),
                    "a disabled app changes it");
            check(!base.equals(Listing.of(0, fake.locales, withReplaced(fake.entries, 0,
                    entry("com.a.app", "com.a.app.Other", false, true, true, true, 10, 1_000, 0x7f010000))).digest),
                    "a renamed launcher activity changes it");
            check(!base.equals(Listing.of(0, Arrays.asList("fr-FR"), fake.entries).digest), "Android's language changes it");
            check(!base.equals(Listing.of(10, fake.locales, fake.entries).digest), "another foreground user changes it");
            check(!base.equals(Listing.of(0, fake.locales, withReplaced(fake.entries, 0,
                    new Entry("com.a.app", "com.a.app.Main", false, true, true, true, 10, 1_000, 0x7f010000, "overlay", null))).digest),
                    "an overlay, split or library coming or going changes it");
        });
        row("digest-follows-the-whole-locale-list", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"));
            List<String> six = Arrays.asList("a", "b", "c", "d", "e", "f");
            List<String> other = Arrays.asList("a", "b", "c", "d", "e", "g");
            Listing first = Listing.of(0, six, fake.entries);
            Listing second = Listing.of(0, other, fake.entries);
            same(first.locales, second.locales, "the header shows the same five tags");
            check(!first.digest.equals(second.digest), "the sixth locale changes the labels, so it changes the digest");
            check(!first.digest.equals(Listing.of(0, Arrays.asList("a", "b", "c", "d", "e"), fake.entries).digest),
                    "a sixth locale coming changes it");
            same(Listing.of(0, Arrays.asList("a", "b"), fake.entries).digest, Listing.of(0, Arrays.asList("a", "b"), fake.entries).digest,
                    "stable");
        });
        row("icon-key-follows-the-icon", () -> {
            Entry a = app("com.a.app", "com.a.app.Main");
            String key = Listing.iconKey(a);
            check(key.matches("[0-9a-f]{16}"), "16 hex characters");
            same(key, Listing.iconKey(app("com.a.app", "com.a.app.Main")), "stable");
            check(!key.equals(Listing.iconKey(entry("com.a.app", "com.a.app.Main", false, true, true, true, 11, 1_000, 0x7f010000))), "version");
            check(!key.equals(Listing.iconKey(entry("com.a.app", "com.a.app.Main", false, true, true, true, 10, 1_001, 0x7f010000))), "install time");
            check(!key.equals(Listing.iconKey(entry("com.a.app", "com.a.app.Main", false, true, true, true, 10, 1_000, 5))), "icon resource");
            check(!key.equals(Listing.iconKey(app("com.a.app", "com.a.app.Other"))), "component");
            check(!key.equals(Listing.iconKey(builtin("com.a.app", "com.a.app.Main"))), "built-in flag");
            check(!key.equals(Listing.iconKey(new Entry("com.a.app", "com.a.app.Main", false, true, true, true, 10, 1_000,
                    0x7f010000, "split", null))), "the files the resources come from");
            same(2, Listing.RENDER_VERSION, "the renderer's version (the icons are drawn for fixed inputs since 2)");
        });
        row("locale-line", () -> {
            same("en-US,ja-JP", Listing.localeLine(Arrays.asList("en-US", "ja-JP")), "tags joined");
            same("a,b,c,d,e", Listing.localeLine(Arrays.asList("a", "b", "c", "d", "e", "f", "g")), "five at most");
            same("en", Listing.localeLine(Arrays.asList("e n", "en\n", "", "en;x", "en")), "only letters, digits and hyphens");
            same("", Listing.localeLine(Collections.<String>emptyList()), "none");
            String longTag = repeat("a", 35);
            String joined = Listing.localeLine(Arrays.asList(longTag, longTag, longTag, longTag, longTag));
            check(joined.length() <= 128, "128 bytes at most");
            String valid41 = "zh-Hant-TW-u-ca-chinese-nu-hanidec-x-private";
            check(valid41.length() > 35 && valid41.length() <= 128, "a valid tag longer than 35 characters");
            same(valid41, Listing.localeLine(Arrays.asList(valid41)), "it fits the budget and is shown");
            same("", Listing.localeLine(Arrays.asList(repeat("a", 129))), "a tag over the budget is not");
        });
    }

    private static List<Entry> withAdded(List<Entry> base, Entry extra) {
        List<Entry> list = new ArrayList<>(base);
        list.add(extra);
        return list;
    }

    private static List<Entry> withReplaced(List<Entry> base, int index, Entry replacement) {
        List<Entry> list = new ArrayList<>(base);
        list.set(index, replacement);
        return list;
    }

    private static void listAnswer() {
        row("digest-answer", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"));
            Result digest = run(fake, "digest");
            Parsed header = parseList(digest, false);
            Result list = run(fake, "list");
            Parsed listed = parseList(list, true);
            same(header.header, listed.header, "digest and list begin with the same header block");
            same("1", header.header.get("count"), "count");
            same("0", header.header.get("skipped"), "skipped");
            same("en-US,ja-JP", header.header.get("locales"), "locales");
            same("0", header.header.get("user"), "user");
            same(Listing.of(0, fake.locales, fake.entries).digest, header.header.get("digest"), "the digest");
            same(0, digest.err.length(), "nothing on stderr");
        });
        row("list-format", () -> {
            Fake fake = new Fake().with(builtin("com.android.settings", "com.android.settings.Settings"),
                    app("com.example.game", "com.example.game.Main$Launcher"));
            fake.labels.put("com.android.settings/com.android.settings.Settings", "Settings");
            fake.labels.put("com.example.game/com.example.game.Main$Launcher", "Fun Game");
            Result result = run(fake, "list");
            Parsed parsed = parseList(result, true);
            same(2, parsed.rows.size(), "two rows");
            same("com.android.settings", parsed.rows.get(0)[0], "package");
            same("com.android.settings.Settings", parsed.rows.get(0)[1], "activity");
            same("Settings", parsed.rows.get(0)[2], "label");
            same("1", parsed.rows.get(0)[3], "built in");
            same("0", parsed.rows.get(1)[3], "installed");
            same(Listing.iconKey(fake.entries.get(0)), parsed.rows.get(0)[4], "icon key");
            same("Fun Game", parsed.rows.get(1)[2], "label");
        });
        row("list-hostile-labels", () -> {
            String[] hostile = {
                "A\tB", "A\nB", "A\r\nend\t0\nB", "rows\nend\t99", "x\u2028y", "p\u2029q", "\nkernarium_apps 1\ndigest\t0",
                "evil\u202E\u2066 label", "$(reboot)", "'; reboot; '", "\u0000nul", "tab\t\t\tmany",
            };
            Fake fake = new Fake();
            for (int i = 0; i < hostile.length; i++) {
                String packageName = "com.host" + i + ".app";
                fake.with(app(packageName, packageName + ".Main"));
                fake.labels.put(packageName + "/" + packageName + ".Main", hostile[i]);
            }
            Result result = run(fake, "list");
            Parsed parsed = parseList(result, true);
            same(hostile.length, parsed.rows.size(), "one row per app, however the labels are written");
            String text = result.text();
            same(hostile.length + 8, lines(result).size(), "the lines are the header, rows, end and one line per app");
            check(!text.contains("\u202E") && !text.contains("\u2066") && !text.contains("\u2028")
                    && !text.contains("\u2029") && !text.contains("\u0000"), "no bidirectional, separator or NUL character reaches the answer");
        });
        row("list-label-unreadable-uses-the-package", () -> {
            Fake fake = new Fake().with(app("com.broken.app", "com.broken.app.Main"), app("com.none.app", "com.none.app.Main"));
            fake.labelFails.add("com.broken.app");
            Parsed parsed = parseList(run(fake, "list"), true);
            same("com.broken.app", parsed.rows.get(0)[2], "the label could not be read");
            same("com.none.app", parsed.rows.get(1)[2], "the application has no label");
        });
        row("list-empty", () -> {
            Parsed parsed = parseList(run(new Fake(), "list"), true);
            same(0, parsed.rows.size(), "no rows");
            same("0", parsed.header.get("count"), "count");
        });
        row("list-sizes-within-the-bounds", () -> {
            Fake fake = new Fake();
            String longPackage = repeat("a", 127) + "." + repeat("b", 127);
            for (int i = 0; i < 1100; i++) {
                String activity = repeat("C", 251) + String.format("%04d", i);
                fake.with(app(i < 1 ? longPackage : repeat("p", 120) + "." + String.format("q%04d", i), activity));
                fake.labels.put(fake.entries.get(i).packageName + "/" + activity, repeat("\uD83D\uDE00", 90));
            }
            Result result = run(fake, "list");
            Parsed parsed = parseList(result, true);
            same(1024, parsed.rows.size(), "the row bound");
            for (String line : lines(result)) {
                check(line.getBytes(StandardCharsets.UTF_8).length <= 860, "a line is at most 860 bytes");
            }
            check(result.out.length < 900 * 1024, "the whole answer is under 900 KiB: " + result.out.length);
        });
    }

    private static void icons() {
        row("icons-record-framing", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"), app("com.b.app", "com.b.app.Main"));
            fake.icons = (entry, size) -> new Icon(size, entry.packageName.equals("com.a.app"), entry.packageName.equals("com.b.app"),
                    png(entry.packageName.equals("com.a.app") ? 1000 : 2000, size));
            Result result = run(fake, "icons", "com.a.app/com.a.app.Main", "com.b.app/com.b.app.Main");
            List<IconRecord> records = parseIcons(result);
            same(2, records.size(), "two records in the order asked");
            same("com.a.app/com.a.app.Main", records.get(0).token, "first");
            same("ok", records.get(0).status, "ok");
            same(192, records.get(0).width, "192 pixels");
            same("adaptive", records.get(0).kind, "adaptive");
            same("0", records.get(0).alpha, "opaque");
            same(1000, records.get(0).length, "length");
            same("legacy", records.get(1).kind, "legacy");
            same("1", records.get(1).alpha, "alpha");
            same(0, result.err.length(), "nothing on stderr");
            same(Arrays.asList(192, 192), fake.iconSizes, "each icon rendered once at the first size");
        });
        row("icons-order-is-the-order-asked", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"), app("com.b.app", "com.b.app.Main"));
            List<IconRecord> records = parseIcons(run(fake, "icons", "com.b.app/com.b.app.Main", "com.a.app/com.a.app.Main"));
            same("com.b.app/com.b.app.Main", records.get(0).token, "first asked, first answered");
            same("com.a.app/com.a.app.Main", records.get(1).token, "second");
        });
        row("icons-statuses", () -> {
            Fake fake = new Fake().with(app("com.ok.app", "com.ok.app.Main"),
                    entry("com.noicon.app", "com.noicon.app.Main", false, true, true, true, 1, 1, 0),
                    app("com.throws.app", "com.throws.app.Main"), app("com.notpng.app", "com.notpng.app.Main"),
                    app("com.badsize.app", "com.badsize.app.Main"), app("com.null.app", "com.null.app.Main"));
            fake.icons = (entry, size) -> {
                switch (entry.packageName) {
                    case "com.throws.app":
                        throw new OutOfMemoryError("a hostile icon");
                    case "com.notpng.app":
                        return new Icon(size, false, false, "GIF89a not a png".getBytes(StandardCharsets.US_ASCII));
                    case "com.badsize.app":
                        return new Icon(size + 1, false, false, png(100, 1));
                    case "com.null.app":
                        return null;
                    default:
                        return new Icon(size, false, false, png(100, 1));
                }
            };
            List<IconRecord> records = parseIcons(run(fake, "icons", "com.ok.app/com.ok.app.Main",
                    "com.gone.app/com.gone.app.Main", "com.noicon.app/com.noicon.app.Main",
                    "com.throws.app/com.throws.app.Main", "com.notpng.app/com.notpng.app.Main",
                    "com.badsize.app/com.badsize.app.Main", "com.null.app/com.null.app.Main"));
            same(Arrays.asList("ok", "missing", "noicon", "error", "error", "error", "error"),
                    statuses(records), "statuses");
        });
        row("icons-not-launchable-is-missing", () -> {
            Fake fake = new Fake().with(entry("com.hidden.app", "com.hidden.app.Main", false, false, true, true, 1, 1, 1),
                    entry("com.off.app", "com.off.app.Main", false, true, false, true, 1, 1, 1),
                    entry("com.perm.app", "com.perm.app.Main", false, true, true, false, 1, 1, 1));
            List<IconRecord> records = parseIcons(run(fake, "icons", "com.hidden.app/com.hidden.app.Main",
                    "com.off.app/com.off.app.Main", "com.perm.app/com.perm.app.Main"));
            same(Arrays.asList("missing", "missing", "missing"), statuses(records), "no icon of what cannot be launched");
            same(0, fake.iconSizes.size(), "nothing rendered");
        });
        row("icons-size-steps", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"));
            fake.icons = (entry, size) -> new Icon(size, true, false, png(size == 192 ? 98305 : 5000, 1));
            List<IconRecord> records = parseIcons(run(fake, "icons", "com.a.app/com.a.app.Main"));
            same("ok", records.get(0).status, "a smaller one fits");
            same(128, records.get(0).width, "the next size");
            same(Arrays.asList(192, 128), fake.iconSizes, "192 then 128");
            Fake all = new Fake().with(app("com.a.app", "com.a.app.Main"));
            all.icons = (entry, size) -> new Icon(size, true, false, png(98305, 1));
            same(Arrays.asList("error"), statuses(parseIcons(run(all, "icons", "com.a.app/com.a.app.Main"))), "none fits");
            same(Arrays.asList(192, 128, 96), all.iconSizes, "every size tried, no other");
        });
        row("icons-png-bound-is-exact", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"));
            fake.icons = (entry, size) -> new Icon(size, true, false, png(AppsCommand.MAX_PNG_BYTES, 1));
            List<IconRecord> records = parseIcons(run(fake, "icons", "com.a.app/com.a.app.Main"));
            same("ok", records.get(0).status, "98304 bytes are within the bound");
            same(192, records.get(0).width, "at the first size");
            same(98304, records.get(0).length, "length");
        });
        row("icons-failing-render-is-silent", () -> {
            Throwable[] failures = {new OutOfMemoryError("a hostile bitmap"), new StackOverflowError(),
                new IllegalStateException("uid=2000 android.permission.SECRET"), new AssertionError("x")};
            java.io.PrintStream before = System.err;
            ByteArrayOutputStream captured = new ByteArrayOutputStream();
            System.setErr(new java.io.PrintStream(captured, true));
            List<String> got;
            try {
                got = new ArrayList<>();
                for (Throwable failure : failures) {
                    Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"));
                    fake.icons = (entry, size) -> {
                        throw sneaky(failure);
                    };
                    Result result = run(fake, "icons", "com.a.app/com.a.app.Main");
                    got.addAll(statuses(parseIcons(result)));
                    same(0, result.err.length(), "nothing on the command's stderr");
                }
            } finally {
                System.setErr(before);
            }
            same(Arrays.asList("error", "error", "error", "error"), got, "an exception or an error is that icon's error");
            same(0, captured.size(), "no stack trace of a dying worker reaches the process's stderr");
        });
        row("icons-render-time-limit", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"), app("com.b.app", "com.b.app.Main"),
                    entry("com.c.app", "com.c.app.Main", false, true, true, true, 1, 1, 0),
                    entry("com.hidden.app", "com.hidden.app.Main", false, false, true, true, 1, 1, 1));
            fake.renderLimit = 200;
            java.util.concurrent.CountDownLatch never = new java.util.concurrent.CountDownLatch(1);
            fake.icons = (entry, size) -> {
                if (entry.packageName.equals("com.a.app")) {
                    never.await();
                }
                return new Icon(size, false, false, png(100, 1));
            };
            long begin = System.nanoTime();
            List<IconRecord> records = parseIcons(run(fake, "icons", "com.a.app/com.a.app.Main", "com.b.app/com.b.app.Main",
                    "com.c.app/com.c.app.Main", "com.hidden.app/com.hidden.app.Main"));
            long tookMillis = (System.nanoTime() - begin) / 1_000_000;
            same(Arrays.asList("error", "deferred", "noicon", "missing"), statuses(records),
                    "the stuck one is an error, no other render is started, the answers that need none are given");
            check(tookMillis < 1500, "the command did not wait for the stuck render: " + tookMillis + " ms");
            same(Arrays.asList(192), fake.iconSizes, "only the stuck render was started");
            never.countDown();
        });
        row("icons-deadline-defers-the-rest", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"), app("com.b.app", "com.b.app.Main"),
                    app("com.c.app", "com.c.app.Main"));
            fake.clockStep = 12_000;
            List<IconRecord> records = parseIcons(run(fake, "icons", "com.a.app/com.a.app.Main", "com.b.app/com.b.app.Main",
                    "com.c.app/com.c.app.Main"));
            same(Arrays.asList("ok", "deferred", "deferred"), statuses(records), "no icon is started after 20 s");
            Fake quick = new Fake().with(app("com.a.app", "com.a.app.Main"));
            quick.clockStep = 20_000;
            same(Arrays.asList("ok"), statuses(parseIcons(run(quick, "icons", "com.a.app/com.a.app.Main"))), "20 s exactly is still in time");
        });
        row("icons-refuses-a-bad-component", () -> {
            for (String bad : new String[] {"a.b", "a.b/Main; reboot", "-x/y", "a.b/Main'", "com.a.app/com.a.app.Main\n"}) {
                Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"));
                Result result = run(fake, "icons", "com.a.app/com.a.app.Main", bad);
                same(2, result.status, "refused: " + show(bad));
                same(0, result.out.length, "no stdout");
                check(!result.platformMade, "nothing asked of the platform");
                check(!result.err.contains(bad) && !result.err.contains("reboot"), "the refused text is not echoed");
            }
        });
        row("icons-count-and-duplicates", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"));
            same(64, run(fake, "icons").status, "none");
            List<String> nine = new ArrayList<>(Arrays.asList("icons"));
            for (int i = 0; i < 9; i++) {
                nine.add("com.a" + i + ".app/com.a" + i + ".Main");
            }
            same(64, run(fake, nine.toArray(new String[0])).status, "nine");
            List<String> eight = new ArrayList<>(nine.subList(0, 9));
            Result ok = run(fake, eight.toArray(new String[0]));
            same(0, ok.status, "eight");
            same(8, parseIcons(ok).size(), "eight records");
            Result twice = run(fake, "icons", "com.a.app/com.a.app.Main", "com.a.app/com.a.app.Main");
            same(64, twice.status, "the same component twice");
            same(0, twice.out.length, "no stdout");
            same("kernarium_apps: the same component twice\n", twice.err, "one line, not the usage text");
        });
        row("icons-answer-size", () -> {
            Fake fake = new Fake();
            List<String> arguments = new ArrayList<>(Arrays.asList("icons"));
            for (int i = 0; i < 8; i++) {
                String packageName = repeat("a", 127) + "." + repeat("b", 120) + i;
                String activity = repeat("C", 254) + i;
                fake.with(app(packageName, activity));
                arguments.add(packageName + "/" + activity);
            }
            fake.icons = (entry, size) -> new Icon(size, true, true, png(AppsCommand.MAX_PNG_BYTES, 2));
            Result result = run(fake, arguments.toArray(new String[0]));
            same(8, parseIcons(result).size(), "eight records");
            check(result.out.length < 800 * 1024, "under 800 KiB: " + result.out.length);
        });
    }

    /** Throws {@code failure} whatever its kind, as a platform's code that does not declare it would. */
    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException sneaky(Throwable failure) throws T {
        throw (T) failure;
    }

    private static List<String> statuses(List<IconRecord> records) {
        List<String> statuses = new ArrayList<>();
        for (IconRecord record : records) {
            statuses.add(record.status);
        }
        return statuses;
    }

    private static void launch() {
        final String settings = "com.android.settings/com.android.settings.Settings";
        row("launch-starts-exactly-the-component", () -> {
            Fake fake = new Fake().with(builtin("com.android.settings", "com.android.settings.Settings"));
            fake.user = 10;
            Result result = run(fake, "launch", settings);
            same(0, result.status, "exit status");
            same("launched\t" + settings + "\n", result.text(), "the answer");
            same(1, fake.started.size(), "one start");
            same(Arrays.asList("/system/bin/cmd", "activity", "start-activity", "--user", "10", "-a",
                    "android.intent.action.MAIN", "-c", "android.intent.category.LAUNCHER", "-f", "0x10200000", "-n",
                    settings), fake.started.get(0), "the argument vector: separate strings, no shell");
            same(0, result.err.length(), "nothing on stderr");
        });
        row("launch-refuses-what-is-not-listed", () -> {
            Fake fake = new Fake().with(builtin("com.android.settings", "com.android.settings.Settings"),
                    entry("com.hidden.app", "com.hidden.app.Main", false, false, true, true, 1, 1, 1),
                    entry("com.off.app", "com.off.app.Main", false, true, false, true, 1, 1, 1),
                    entry("com.perm.app", "com.perm.app.Main", false, true, true, false, 1, 1, 1));
            String[] refused = {
                "com.android.settings/com.android.settings.Other", "com.other.app/com.other.app.Main",
                "com.android.settings/com.android.settings.Main", "com.hidden.app/com.hidden.app.Main",
                "com.off.app/com.off.app.Main", "com.perm.app/com.perm.app.Main",
            };
            for (String token : refused) {
                Result result = run(fake, "launch", token);
                same(2, result.status, "refused: " + token);
                same(0, result.out.length, "no stdout");
                check(result.err.startsWith("kernarium_apps: refused"), "the refusal is said: " + result.err);
            }
            same(0, fake.started.size(), "no process was started for any of them");
        });
        row("launch-refuses-hostile-arguments-before-anything-runs", () -> {
            Fake fake = new Fake().with(builtin("com.android.settings", "com.android.settings.Settings"));
            String[] hostile = {
                settings + "; reboot", settings + " --es x y", "-n " + settings, settings + "\n", settings + "'",
                "--user 0/" + settings, "com.android.settings/$(id)", "com.android.settings/`id`",
                "com.android.settings/com.android.settings.Settings|sh", "com.android.settings", "/", "",
                "com.android.settings//com.android.settings.Settings", "com.android.settings/.Settings",
                "com.android.settings/com.android.settings.Settings\u0000",
                "com.android;settings/com.android.settings.Settings", "com.android.set tings/x.Y",
                "com.android.set'tings/x.Y", "com.android$settings/x.Y", "com.android.set-tings/x.Y",
                "com.android.set\"tings/x.Y", "com.android.set(tings)/x.Y", "com.android.set\ntings/x.Y",
            };
            for (String token : hostile) {
                Result result = run(fake, "launch", token);
                same(2, result.status, "refused: " + show(token));
                same(0, result.out.length, "no stdout");
                check(!result.platformMade, "the platform is not even made: " + show(token));
            }
            same(0, fake.started.size(), "nothing started");
        });
        row("launch-failures", () -> {
            Fake fake = new Fake().with(builtin("com.android.settings", "com.android.settings.Settings"));
            fake.startResult = new AppsCommand.StartResult(255, "Exception occurred while executing 'start-activity':\n");
            Result exited = run(fake, "launch", settings);
            same(1, exited.status, "a status other than 0");
            same(0, exited.out.length, "no stdout");
            check(exited.err.length() <= 512, "stderr is bounded");
            fake.startResult = new AppsCommand.StartResult(0,
                    "Starting: Intent { }\nError type 3\nError: Activity class {com.x/com.x.Y} does not exist.\n");
            Result reported = run(fake, "launch", settings);
            same(1, reported.status, "an Error line with status 0");
            same(0, reported.out.length, "no stdout");
            fake.startResult = new AppsCommand.StartResult(0, "Starting: Intent { }\nWarning: Activity not started, its current task has been brought to the front\n");
            Result front = run(fake, "launch", settings);
            same(0, front.status, "an app already open is brought to the front");
            fake.startResult = new AppsCommand.StartResult(255, "Starting: Intent { }\n\nException occurred while executing 'start-activity':\n"
                    + "java.lang.SecurityException: Permission Denial: starting Intent { cmp=com.x/com.x.Y } from null (pid=1, uid=2000) requires android.permission.SECRET\n"
                    + "\tat com.android.server.am.ActivityManagerService.start(ActivityManagerService.java:1)\n");
            Result denied = run(fake, "launch", settings);
            same(1, denied.status, "a permission denial");
            same("kernarium_apps: cmd activity start-activity: permission denied (exit 255)\n", denied.err,
                    "fixed words: neither the uid nor the permission is copied");
        });
        row("launch-component-named-like-an-error", () -> {
            // The command echoes the intent it starts, component included: a launcher named like a failure starts.
            String[][] names = {
                {"com.example.viewer", "com.example.viewer.SecurityExceptionViewer"},
                {"com.example.errors", "com.example.errors.ErrorActivity"},
                {"com.example.perm", "com.example.perm.PermissionDenialMain"},
                {"com.example.exc", "com.example.exc.ExceptionList"},
                {"com.example.error", "com.example.error.Error"},
            };
            for (String[] name : names) {
                Fake fake = new Fake().with(app(name[0], name[1]));
                String token = name[0] + "/" + name[1];
                fake.startResult = new AppsCommand.StartResult(0, "Starting: Intent { act=android.intent.action.MAIN "
                        + "cat=[android.intent.category.LAUNCHER] flg=0x10200000 cmp=" + token + " }\n");
                Result result = run(fake, "launch", token);
                same(0, result.status, "a start of " + token);
                same("launched\t" + token + "\n", result.text(), "the answer");
                fake.startResult = new AppsCommand.StartResult(0, "Starting: Intent { cmp=" + token + " }\n"
                        + "Warning: Activity not started, its current task has been brought to the front\n");
                same(0, run(fake, "launch", token).status, "an app already open: " + token);
            }
        });
    }

    private static void starts() {
        row("start-outcome", () -> {
            check(StartOutcome.failure(0, "Starting: Intent { cmp=a.b/c.D }\n") == null, "plain start");
            check(StartOutcome.failure(0, "") == null, "no output");
            same("cmd activity start-activity: exited 1", StartOutcome.failure(1, ""), "status 1 and nothing said");
            check(StartOutcome.failure(0, "Starting: Intent { cmp=a.b/c.SecurityExceptionViewer }\n"
                    + "Warning: Activity not started, intent has been delivered to currently running top-most instance.\n") == null,
                    "a warning is not a failure");
            String reason = StartOutcome.failure(1, "Error: \u202E \u0001 bad \u00e9 " + repeat("x", 500) + "\n");
            same("cmd activity start-activity: start failed (exit 1)", reason, "an Error line gives a fixed reason, not its text");
            for (char c : reason.toCharArray()) {
                check(c >= 0x20 && c < 0x7F, "printable ASCII only");
            }
        });
        row("start-outcome-kinds", () -> {
            String denied = "cmd activity start-activity: permission denied";
            check(StartOutcome.failure(255, "Starting: Intent { }\n\nException occurred while executing 'start-activity':\n"
                    + "java.lang.SecurityException: Permission Denial: starting Intent { } from null (pid=9, uid=2000)\n").startsWith(denied),
                    "a security exception is a denial, the first 'Exception occurred' line notwithstanding");
            check(StartOutcome.failure(0, "Starting: Intent { }\nError: Activity not started, you do not have permission to access it.\n")
                    .startsWith(denied), "an Error line about a permission");
            check(StartOutcome.failure(0, "Permission Denial: starting Intent { } from null requires android.permission.X\n")
                    .startsWith(denied), "a Permission Denial line");
            String missing = "cmd activity start-activity: no such activity";
            check(StartOutcome.failure(0, "Starting: Intent { }\nError type 3\nError: Activity class {com.x/com.x.Y} does not exist.\n")
                    .startsWith(missing), "a class that does not exist");
            String failed = "cmd activity start-activity: start failed";
            check(StartOutcome.failure(0, "Starting: Intent { }\nError: Activity not started, unable to resolve Intent { }\n")
                    .startsWith(failed), "another Error line");
            check(StartOutcome.failure(255, "java.lang.IllegalStateException: boom\n").startsWith(failed), "another exception");
            check(StartOutcome.failure(1, "at com.android.Foo.bar(Foo.java:1)\n").equals("cmd activity start-activity: exited 1"),
                    "a stack frame alone says nothing");
            String reason = StartOutcome.failure(255, "Error: uid=2000 android.permission.SECRET /data/app/x\n");
            check(!reason.contains("2000") && !reason.contains("SECRET") && !reason.contains("/data"),
                    "nothing of the command's own words: " + reason);
            check(StartOutcome.failure(0, "Starting: Intent { cmp=a.b/c.ErrorActivity }\n") == null, "an echoed component");
            check(StartOutcome.failure(0, "Starting: Intent { cmp=a.b/c.Error }\nStarting: Exception\n") == null,
                    "Starting lines never count");
        });
    }

    private static void usageAndFailures() {
        row("usage", () -> {
            String[][] bad = {
                {}, {"list", "extra"}, {"digest", "x"}, {"help"}, {"--locale", "en"}, {"list", "--locale", "en"},
                {"launch"}, {"launch", "a.b/C", "d.e/F"}, {"LIST"}, {"icon", "a.b/C"}, {"set", "a.b", "on"},
                {""}, {"list\n"},
            };
            for (String[] args : bad) {
                Fake fake = new Fake();
                Result result = run(fake, args);
                same(64, result.status, "usage: " + show(Arrays.asList(args).toString()));
                same(0, result.out.length, "no stdout");
                check(result.err.startsWith("usage: kernarium_apps"), "the usage line");
                check(!result.platformMade, "the platform is not made");
            }
        });
        row("failure-leaves-no-stdout", () -> {
            Fake fake = new Fake() {
                @Override
                public List<Entry> launcherActivities(int forUser) throws Exception {
                    throw new PlatformException("the package manager gave no answer");
                }
            };
            for (String verb : new String[] {"digest", "list"}) {
                Result result = run(fake, verb);
                same(1, result.status, verb + " exit status");
                same(0, result.out.length, verb + " prints nothing");
                same("kernarium_apps: " + verb + " failed: the package manager gave no answer\n", result.err, "stderr says why");
            }
            Result icons = run(fake, "icons", "a.b/C");
            same(1, icons.status, "icons");
            same(0, icons.out.length, "icons prints nothing");
            Result launch = run(fake, "launch", "a.b/C");
            same(1, launch.status, "launch");
            same(0, launch.out.length, "launch prints nothing");
        });
        row("platform-cannot-be-made", () -> {
            StringWriter err = new StringWriter();
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            int status = AppsCommand.run(new String[] {"list"}, () -> {
                throw new PlatformException("the package manager is not running");
            }, out, new PrintWriter(err));
            same(1, status, "exit status");
            same(0, out.size(), "no stdout");
            same("kernarium_apps: the package manager is not running\n", err.toString(), "stderr says why");
            same(64, AppsCommand.run(new String[] {"nope"}, () -> {
                throw new IllegalStateException("never made");
            }, out, new PrintWriter(new StringWriter())), "a usage error needs no platform");
        });
        row("failure-reasons-are-fixed", () -> {
            // A framework's message can carry a uid, a permission or a path: only the class of the exception is said.
            Fake fake = new Fake() {
                @Override
                public int currentUser() {
                    throw new SecurityException("Permission Denial: getCurrentUser() from pid=1, uid=2000 requires android.permission.INTERACT_ACROSS_USERS");
                }
            };
            for (String verb : new String[] {"digest", "list"}) {
                Result result = run(fake, verb);
                same(1, result.status, verb + " exit status");
                same("kernarium_apps: " + verb + " failed: unexpected java.lang.SecurityException\n", result.err, "the class only");
                same(0, result.out.length, "nothing on stdout");
            }
            Result icons = run(fake, "icons", "a.b/C");
            same(1, icons.status, "icons");
            same(0, icons.out.length, "icons prints nothing");
            same("kernarium_apps: icons failed: unexpected java.lang.SecurityException\n", icons.err, "icons says the class");
            Result launch = run(fake, "launch", "a.b/C");
            same(1, launch.status, "launch with no known foreground user");
            same(0, launch.out.length, "no stdout");
            same(0, fake.started.size(), "nothing is started for a user that is not known");
        });
        row("failure-of-any-question-is-a-failure", () -> {
            // The commands take no substitute: a configuration, a listing or a user that cannot be had is exit 1.
            final String[] broken = {"locales", "listing", "user"};
            for (String what : broken) {
                Fake fake = new Fake() {
                    @Override
                    public int currentUser() throws Exception {
                        if (what.equals("user")) {
                            throw new PlatformException("the foreground user is not known");
                        }
                        return super.currentUser();
                    }

                    @Override
                    public List<String> localeTags() throws Exception {
                        if (what.equals("locales")) {
                            throw new PlatformException("the system's configuration cannot be read");
                        }
                        return super.localeTags();
                    }

                    @Override
                    public List<Entry> launcherActivities(int forUser) throws Exception {
                        if (what.equals("listing")) {
                            throw new PlatformException("a permission could not be checked");
                        }
                        return super.launcherActivities(forUser);
                    }
                };
                fake.with(app("com.a.app", "com.a.app.Main"));
                for (String[] args : new String[][] {{"digest"}, {"list"}, {"launch", "com.a.app/com.a.app.Main"}}) {
                    if (what.equals("locales") && args[0].equals("launch")) {
                        continue; // a launch needs no locale
                    }
                    Result result = run(fake, args);
                    same(1, result.status, what + " broken: " + args[0]);
                    same(0, result.out.length, "no stdout");
                }
                same(0, fake.started.size(), what + " broken: nothing started");
            }
        });
        row("stderr-is-one-bounded-line", () -> {
            Fake fake = new Fake() {
                @Override
                public int currentUser() throws Exception {
                    throw new PlatformException(repeat("long message \u202E ", 500) + "\nsecond line");
                }
            };
            Result result = run(fake, "list");
            same(1, result.status, "exit status");
            check(result.err.getBytes(StandardCharsets.UTF_8).length <= 512, "512 bytes at most: " + result.err.length());
            same(1, result.err.split("\n").length, "one line");
            for (char c : result.err.toCharArray()) {
                check(c == '\n' || (c >= 0x20 && c < 0x7F), "printable ASCII");
            }
        });
        row("exit-statuses-are-the-contract", () -> {
            same(0, AppsCommand.EXIT_OK, "ok");
            same(1, AppsCommand.EXIT_FAILURE, "failure");
            same(2, AppsCommand.EXIT_REFUSED, "refused");
            same(64, AppsCommand.EXIT_USAGE, "usage");
            same(1, AppsCommand.FORMAT_VERSION, "format version");
            same("kernarium_apps", AppsCommand.NAME, "name");
            same(8, AppsCommand.MAX_ICONS, "components per icons");
            same(98304, AppsCommand.MAX_PNG_BYTES, "PNG bound");
            same(20_000L, AppsCommand.ICON_DEADLINE_MILLIS, "deadline");
            same(8_000L, AppsCommand.ICON_RENDER_MILLIS, "one icon's render");
            check(AppsCommand.ICON_DEADLINE_MILLIS + AppsCommand.ICON_RENDER_MILLIS <= 30_000, "the whole icons command in the host's 30 s");
            same(1024, Listing.MAX_ROWS, "rows");
            same(32, SourceBound.MAX_RESOURCES, "resources one icon may reach");
            same(6, SourceBound.MAX_DEPTH, "depth of the chain one icon may reach");
            same(8, Listing.MAX_PER_PACKAGE, "per package");
            same(80, LabelText.MAX_CODE_POINTS, "label code points");
            same(320, LabelText.MAX_BYTES, "label bytes");
        });
    }

    /** A shell command line as the argument vector of a process. */
    private static List<String> sh(String script, String... arguments) {
        List<String> argv = new ArrayList<>(Arrays.asList("/bin/sh", "-c", script, "sh"));
        argv.addAll(Arrays.asList(arguments));
        return argv;
    }

    /** Whether a process that the child wrote its pid for is still there (a zombie is not reaped, so it counts). */
    private static boolean alive(java.io.File pidFile) throws Exception {
        String text = new String(java.nio.file.Files.readAllBytes(pidFile.toPath()), StandardCharsets.US_ASCII).trim();
        check(!text.isEmpty(), "the child wrote its pid");
        java.util.Optional<ProcessHandle> handle = ProcessHandle.of(Long.parseLong(text));
        boolean here = handle.isPresent() && handle.get().isAlive();
        if (here) {
            handle.get().destroyForcibly(); // do not leave it behind when the row fails
        }
        return here;
    }

    private static java.io.File pidFile() throws Exception {
        java.io.File file = java.io.File.createTempFile("kernarium-apps-pid", ".txt");
        file.deleteOnExit();
        return file;
    }

    private static void children() {
        row("child-output-is-drained", () -> {
            // 330 kB of output and status 3: a parent that does not read while it waits stalls the child on a full pipe.
            ChildProcess.Result result = ChildProcess.run(sh("i=0; while [ $i -lt 30000 ]; do echo 0123456789; i=$((i+1)); done; exit 3"),
                    4_000, 4096);
            same(3, result.exit, "the child's status");
            same(4096, result.output.length(), "the first bytes are kept and no more");
            check(result.output.startsWith("0123456789\n"), "the output is the child's");
        });
        row("child-output-merges-error", () -> {
            ChildProcess.Result result = ChildProcess.run(sh("echo out; echo err >&2"), 4_000, 4096);
            same(0, result.exit, "status");
            check(result.output.contains("out") && result.output.contains("err"), "both streams: " + show(result.output));
        });
        row("child-deadline-terminates-and-reaps", () -> {
            java.io.File pid = pidFile();
            try {
                ChildProcess.run(sh("echo $$ > \"$1\"; exec sleep 60", pid.getPath()), 600, 4096);
                throw new Mismatch("a child past the deadline is a failure");
            } catch (PlatformException late) {
                same("cmd activity start-activity did not finish", late.getMessage(), "the reason is fixed");
            }
            check(!alive(pid), "the child is gone when run returns");
        });
        row("child-that-ignores-termination-is-killed", () -> {
            java.io.File pid = pidFile();
            long begin = System.nanoTime();
            try {
                ChildProcess.run(sh("trap '' TERM; echo $$ > \"$1\"; while :; do sleep 1; done", pid.getPath()), 600, 4096);
                throw new Mismatch("a child past the deadline is a failure");
            } catch (PlatformException late) {
                check(late.getMessage() != null, "failed");
            }
            long tookMillis = (System.nanoTime() - begin) / 1_000_000;
            check(!alive(pid), "SIGKILL ended it");
            check(tookMillis < 600 + 2 * ChildProcess.GRACE_MILLIS + 2_000, "bounded: " + tookMillis + " ms");
        });
        row("child-is-reaped-when-the-run-is-interrupted", () -> {
            java.io.File pid = pidFile();
            Throwable[] ended = new Throwable[1];
            Thread runner = new Thread(() -> {
                try {
                    ChildProcess.run(sh("echo $$ > \"$1\"; exec sleep 60", pid.getPath()), 30_000, 4096);
                } catch (Throwable problem) {
                    ended[0] = problem;
                }
            });
            runner.start();
            while (pid.length() == 0 && runner.isAlive()) {
                Thread.sleep(20);
            }
            Thread.sleep(100);
            runner.interrupt();
            runner.join(10_000);
            check(!runner.isAlive(), "the run ended");
            check(ended[0] instanceof InterruptedException, "by the interrupt: " + ended[0]);
            check(!alive(pid), "the child is gone");
        });
        row("child-that-cannot-start-fails", () -> {
            try {
                ChildProcess.run(Arrays.asList("/nonexistent/kernarium-apps-test"), 1_000, 4096);
                throw new Mismatch("no such program");
            } catch (java.io.IOException expected) {
                check(true, "an exception, not a result");
            }
        });
    }

    /** A graph of resources by id, for the source bound's rows. */
    private static final class FakeGraph implements SourceBound.Graph {
        final Map<Integer, SourceBound.Node> nodes = new HashMap<>();
        final List<Integer> asked = new ArrayList<>();

        FakeGraph with(int id, SourceBound.Node node) {
            nodes.put(id, node);
            return this;
        }

        @Override
        public SourceBound.Node node(int id) throws Exception {
            asked.add(id);
            SourceBound.Node node = nodes.get(id);
            if (node == null) {
                throw new java.io.IOException("no resource " + id);
            }
            return node;
        }
    }

    private static boolean within(FakeGraph graph, int root) throws Exception {
        return SourceBound.within(graph, root, 640);
    }

    private static void sources() {
        row("source-bound-one-bitmap", () -> {
            int none = SourceBound.DENSITY_NONE;
            check(within(new FakeGraph().with(1, SourceBound.Node.bitmap(4096, 4096, none)), 1), "4096 x 4096 never scaled is 16 Mi pixels");
            check(!within(new FakeGraph().with(1, SourceBound.Node.bitmap(4097, 4096, none)), 1), "one column more is over");
            check(!within(new FakeGraph().with(1, SourceBound.Node.bitmap(20_000, 20_000, none)), 1), "a bitmap of 400 Mpixels");
            check(!within(new FakeGraph().with(1, SourceBound.Node.bitmap(Integer.MAX_VALUE, Integer.MAX_VALUE, 0)), 1),
                    "dimensions that overflow a product");
            check(within(new FakeGraph().with(1, SourceBound.Node.bitmap(192, 192, 640)), 1), "an icon as drawn");
            check(within(new FakeGraph().with(1, SourceBound.Node.other()), 1), "a colour");
            check(within(new FakeGraph(), 0), "no resource");
        });
        row("source-bound-follows-the-density", () -> {
            // The framework scales an image resource to the density it is loaded for: a file with no density
            // (the default, 160) at 640 is four times as wide and four times as tall.
            check(within(new FakeGraph().with(1, SourceBound.Node.bitmap(1024, 1024, 0)), 1), "1024 x 1024 at 160 becomes 4096 x 4096");
            check(!within(new FakeGraph().with(1, SourceBound.Node.bitmap(1025, 1024, 0)), 1), "one column more is over");
            check(!within(new FakeGraph().with(1, SourceBound.Node.bitmap(2048, 2048, 160)), 1), "declared 160 is the default");
            check(within(new FakeGraph().with(1, SourceBound.Node.bitmap(2048, 2048, 320)), 1), "xhdpi at 640 is twice");
            check(within(new FakeGraph().with(1, SourceBound.Node.bitmap(4096, 4096, SourceBound.DENSITY_NONE)), 1),
                    "nodpi is not scaled");
        });
        row("source-bound-layers-add-up", () -> {
            int none = SourceBound.DENSITY_NONE;
            FakeGraph adaptive = new FakeGraph().with(1, SourceBound.Node.xml(2, 3))
                    .with(2, SourceBound.Node.other()).with(3, SourceBound.Node.xml(4)).with(4, SourceBound.Node.bitmap(432, 432, none));
            check(within(adaptive, 1), "an adaptive icon with a colour background and a bitmap foreground");
            FakeGraph big = new FakeGraph().with(1, SourceBound.Node.xml(2, 3, 4, 5, 6))
                    .with(2, SourceBound.Node.bitmap(4096, 2048, none)).with(3, SourceBound.Node.bitmap(4096, 2048, none))
                    .with(4, SourceBound.Node.bitmap(4096, 2048, none)).with(5, SourceBound.Node.bitmap(4096, 2048, none))
                    .with(6, SourceBound.Node.bitmap(10, 10, none));
            check(!within(big, 1), "four bitmaps of 8 Mpixels are 32 Mpixels and the fifth thing tips it over");
            FakeGraph fits = new FakeGraph().with(1, SourceBound.Node.xml(2, 3, 4))
                    .with(2, SourceBound.Node.bitmap(4096, 2048, none)).with(3, SourceBound.Node.bitmap(4096, 2048, none))
                    .with(4, SourceBound.Node.bitmap(4096, 2048, none));
            check(within(fits, 1), "three of them (24 Mpixels) are within the total");
            FakeGraph exact = new FakeGraph().with(1, SourceBound.Node.xml(2, 3))
                    .with(2, SourceBound.Node.bitmap(4096, 4096, none)).with(3, SourceBound.Node.bitmap(4096, 4096, none));
            check(within(exact, 1), "two of 16 Mpixels are exactly the total");
            FakeGraph over = new FakeGraph().with(1, SourceBound.Node.xml(2, 3, 4))
                    .with(2, SourceBound.Node.bitmap(4096, 4096, none)).with(3, SourceBound.Node.bitmap(4096, 4096, none))
                    .with(4, SourceBound.Node.bitmap(1, 1, none));
            check(!within(over, 1), "one pixel more than the total");
        });
        row("source-bound-walk-is-bounded", () -> {
            FakeGraph cycle = new FakeGraph().with(1, SourceBound.Node.xml(2)).with(2, SourceBound.Node.xml(1, 2, 1));
            check(within(cycle, 1), "a cycle ends");
            same(Arrays.asList(1, 2), cycle.asked, "each resource is looked at once");
            FakeGraph chain = new FakeGraph();
            for (int id = 1; id <= 10; id++) {
                chain.with(id, SourceBound.Node.xml(id + 1));
            }
            chain.with(11, SourceBound.Node.other());
            check(!within(chain, 1), "a chain deeper than 6 is over");
            FakeGraph shallow = new FakeGraph();
            for (int id = 1; id <= 7; id++) {
                shallow.with(id, id == 7 ? SourceBound.Node.other() : SourceBound.Node.xml(id + 1));
            }
            check(within(shallow, 1), "a chain of exactly the depth is within");
            FakeGraph wide = new FakeGraph();
            int[] ids = new int[32];
            for (int i = 0; i < ids.length; i++) {
                ids[i] = 100 + i;
                wide.with(ids[i], SourceBound.Node.bitmap(10, 10, SourceBound.DENSITY_NONE));
            }
            wide.with(1, SourceBound.Node.xml(ids));
            check(!within(wide, 1), "the root and 32 files are 33 resources, over 32");
            FakeGraph many = new FakeGraph();
            int[] some = new int[31];
            for (int i = 0; i < some.length; i++) {
                some[i] = 100 + i;
                many.with(some[i], SourceBound.Node.bitmap(10, 10, SourceBound.DENSITY_NONE));
            }
            many.with(1, SourceBound.Node.xml(some));
            check(within(many, 1), "the root and 31 files are exactly 32 resources, within");
        });
        row("source-bound-colours-are-free", () -> {
            // A vector with many named colours is not a big icon: only the files count towards the bound.
            FakeGraph colours = new FakeGraph();
            int[] ids = new int[100];
            for (int i = 0; i < ids.length; i++) {
                ids[i] = 1000 + i;
                colours.with(ids[i], SourceBound.Node.other());
            }
            colours.with(1, SourceBound.Node.xml(ids));
            check(within(colours, 1), "a hundred colours");
        });
        row("source-bound-unreadable-is-an-error", () -> {
            try {
                within(new FakeGraph().with(1, SourceBound.Node.xml(2)), 1);
                throw new Mismatch("a resource that cannot be read");
            } catch (java.io.IOException expected) {
                check(true, "thrown");
            }
            try {
                within(new FakeGraph().with(1, SourceBound.Node.bitmap(0, 0, 0)), 1);
                throw new Mismatch("a bitmap with no size");
            } catch (java.io.IOException expected) {
                check(true, "thrown");
            }
            try {
                within(new FakeGraph().with(1, SourceBound.Node.bitmap(-1, -1, 0)), 1);
                throw new Mismatch("a bitmap whose bounds could not be read");
            } catch (java.io.IOException expected) {
                check(true, "thrown");
            }
        });
        row("resource-paths-follow-the-framework", () -> {
            List<ResourcePaths.Item> items = ResourcePaths.of("/data/app/a/base.apk", "/data/app/a/private.apk",
                    new String[] {"/data/app/a/split_one.apk", "", null, "/data/app/a/split_two.apk"},
                    new String[] {"/data/app/a/private_split.apk"},
                    new String[] {"/system/framework/lib.jar", "/data/app/lib/lib.apk", null},
                    new String[] {"/o/legacy.apk", "/o/both.idmap"},
                    new String[] {"/o/both.idmap", "/o/new.idmap"});
            same(Arrays.asList("BASE /data/app/a/base.apk", "SPLIT /data/app/a/split_one.apk", "SPLIT /data/app/a/split_two.apk",
                    "LIBRARY /data/app/lib/lib.apk", "OVERLAY /o/legacy.apk", "OVERLAY /o/both.idmap", "OVERLAY /o/new.idmap"),
                    describe(items), "base, splits, libraries (APKs only), overlays (legacy first, each once)");
            check(ResourcePaths.hasBase(items), "it can be opened");
            same(Arrays.asList("BASE /data/app/a/private.apk", "SPLIT /data/app/a/private_split.apk"),
                    describe(ResourcePaths.of(null, "/data/app/a/private.apk", null, new String[] {"/data/app/a/private_split.apk"},
                            null, null, null)), "the source directories when no public ones are given");
            check(!ResourcePaths.hasBase(ResourcePaths.of(null, null, new String[] {"/x.apk"}, null, null, null, null)),
                    "no base, no resources");
            check(!ResourcePaths.hasBase(ResourcePaths.of("", "", null, null, null, null, null)), "an empty base");
            String key = ResourcePaths.fingerprint(items);
            same(key, ResourcePaths.fingerprint(items), "stable");
            check(key.matches("[0-9a-f]{16}"), "16 hex characters");
            check(!key.equals(ResourcePaths.fingerprint(ResourcePaths.of("/data/app/a/base.apk", null, null, null, null, null, null))),
                    "it follows the list");
            check(!ResourcePaths.fingerprint(ResourcePaths.of("/a.apk", null, new String[] {"/b.apk"}, null, null, null, null))
                    .equals(ResourcePaths.fingerprint(ResourcePaths.of("/a.apk", null, null, null, new String[] {"/b.apk"}, null, null))),
                    "a split is not a library");
        });
    }

    private static List<String> describe(List<ResourcePaths.Item> items) {
        List<String> described = new ArrayList<>();
        for (ResourcePaths.Item item : items) {
            described.add(item.kind + " " + item.path);
        }
        return described;
    }

    private static void sizes() {
        row("worst-case-answers-fit-the-host-limit", () -> {
            // The host reads at most 1 MiB of stdout (ShellV2's default limit): the three answers stay far below it.
            int row = 255 + 1 + 255 + 1 + 320 + 1 + 1 + 1 + 16 + 1;
            check(row <= 860, "a row is at most 860 bytes: " + row);
            check(1024L * row + 1024 < 900 * 1024, "1024 rows");
            check(8L * (AppsCommand.MAX_PNG_BYTES + 700) < 800 * 1024, "8 icons");
        });
    }
}
