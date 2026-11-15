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
 * The one place that says which applications Old app compatibility applies to, and where the
 * choice is stored.
 *
 * <p>An application is eligible when it is not a system application, its primary ABI is a 32-bit
 * one, it targets an API level below 14 (Android before 4.0) and it does not share a user ID.
 * The framework hook (frameworks/base, ActivityThread.handleBindApplication) applies the same
 * rule to the application it is starting, in the order that keeps the common case cheap: the
 * target level, then the process's bitness, then the property of {@link #propertyName}. It does
 * not need the system and shared-user tests, which only the listing needs: a system application
 * is never listed, so its property is never set, and a user ID shared by packages of different
 * eligibility is the hook's own decision.
 */
final class OldAppEligibility {
    /** Android 4.0, Build.VERSION_CODES.ICE_CREAM_SANDWICH: a lower target level is "old". */
    static final int TARGET_LEVEL_BELOW = 14;

    /** The store: one persistent property per package, its value the turn-on time in epoch seconds. */
    static final String STORE_PREFIX = "persist.sys.kernarium.oldapp.";

    /**
     * A package name as the package manager accepts it, and so a legal property name part (letters,
     * digits, underscore and dots, with no empty segment).
     */
    private static final Pattern PACKAGE_NAME =
            Pattern.compile("[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z0-9_]+)*");

    private OldAppEligibility() {}

    /** @param abis32 the 32-bit ABIs of the device (Build.SUPPORTED_32_BIT_ABIS). */
    static boolean isEligible(Candidate candidate, Collection<String> abis32) {
        return !candidate.system
                && candidate.sharedUserId == null
                && candidate.targetSdkVersion < TARGET_LEVEL_BELOW
                && candidate.primaryCpuAbi != null
                && abis32.contains(candidate.primaryCpuAbi);
    }

    static boolean isPackageName(String text) {
        return PACKAGE_NAME.matcher(text).matches();
    }

    static String propertyName(String packageName) {
        return STORE_PREFIX + packageName;
    }
}
