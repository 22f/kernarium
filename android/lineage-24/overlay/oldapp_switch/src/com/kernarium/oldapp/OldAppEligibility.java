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

import java.util.Collection;
import java.util.regex.Pattern;

/**
 * The one place that says which applications Old app compatibility applies to, where the choice
 * is stored, and when a stored choice no longer belongs to the application it was made for.
 *
 * <p>An application is eligible when it is not a system application, its primary ABI is a 32-bit
 * one, it targets an API level below 14 (Android before 4.0), it does not share a user ID and
 * its package name can be the end of a property name as it is. The framework hook
 * (frameworks/base, ActivityThread.handleBindApplication) applies the same rule to the
 * application it is starting, in the order that keeps the common case cheap: the target level,
 * then the process's bitness, then the property of {@link #propertyName}. It does not need the
 * other tests, which the command applies before it lets a property stand: the listing clears a
 * choice that {@link #outlived} says is no longer the application's.
 */
final class OldAppEligibility {
    /** Android 4.0, Build.VERSION_CODES.ICE_CREAM_SANDWICH: a lower target level is "old". */
    static final int TARGET_LEVEL_BELOW = 14;

    /** The store: one persistent property per package, its value the turn-on time in epoch seconds. */
    static final String STORE_PREFIX = "persist.sys.kernarium.oldapp.";

    /**
     * What the property service accepts after the prefix (init's IsLegalPropertyName): letters,
     * digits and _ @ : -, in segments that no dot leaves empty, so no leading, trailing or
     * doubled dot. The package manager accepts a few names that break it (org.example..app).
     */
    private static final Pattern PROPERTY_KEY_PART =
            Pattern.compile("[A-Za-z0-9_@:-]+(\\.[A-Za-z0-9_@:-]+)*");

    private OldAppEligibility() {}

    /** @param abis32 the 32-bit ABIs of the device (Build.SUPPORTED_32_BIT_ABIS). */
    static boolean isEligible(Candidate candidate, Collection<String> abis32) {
        return !candidate.system
                && candidate.sharedUserId == null
                && candidate.targetSdkVersion < TARGET_LEVEL_BELOW
                && candidate.primaryCpuAbi != null
                && abis32.contains(candidate.primaryCpuAbi)
                && isPropertyKeyPart(candidate.packageName);
    }

    /** Whether the text can follow {@link #STORE_PREFIX} in a property name, as it is. */
    static boolean isPropertyKeyPart(String text) {
        return PROPERTY_KEY_PART.matcher(text).matches();
    }

    static String propertyName(String packageName) {
        return STORE_PREFIX + packageName;
    }

    /**
     * Whether a stored turn-on no longer belongs to the installation it was made for: its
     * package is gone, is not eligible any more (an update that raised the target level, a
     * reinstall with a shared user ID), or was first installed after the turn-on time (the
     * application was uninstalled and installed again). A value that is not a time is left alone.
     *
     * @param candidate the installed package, or null when it is not installed.
     * @param turnedOn the stored value.
     */
    static boolean outlived(Candidate candidate, Collection<String> abis32, String turnedOn) {
        if (candidate == null || !isEligible(candidate, abis32)) {
            return true;
        }
        try {
            return candidate.firstInstallSeconds > Long.parseLong(turnedOn.trim());
        } catch (NumberFormatException notATime) {
            return false;
        }
    }
}
