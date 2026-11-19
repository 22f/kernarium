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

import android.app.ActivityManager;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.pm.ResolveInfo;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.AdaptiveIconDrawable;
import android.graphics.drawable.Drawable;
import android.os.LocaleList;
import android.os.ServiceManager;
import android.os.UserHandle;
import android.util.DisplayMetrics;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.TimeUnit;

/**
 * {@link AppsCommand.Platform} on Android, for a process that runs as the adb shell user: the package
 * manager through its binder interface (the foreground user's applications), the labels and icons
 * from each application's own resources (the shell user may read any application's package), and
 * {@code cmd activity start-activity} for the start. Nothing is written.
 */
final class AndroidPlatform implements AppsCommand.Platform {
    private static final int OUTPUT_LIMIT = 4096;
    /** The density icons are loaded for: the largest, so that the best bitmap the application has is the one used. */
    private static final int ICON_DENSITY = DisplayMetrics.DENSITY_XXXHIGH;
    /** An adaptive icon's layers are drawn 108 dp for the 72 dp window the launcher shows: a quarter more on each side. */
    private static final float LAYER_OUTSET = 0.25f;

    private final IPackageManager mPackageManager;
    private final Map<String, PackageInfo> mPackages = new HashMap<>();
    private final Map<String, Boolean> mPermissions = new HashMap<>();
    private final Map<String, Resources> mLabelResources = new HashMap<>();
    private final Map<String, Resources> mIconResources = new HashMap<>();
    private Configuration mConfiguration;

    AndroidPlatform() {
        mPackageManager = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
        if (mPackageManager == null) {
            throw new IllegalStateException("the package manager is not running");
        }
    }

    @Override
    public int currentUser() {
        try {
            return ActivityManager.getCurrentUser();
        } catch (RuntimeException problem) {
            // Not told (the activity manager refused the question): the user this process runs for, which
            // is the owner on a device with one user.
            return UserHandle.myUserId();
        }
    }

    @Override
    public List<Entry> launcherActivities(int user) throws Exception {
        Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        long flags = PackageManager.MATCH_DIRECT_BOOT_AWARE | PackageManager.MATCH_DIRECT_BOOT_UNAWARE;
        ParceledListSlice<ResolveInfo> slice = mPackageManager.queryIntentActivities(intent, null, flags, user);
        List<Entry> result = new ArrayList<>();
        if (slice == null) {
            return result;
        }
        for (ResolveInfo resolved : slice.getList()) {
            ActivityInfo info = resolved.activityInfo;
            if (info == null || info.applicationInfo == null || info.packageName == null || info.name == null) {
                continue;
            }
            PackageInfo packageInfo = packageInfo(info.packageName, user);
            result.add(new Entry(info.packageName, info.name,
                    (info.applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) != 0, info.exported,
                    info.isEnabled(), permitted(info.permission),
                    packageInfo == null ? 0 : packageInfo.getLongVersionCode(),
                    packageInfo == null ? 0 : packageInfo.lastUpdateTime, info.getIconResource(), info));
        }
        return result;
    }

    private PackageInfo packageInfo(String packageName, int user) {
        if (!mPackages.containsKey(packageName)) {
            PackageInfo info = null;
            try {
                info = mPackageManager.getPackageInfo(packageName, 0L, user);
            } catch (Exception problem) {
                // The version stays 0: the icon key then does not follow this package's updates.
            }
            mPackages.put(packageName, info);
        }
        return mPackages.get(packageName);
    }

    /** Whether the shell user holds the permission an activity asks for (none asked is permitted). */
    private boolean permitted(String permission) {
        if (permission == null) {
            return true;
        }
        Boolean known = mPermissions.get(permission);
        if (known == null) {
            boolean granted = false;
            try {
                granted = ActivityManager.getService().checkPermission(permission, android.os.Process.myPid(),
                        android.os.Process.myUid())
                        == PackageManager.PERMISSION_GRANTED;
            } catch (Exception problem) {
                // Not told: the activity is not offered.
            }
            known = granted;
            mPermissions.put(permission, known);
        }
        return known;
    }

    @Override
    public List<String> localeTags() {
        List<String> tags = new ArrayList<>();
        String joined = configuration().getLocales().toLanguageTags();
        tags.addAll(Arrays.asList(joined.split(",")));
        return tags;
    }

    @Override
    public String label(Entry entry) {
        ActivityInfo info = (ActivityInfo) entry.source;
        ApplicationInfo application = info.applicationInfo;
        // The launcher's order: the activity's label, then the application's.
        CharSequence text = text(info.nonLocalizedLabel, info.labelRes, application);
        if (text == null) {
            text = text(application.nonLocalizedLabel, application.labelRes, application);
        }
        return text == null ? null : text.toString();
    }

    private CharSequence text(CharSequence literal, int resource, ApplicationInfo application) {
        if (literal != null) {
            return literal;
        }
        if (resource == 0) {
            return null;
        }
        Resources resources = resources(application, false);
        return resources == null ? null : resources.getText(resource, null);
    }

    @Override
    public Icon icon(Entry entry, int size) throws Exception {
        ActivityInfo info = (ActivityInfo) entry.source;
        Resources resources = resources(info.applicationInfo, true);
        if (resources == null) {
            throw new IllegalStateException("the application's resources cannot be opened");
        }
        Drawable drawable = resources.getDrawableForDensity(entry.iconResource, ICON_DENSITY, null);
        if (drawable == null) {
            throw new IllegalStateException("the icon is not a drawable");
        }
        Bitmap bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        try {
            Canvas canvas = new Canvas(bitmap);
            boolean adaptive = drawable instanceof AdaptiveIconDrawable;
            if (adaptive) {
                // The layers over the window the launcher shows, with no mask: iOS puts its own on.
                AdaptiveIconDrawable icon = (AdaptiveIconDrawable) drawable;
                int outset = Math.round(size * LAYER_OUTSET);
                for (Drawable layer : new Drawable[] {icon.getBackground(), icon.getForeground()}) {
                    if (layer != null) {
                        layer.setBounds(-outset, -outset, size + outset, size + outset);
                        layer.draw(canvas);
                    }
                }
            } else {
                drawable.setBounds(0, 0, size, size);
                drawable.draw(canvas);
            }
            int[] pixels = new int[size * size];
            bitmap.getPixels(pixels, 0, size, 0, 0, size, size);
            boolean hasAlpha = false;
            for (int pixel : pixels) {
                if ((pixel >>> 24) != 0xFF) {
                    hasAlpha = true;
                    break;
                }
            }
            ByteArrayOutputStream png = new ByteArrayOutputStream();
            if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, png)) {
                throw new IllegalStateException("the icon could not be encoded");
            }
            return new Icon(size, adaptive, hasAlpha, png.toByteArray());
        } finally {
            bitmap.recycle();
        }
    }

    /**
     * The application's own resources (every APK of it) in the system's configuration, for the
     * label, or at the icon density, for the icon. One per application and use.
     */
    private Resources resources(ApplicationInfo application, boolean icon) {
        Map<String, Resources> cache = icon ? mIconResources : mLabelResources;
        if (!cache.containsKey(application.packageName)) {
            Resources resources = null;
            AssetManager assets = new AssetManager();
            boolean added = application.sourceDir != null && assets.addAssetPath(application.sourceDir) != 0;
            if (added && application.splitSourceDirs != null) {
                for (String split : application.splitSourceDirs) {
                    assets.addAssetPath(split);
                }
            }
            if (added) {
                DisplayMetrics metrics = new DisplayMetrics();
                metrics.setToDefaults();
                Configuration configuration = new Configuration(configuration());
                if (icon) {
                    metrics.densityDpi = ICON_DENSITY;
                    metrics.density = ICON_DENSITY / (float) DisplayMetrics.DENSITY_DEFAULT;
                    configuration.densityDpi = ICON_DENSITY;
                }
                resources = new Resources(assets, metrics, configuration);
            } else {
                assets.close();
            }
            cache.put(application.packageName, resources);
        }
        return cache.get(application.packageName);
    }

    /** The system's current configuration (its locales above all), or the process's default one. */
    private Configuration configuration() {
        if (mConfiguration == null) {
            Configuration configuration = null;
            try {
                configuration = ActivityManager.getService().getConfiguration();
            } catch (Exception problem) {
                // The default locales below are what a process of this uid has.
            }
            if (configuration == null) {
                configuration = new Configuration();
                configuration.setLocales(LocaleList.getDefault());
            }
            mConfiguration = configuration;
        }
        return mConfiguration;
    }

    @Override
    public AppsCommand.StartResult start(int user, Component component) throws Exception {
        Process process = new ProcessBuilder(AppsCommand.startArguments(user, component))
                .redirectErrorStream(true).start();
        process.getOutputStream().close();
        // The command prints a line or two (the pipe holds far more), so the wait comes first and has the bound.
        if (!process.waitFor(AppsCommand.START_SECONDS, TimeUnit.SECONDS)) {
            process.destroy();
            throw new IllegalStateException("cmd activity start-activity did not finish");
        }
        StringBuilder output = new StringBuilder();
        try (InputStream in = process.getInputStream()) {
            byte[] buffer = new byte[OUTPUT_LIMIT];
            int n = in.read(buffer);
            if (n > 0) {
                output.append(new String(buffer, 0, n, StandardCharsets.UTF_8));
            }
        }
        return new AppsCommand.StartResult(process.exitValue(), output.toString());
    }

    @Override
    public long nowMillis() {
        return System.currentTimeMillis();
    }
}
