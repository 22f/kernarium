// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package com.kernarium.oldapp;

import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/**
 * The commands of {@code kernarium_oldapp}: lists the applications Old app compatibility can apply
 * to and turns it on or off for one. {@link Main} runs them on Android, as the adb shell user (see
 * the wrapper script of the same name); the choice is the property
 * {@link OldAppEligibility#propertyName}.
 *
 * <pre>
 * kernarium_oldapp list                       kernarium_oldapp 1, then one line per eligible application:
 *                                             package TAB label TAB on|off TAB turned-on TAB last-update
 * kernarium_oldapp set PACKAGE on|off         PACKAGE TAB on|off; exit 2 for an application that is not eligible
 * </pre>
 *
 * Exit status: 0 done, 1 any other failure, 2 refused (not eligible), 64 usage. The format grows
 * by trailing fields; {@link #FORMAT_VERSION} changes when a field's meaning does.
 */
final class OldAppCommand {
    static final int FORMAT_VERSION = 1;
    static final String NAME = "kernarium_oldapp";
    static final int EXIT_OK = 0;
    static final int EXIT_FAILURE = 1;
    static final int EXIT_REFUSED = 2;
    static final int EXIT_USAGE = 64;

    /** The platform the commands stand on; the Android one is {@link AndroidPlatform}. */
    interface Platform {
        /** The installed packages of the current user. */
        List<Candidate> installedPackages() throws Exception;

        /** One installed package, or null. */
        Candidate find(String packageName) throws Exception;

        /** The device's 32-bit ABIs. */
        List<String> abis32();

        /** The application's label in the system's current language. */
        String label(Candidate candidate) throws Exception;

        /** The system property's value, or the empty string. */
        String getProperty(String name);

        /** Sets a system property, and throws if the system did not take it. */
        void setProperty(String name, String value) throws Exception;

        /** Stops the application's processes, as {@code am force-stop} does. */
        void forceStop(String packageName) throws Exception;

        /** The system's clock in epoch seconds. */
        long nowSeconds();
    }

    private OldAppCommand() {}

    static int run(String[] args, Platform platform, PrintWriter out, PrintWriter err) {
        if (args.length == 1 && args[0].equals("list")) {
            return list(platform, out, err);
        }
        if (args.length == 3 && args[0].equals("set") && OldAppEligibility.isPackageName(args[1])
                && (args[2].equals("on") || args[2].equals("off"))) {
            return set(platform, args[1], args[2].equals("on"), out, err);
        }
        err.print("usage: " + NAME + " list\n       " + NAME + " set PACKAGE on|off\n");
        return EXIT_USAGE;
    }

    private static int list(Platform platform, PrintWriter out, PrintWriter err) {
        try {
            List<String> abis32 = platform.abis32();
            List<Candidate> eligible = new ArrayList<>();
            for (Candidate candidate : platform.installedPackages()) {
                if (OldAppEligibility.isEligible(candidate, abis32)) {
                    eligible.add(candidate);
                }
            }
            Collections.sort(eligible, new Comparator<Candidate>() {
                @Override
                public int compare(Candidate a, Candidate b) {
                    return a.packageName.compareTo(b.packageName);
                }
            });
            StringBuilder text = new StringBuilder();
            text.append(NAME).append(' ').append(FORMAT_VERSION).append('\n');
            for (Candidate candidate : eligible) {
                String turnedOn = platform.getProperty(OldAppEligibility.propertyName(candidate.packageName));
                boolean on = !turnedOn.isEmpty();
                text.append(candidate.packageName).append('\t')
                        .append(oneLine(label(platform, candidate))).append('\t')
                        .append(on ? "on" : "off").append('\t')
                        .append(on ? oneLine(turnedOn) : "0").append('\t')
                        .append(candidate.lastUpdateSeconds).append('\n');
            }
            out.print(text);
            return EXIT_OK;
        } catch (Exception problem) {
            err.print(NAME + ": list failed: " + problem + "\n");
            return EXIT_FAILURE;
        }
    }

    private static int set(Platform platform, String packageName, boolean on, PrintWriter out,
            PrintWriter err) {
        try {
            Candidate candidate = platform.find(packageName);
            if (candidate == null || !OldAppEligibility.isEligible(candidate, platform.abis32())) {
                err.print(NAME + ": " + packageName + " is not an application Old app compatibility applies to\n");
                return EXIT_REFUSED;
            }
            String name = OldAppEligibility.propertyName(packageName);
            String value = on ? Long.toString(platform.nowSeconds()) : "";
            platform.setProperty(name, value);
            if (!platform.getProperty(name).equals(value)) {
                err.print(NAME + ": the system did not keep " + name + "\n");
                return EXIT_FAILURE;
            }
            // After the write: an application that starts again after it is stopped finds the new value.
            try {
                platform.forceStop(packageName);
            } catch (Exception problem) {
                err.print(NAME + ": " + packageName + " is " + (on ? "on" : "off")
                        + " but could not be stopped: " + problem + "\n");
                return EXIT_FAILURE;
            }
            out.print(packageName + "\t" + (on ? "on" : "off") + "\n");
            return EXIT_OK;
        } catch (Exception problem) {
            err.print(NAME + ": set failed: " + problem + "\n");
            return EXIT_FAILURE;
        }
    }

    private static String label(Platform platform, Candidate candidate) {
        String label = null;
        try {
            label = platform.label(candidate);
        } catch (Exception problem) {
            // An application whose label cannot be read is listed under its package name.
        }
        return label == null || label.isEmpty() ? candidate.packageName : label;
    }

    /** A TAB or a line break becomes a space, so the text stays inside its field. */
    static String oneLine(String text) {
        StringBuilder result = new StringBuilder(text.length());
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            boolean separator = c == '\t' || c == '\n' || c == '\r' || c == '\u000B' || c == '\f'
                    || c == '\u0085' || c == ' ' || c == ' ';
            result.append(separator ? ' ' : c);
        }
        return result.toString();
    }
}
