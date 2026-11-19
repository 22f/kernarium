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

package com.kernarium.apps;

/** Reads what {@code cmd activity start-activity} printed and how it ended. */
final class StartOutcome {
    /** The longest reason the tool prints. */
    static final int MAX_REASON = 200;

    private StartOutcome() {}

    /**
     * Why the start did not happen, as one printable ASCII line, or null when it did. The shell command
     * exits 0 for some refusals and prints an {@code Error} line, so the output is read as well as the
     * status. A task that was only brought to the front is a start: that is what a tap on an app that
     * is already open does.
     */
    static String failure(int exit, String output) {
        String error = null;
        for (String line : output.split("\n")) {
            String trimmed = line.trim();
            if (trimmed.startsWith("Error") || trimmed.contains("Permission Denial")
                    || trimmed.contains("SecurityException") || trimmed.startsWith("Exception")) {
                error = trimmed;
                break;
            }
        }
        if (exit == 0 && error == null) {
            return null;
        }
        String first = error;
        if (first == null) {
            for (String line : output.split("\n")) {
                if (!line.trim().isEmpty()) {
                    first = line.trim();
                    break;
                }
            }
        }
        String reason = "cmd activity start-activity " + (exit == 0 ? "reported" : "exited " + exit)
                + (first == null ? "" : ": " + first);
        return ascii(reason, MAX_REASON);
    }

    /** Printable ASCII only, one line, at most {@code limit} characters. */
    static String ascii(String text, int limit) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < text.length() && out.length() < limit; i++) {
            char c = text.charAt(i);
            out.append(c >= 0x20 && c < 0x7F ? c : '?');
        }
        return out.toString();
    }
}
