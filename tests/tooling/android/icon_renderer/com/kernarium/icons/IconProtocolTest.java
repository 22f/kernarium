// SPDX-License-Identifier: GPL-2.0-or-later
//
// What the shell tool (kernarium_apps) and the icon renderer (KernariumIcons) agree on, on a plain JVM: the shell
// user's uid, the shape of a request (the resource id and the files, in the order the framework adds them), a
// path's rules, the sizes and bounds, and the layers' outset and alpha rule.
// Each row prints "ok <name>" or "FAIL <name>: why"; the last line is PASS when no row failed.
// test_icon_renderer.py compiles and runs it, and runs it again on a copy of IconProtocol with one rule removed
// at a time, where the rows named there must fail.

package com.kernarium.icons;

import java.util.Arrays;

public final class IconProtocolTest {
    private static final class Mismatch extends RuntimeException {
        private static final long serialVersionUID = 1L;

        Mismatch(String message) {
            super(message);
        }
    }

    private interface Row {
        void run() throws Exception;
    }

    private static int failures;

    private static void row(String name, Row body) {
        try {
            body.run();
            System.out.println("ok " + name);
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
        if (!expected.equals(actual)) {
            throw new Mismatch(what + ": expected " + expected + ", got " + actual);
        }
    }

    private static String[] paths(int count) {
        String[] paths = new String[count];
        for (int i = 0; i < count; i++) {
            paths[i] = "/data/app/p" + i + "/base.apk";
        }
        return paths;
    }

    private static boolean valid(int[] kinds) {
        return IconProtocol.validRequest(0x7f010000, kinds, paths(kinds.length));
    }

    private static String repeat(char c, int count) {
        char[] chars = new char[count];
        Arrays.fill(chars, c);
        return new String(chars);
    }

    public static void main(String[] args) {
        names();
        shell();
        request();
        paths();
        sizes();
        drawing();
        System.out.println(failures == 0 ? "PASS" : "FAILED " + failures);
        System.exit(failures == 0 ? 0 : 1);
    }

    private static void names() {
        row("protocol-names-are-the-contract", () -> {
            same("com.kernarium.icons", IconProtocol.PACKAGE, "package");
            same("com.kernarium.icons", IconProtocol.AUTHORITY, "authority");
            same("com.kernarium.icons.IconRenderService", IconProtocol.SERVICE_CLASS, "service");
            same("render", IconProtocol.METHOD_RENDER, "method");
            same(0, IconProtocol.STATUS_OK, "ok");
            same(1, IconProtocol.STATUS_ERROR, "error");
            same(Arrays.asList(0, 1, 2, 3), Arrays.asList(IconProtocol.KIND_BASE, IconProtocol.KIND_SPLIT,
                    IconProtocol.KIND_LIBRARY, IconProtocol.KIND_OVERLAY), "the kinds, in the order the framework adds them");
            check(new java.util.HashSet<>(Arrays.asList(IconProtocol.KEY_RESOURCE, IconProtocol.KEY_KINDS,
                    IconProtocol.KEY_PATHS, IconProtocol.KEY_STATUS, IconProtocol.KEY_SIZE, IconProtocol.KEY_ADAPTIVE,
                    IconProtocol.KEY_ALPHA, IconProtocol.KEY_PNG)).size() == 8, "eight keys, all different");
        });
    }

    private static void shell() {
        row("protocol-shell-uid", () -> {
            check(IconProtocol.fromShell(2000), "the shell user");
            check(IconProtocol.fromShell(1002000), "the shell user of another Android user");
            check(IconProtocol.fromShell(102000), "the same app id in a user's range");
            for (int other : new int[] {0, 1000, 1001, 1999, 2001, 10000, 10125, 12000, 99000, 100000, 1000000, 99009, -1,
                -2000, -98000}) {
                check(!IconProtocol.fromShell(other), "uid " + other + " is not the shell user's");
            }
            same(2000, IconProtocol.SHELL_APP_ID, "the shell app id");
            same(100000, IconProtocol.PER_USER_RANGE, "the range of one user");
        });
    }

    private static void request() {
        row("request-shapes-that-are-accepted", () -> {
            check(valid(new int[] {0}), "a base alone");
            check(valid(new int[] {0, 1, 1, 2, 3, 3}), "base, splits, library, overlays in that order");
            check(valid(new int[] {0, 3}), "a base and an overlay");
            check(valid(new int[] {0, 2}), "a base and a library");
            int[] most = new int[128];
            Arrays.fill(most, 1, most.length, 3);
            check(valid(most), "128 files");
            check(IconProtocol.validRequest(Integer.MAX_VALUE, new int[] {0}, paths(1)), "the largest resource id");
            check(IconProtocol.validRequest(1, new int[] {0}, paths(1)), "the smallest resource id");
        });
        row("request-needs-a-resource", () -> {
            check(!IconProtocol.validRequest(0, new int[] {0}, paths(1)), "resource 0 is no resource");
            check(!IconProtocol.validRequest(-1, new int[] {0}, paths(1)), "a negative resource id");
            check(!IconProtocol.validRequest(Integer.MIN_VALUE, new int[] {0}, paths(1)), "the least resource id");
        });
        row("request-arrays-agree", () -> {
            check(!IconProtocol.validRequest(1, null, paths(1)), "no kinds");
            check(!IconProtocol.validRequest(1, new int[] {0}, null), "no paths");
            check(!IconProtocol.validRequest(1, new int[] {0, 3}, paths(1)), "more kinds than paths");
            check(!IconProtocol.validRequest(1, new int[] {0}, paths(2)), "more paths than kinds");
            check(!IconProtocol.validRequest(1, new int[0], new String[0]), "no files");
        });
        row("request-file-count-is-bounded", () -> {
            int[] over = new int[129];
            Arrays.fill(over, 1, over.length, 3);
            check(!valid(over), "129 files");
        });
        row("request-starts-with-exactly-one-base", () -> {
            check(!valid(new int[] {1}), "a split first");
            check(!valid(new int[] {3, 0}), "an overlay before the base");
            check(!valid(new int[] {2}), "a library first");
            check(!valid(new int[] {0, 0}), "two bases");
            check(!valid(new int[] {0, 1, 0}), "a base after a split");
        });
        row("request-kinds-are-the-known-ones", () -> {
            check(!valid(new int[] {0, 4}), "a kind past the overlays");
            check(!valid(new int[] {0, -1}), "a negative kind");
            check(!valid(new int[] {0, Integer.MAX_VALUE}), "an absurd kind");
        });
        row("request-order-is-the-frameworks", () -> {
            check(!valid(new int[] {0, 3, 1}), "a split after an overlay");
            check(!valid(new int[] {0, 2, 1}), "a split after a library");
            check(!valid(new int[] {0, 3, 2}), "a library after an overlay");
            check(valid(new int[] {0, 1, 2, 3}), "each kind once, in order");
        });
        row("request-names-valid-paths-only", () -> {
            for (String bad : new String[] {null, "", "/", "relative/base.apk", "/a/../b.apk"}) {
                String[] one = {bad};
                check(!IconProtocol.validRequest(1, new int[] {0}, one), "the path " + bad);
            }
            String[] second = {"/a/base.apk", "../x"};
            check(!IconProtocol.validRequest(1, new int[] {0, 3}, second), "a bad path that is not the first");
        });
    }

    private static void paths() {
        row("path-accepts", () -> {
            for (String good : new String[] {"/a", "/data/app/~~x==/com.a-y==/base.apk", "/a b", "/a/..b", "/a/.hidden",
                "/a/b..", "/product/overlay/A.apk", "/~", "/" + repeat('a', 1023)}) {
                check(IconProtocol.validPath(good), "the path " + good.substring(0, Math.min(40, good.length())));
            }
        });
        row("path-is-absolute-and-not-empty", () -> {
            check(!IconProtocol.validPath(null), "null");
            check(!IconProtocol.validPath(""), "the empty path");
            check(!IconProtocol.validPath("/"), "the root alone");
            check(!IconProtocol.validPath("a"), "one character, relative");
            check(!IconProtocol.validPath("ab"), "two characters, relative");
            check(!IconProtocol.validPath("data/app/base.apk"), "relative");
        });
        row("path-length-is-bounded", () -> {
            check(!IconProtocol.validPath("/" + repeat('a', 1024)), "1025 characters");
            same(1024, IconProtocol.MAX_PATH_CHARS, "the longest path");
        });
        row("path-is-printable-ascii", () -> {
            for (char bad : new char[] {0, 0x09, 0x0A, 0x0D, 0x1F, 0x7F, 0x80, 0xA0, 0xE9, 0x2028, 0xFFFF}) {
                check(!IconProtocol.validPath("/a" + bad + "b"), "the character U+" + Integer.toHexString(bad));
            }
            check(IconProtocol.validPath("/a b"), "a space is printable");
            check(IconProtocol.validPath("/a~b"), "a tilde is printable");
            check(!IconProtocol.validPath("/a😀b"), "an emoji");
        });
        row("path-has-no-empty-or-dot-segments", () -> {
            for (String bad : new String[] {"//a", "/a//b", "/a/", "/a/./b", "/./a", "/a/.", "/a/../b", "/../a", "/a/.."}) {
                check(!IconProtocol.validPath(bad), "the path " + bad);
            }
        });
    }

    private static void sizes() {
        row("sizes-and-bounds", () -> {
            same(Arrays.asList(192, 128, 96), Arrays.asList(IconProtocol.SIZES[0], IconProtocol.SIZES[1], IconProtocol.SIZES[2]),
                    "the sizes, largest first");
            same(3, IconProtocol.SIZES.length, "three sizes");
            same(98304, IconProtocol.MAX_PNG_BYTES, "the PNG bound");
            same(128, IconProtocol.MAX_FILES, "the file bound");
            same(640, IconProtocol.DENSITY, "the density");
        });
        row("size-steps-down-through-the-list", () -> {
            same(128, IconProtocol.smaller(192), "after 192");
            same(96, IconProtocol.smaller(128), "after 128");
            same(0, IconProtocol.smaller(96), "after 96 there is none");
            same(0, IconProtocol.smaller(100), "a size that is not one of them");
            same(0, IconProtocol.smaller(0), "zero");
            same(0, IconProtocol.smaller(256), "a larger one");
        });
    }

    private static void drawing() {
        row("layer-outset-is-a-quarter", () -> {
            same(48, IconProtocol.outset(192), "at 192");
            same(32, IconProtocol.outset(128), "at 128");
            same(24, IconProtocol.outset(96), "at 96");
            same(0, IconProtocol.outset(0), "at 0");
        });
        row("alpha-is-any-pixel-not-opaque", () -> {
            check(!IconProtocol.hasAlpha(new int[0]), "no pixels");
            check(!IconProtocol.hasAlpha(new int[] {0xFF000000, 0xFFFFFFFF, 0xFF123456}), "all opaque");
            check(IconProtocol.hasAlpha(new int[] {0xFF000000, 0x00000000}), "one transparent");
            check(IconProtocol.hasAlpha(new int[] {0xFF000000, 0xFEFFFFFF}), "one pixel barely transparent");
            check(IconProtocol.hasAlpha(new int[] {0x80FF0000}), "a half transparent pixel");
            check(IconProtocol.hasAlpha(new int[] {0x01FFFFFF}), "an almost invisible one");
        });
    }
}
