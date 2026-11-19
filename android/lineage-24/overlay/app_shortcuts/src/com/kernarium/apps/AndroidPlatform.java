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
import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.drawable.AdaptiveIconDrawable;
import android.graphics.drawable.Drawable;
import android.os.Build;
import android.os.LocaleList;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.util.ArraySet;
import android.util.DisplayMetrics;
import android.util.TypedValue;

import org.xmlpull.v1.XmlPullParser;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * {@link AppsCommand.Platform} on Android, for a process that runs as the adb shell user: the package
 * manager through its binder interface (the foreground user's applications), the labels and icons
 * from each application's own resources (the shell user may read any application's package), and
 * {@code cmd activity start-activity} for the start. Nothing is written.
 *
 * <p>No question has a substitute answer: when the foreground user, a permission, the system's
 * configuration or a package's facts cannot be had, the command fails (status 1); a package that is gone
 * between the query and its facts is left out.
 */
final class AndroidPlatform implements AppsCommand.Platform {
    private static final int OUTPUT_LIMIT = 4096;
    /** The density icons are loaded for: the largest, so that the best bitmap the application has is the one used. */
    private static final int ICON_DENSITY = DisplayMetrics.DENSITY_XXXHIGH;
    /** An adaptive icon's layers are drawn 108 dp for the 72 dp window the launcher shows: a quarter more on each side. */
    private static final float LAYER_OUTSET = 0.25f;
    /** The locale icons are drawn for. Fixed, with the density and the night mode, so that one resource id is one picture. */
    private static final Locale ICON_LOCALE = Locale.US;
    /** The most elements an icon's XML resource may have, and the most drawables one XML resource may refer to. */
    private static final int MAX_XML_ELEMENTS = 4096;
    private static final int MAX_REFERENCES = 64;
    /** The system assets the framework builds (AssetManager.FRAMEWORK_APK_PATH and LINEAGE_APK_PATH). */
    private static final String FRAMEWORK_APK = "/system/framework/framework-res.apk";
    private static final String PLATFORM_APK = "/system/framework/org.lineageos.platform-res.apk";
    private static boolean sSystemAssetsBuilt;

    private final IPackageManager mPackageManager;
    private final Map<String, PackageInfo> mPackages = new HashMap<>();
    private final Map<String, Boolean> mPermissions = new HashMap<>();
    private final Map<String, Resources> mLabelResources = new HashMap<>();
    private final Map<String, Resources> mIconResources = new HashMap<>();
    private Configuration mConfiguration;

    AndroidPlatform() throws PlatformException {
        mPackageManager = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
        if (mPackageManager == null) {
            throw new PlatformException("the package manager is not running");
        }
    }

    @Override
    public int currentUser() throws PlatformException {
        try {
            return ActivityManager.getCurrentUser();
        } catch (RuntimeException problem) {
            // No substitute (not the user this process runs for): a start or a listing for the wrong
            // user is worse than none.
            throw new PlatformException("the foreground user is not known", problem);
        }
    }

    @Override
    public List<Entry> launcherActivities(int user) throws Exception {
        Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        long flags = PackageManager.MATCH_DIRECT_BOOT_AWARE | PackageManager.MATCH_DIRECT_BOOT_UNAWARE;
        ParceledListSlice<ResolveInfo> slice = mPackageManager.queryIntentActivities(intent, null, flags, user);
        if (slice == null) {
            throw new PlatformException("the package manager gave no answer");
        }
        List<Entry> result = new ArrayList<>();
        for (ResolveInfo resolved : slice.getList()) {
            ActivityInfo info = resolved.activityInfo;
            if (info == null || info.applicationInfo == null || info.packageName == null || info.name == null) {
                continue;
            }
            PackageInfo packageInfo = packageInfo(info.packageName, user);
            if (packageInfo == null) {
                continue; // removed since the query
            }
            ApplicationInfo application = info.applicationInfo;
            List<ResourcePaths.Item> paths = resourcePaths(application);
            // Enabled: the resolver's own answer. The query does not ask for disabled components, so every
            // activity it returns is enabled for the user, and ActivityInfo.isEnabled() (the manifest's flag
            // and the application's) would drop an alias that is disabled in the manifest and enabled since.
            boolean builtIn = (application.flags & ApplicationInfo.FLAG_SYSTEM) != 0;
            // A built-in application that was never updated has an update time that is its APK's file time (one
            // fixed date in every image) or the wall time of the first boot that scanned it, and the resource
            // fingerprint names paths and not content: the image's own build time is an input of its own.
            long imageBuild = builtIn && (application.flags & ApplicationInfo.FLAG_UPDATED_SYSTEM_APP) == 0
                    ? Build.TIME : 0;
            result.add(new Entry(info.packageName, info.name, builtIn,
                    info.exported, true, permitted(info.permission), packageInfo.getLongVersionCode(),
                    packageInfo.lastUpdateTime, imageBuild, info.getIconResource(), ResourcePaths.fingerprint(paths),
                    info));
        }
        return result;
    }

    /** The package's facts for the user, or null when it is not installed for them (cached, absence too). */
    private PackageInfo packageInfo(String packageName, int user) throws Exception {
        if (!mPackages.containsKey(packageName)) {
            mPackages.put(packageName, mPackageManager.getPackageInfo(packageName, 0L, user));
        }
        return mPackages.get(packageName);
    }

    /** Whether the shell user holds the permission an activity asks for (none asked is permitted). */
    private boolean permitted(String permission) throws PlatformException {
        if (permission == null) {
            return true;
        }
        Boolean known = mPermissions.get(permission);
        if (known == null) {
            try {
                known = ActivityManager.getService().checkPermission(permission, android.os.Process.myPid(),
                        android.os.Process.myUid()) == PackageManager.PERMISSION_GRANTED;
            } catch (Exception problem) {
                throw new PlatformException("a permission could not be checked", problem);
            }
            mPermissions.put(permission, known);
        }
        return known;
    }

    @Override
    public List<String> localeTags() throws PlatformException {
        String joined = configuration().getLocales().toLanguageTags();
        return new ArrayList<>(Arrays.asList(joined.split(",")));
    }

    @Override
    public String label(Entry entry) throws Exception {
        ActivityInfo info = (ActivityInfo) entry.source;
        ApplicationInfo application = info.applicationInfo;
        // The launcher's order: the activity's label, then the application's.
        CharSequence text = text(info.nonLocalizedLabel, info.labelRes, application);
        if (text == null) {
            text = text(application.nonLocalizedLabel, application.labelRes, application);
        }
        return text == null ? null : text.toString();
    }

    private CharSequence text(CharSequence literal, int resource, ApplicationInfo application) throws Exception {
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
            throw new PlatformException("the application's resources cannot be opened");
        }
        if (!SourceBound.within(new ResourceGraph(resources), entry.iconResource, ICON_DENSITY)) {
            throw new PlatformException("the icon's source is over the bound");
        }
        Drawable drawable = resources.getDrawableForDensity(entry.iconResource, ICON_DENSITY, null);
        if (drawable == null) {
            throw new PlatformException("the icon is not a drawable");
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
                throw new PlatformException("the icon could not be encoded");
            }
            return new Icon(size, adaptive, hasAlpha, png.toByteArray());
        } finally {
            bitmap.recycle();
        }
    }

    /** The files an application's resources come from, for the user the application info describes. */
    private static List<ResourcePaths.Item> resourcePaths(ApplicationInfo application) {
        return ResourcePaths.of(application.publicSourceDir, application.sourceDir, application.splitPublicSourceDirs,
                application.splitSourceDirs, application.sharedLibraryFiles, application.resourceDirs,
                application.overlayPaths);
    }

    /**
     * The application's own resources, every file its effective resource path names (the base, its splits, its
     * shared libraries and the overlays enabled for the user, as the framework adds them for the application
     * itself): in the fixed configuration with the system's locale list, for the label; in the same configuration
     * with a fixed locale, for the icon. One per application and use. Null when the base or a split cannot be
     * added (the label is then the package name and the icon an error); a shared library or an overlay that cannot
     * be added is skipped, as the framework does.
     */
    private Resources resources(ApplicationInfo application, boolean icon) throws PlatformException {
        Map<String, Resources> cache = icon ? mIconResources : mLabelResources;
        if (!cache.containsKey(application.packageName)) {
            List<ResourcePaths.Item> paths = resourcePaths(application);
            Resources resources = null;
            systemAssetsWithoutIdmaps();
            AssetManager assets = new AssetManager();
            boolean complete = ResourcePaths.hasBase(paths);
            for (int i = 0; complete && i < paths.size(); i++) {
                ResourcePaths.Item item = paths.get(i);
                int cookie;
                switch (item.kind) {
                    case LIBRARY:
                        cookie = assets.addAssetPathAsSharedLibrary(item.path);
                        break;
                    case OVERLAY:
                        cookie = assets.addOverlayPath(item.path);
                        break;
                    default:
                        cookie = assets.addAssetPath(item.path);
                        break;
                }
                complete = cookie != 0 || item.kind == ResourcePaths.Kind.OVERLAY
                        || item.kind == ResourcePaths.Kind.LIBRARY;
            }
            if (complete) {
                DisplayMetrics metrics = new DisplayMetrics();
                metrics.setToDefaults();
                metrics.densityDpi = ICON_DENSITY;
                metrics.density = ICON_DENSITY / (float) DisplayMetrics.DENSITY_DEFAULT;
                resources = new Resources(assets, metrics,
                        fixedConfiguration(icon ? new LocaleList(ICON_LOCALE) : configuration().getLocales()));
            } else {
                assets.close();
            }
            cache.put(application.packageName, resources);
        }
        return cache.get(application.packageName);
    }

    /**
     * Builds the framework's system assets (framework-res and the platform's own resources) before the first
     * AssetManager asks for them, without the static overlays that target the framework. A process the zygote did not
     * fork builds them on its first AssetManager and runs idmap2 to make those overlays' idmaps, which the shell domain
     * may not execute: a denial on every run. The overlays change the framework's own values, not an application's
     * label or icon. The assets and their order are the framework's. The manager is published last, once it holds its
     * assets; when anything fails on the way (an exception, a framework that is not as written here, an allocation
     * failure) the statics are put back as they were found and the framework's own initialisation runs at the first
     * AssetManager, denial and all. An allocation failure is also passed on, for the caller's own recovery, and the
     * next call tries again.
     */
    private static synchronized void systemAssetsWithoutIdmaps() {
        if (sSystemAssetsBuilt) {
            return;
        }
        sSystemAssetsBuilt = true;
        Field system = null;
        Field array = null;
        Field set = null;
        Object previousArray = null;
        Object previousSet = null;
        boolean touched = false;
        try {
            system = AssetManager.class.getDeclaredField("sSystem");
            array = AssetManager.class.getDeclaredField("sSystemApkAssets");
            set = AssetManager.class.getDeclaredField("sSystemApkAssetsSet");
            system.setAccessible(true);
            array.setAccessible(true);
            set.setAccessible(true);
            if (system.get(null) != null) {
                return; // already built
            }
            previousArray = array.get(null);
            previousSet = set.get(null);
            List<ApkAssets> assets = new ArrayList<>();
            assets.add(ApkAssets.loadFromPath(FRAMEWORK_APK, ApkAssets.PROPERTY_SYSTEM));
            if (new File(PLATFORM_APK).exists()) {
                assets.add(ApkAssets.loadFromPath(PLATFORM_APK, ApkAssets.PROPERTY_SYSTEM));
            }
            ApkAssets[] all = assets.toArray(new ApkAssets[0]);
            Constructor<AssetManager> constructor = AssetManager.class.getDeclaredConstructor(boolean.class);
            constructor.setAccessible(true);
            // setApkAssets reads the two statics, so they are set first; the manager is published after it has its assets.
            touched = true;
            set.set(null, new ArraySet<ApkAssets>(assets));
            array.set(null, all);
            AssetManager manager = constructor.newInstance(true);
            manager.setApkAssets(all, false);
            system.set(null, manager);
        } catch (Exception | LinkageError problem) {
            restoreSystemAssets(touched, array, previousArray, set, previousSet);
        } catch (Error problem) {
            restoreSystemAssets(touched, array, previousArray, set, previousSet);
            sSystemAssetsBuilt = false;
            throw problem;
        }
    }

    /** Puts the two statics the bootstrap changed back as they were found (nothing is changed before {@code touched}). */
    private static void restoreSystemAssets(boolean touched, Field array, Object previousArray, Field set,
            Object previousSet) {
        if (!touched) {
            return;
        }
        try {
            set.set(null, previousSet);
            array.set(null, previousArray);
        } catch (Exception | LinkageError ignored) {
            // Nothing more can be done: the framework builds the assets over again.
        }
    }

    /**
     * What a label is read for and an icon is drawn for: the highest density, day mode, the given locales, and no
     * other qualifier (screen size, orientation...), whatever the device's own state. A label then depends on the
     * system's locale list alone, which the digest covers, and one icon resource id is one picture for a package
     * version, so neither the digest nor the icon key needs a configuration in it.
     */
    private static Configuration fixedConfiguration(LocaleList locales) {
        Configuration configuration = new Configuration();
        configuration.densityDpi = ICON_DENSITY;
        configuration.uiMode = Configuration.UI_MODE_TYPE_NORMAL | Configuration.UI_MODE_NIGHT_NO;
        configuration.setLocales(locales);
        return configuration;
    }

    /** The system's current configuration (its locales above all); there is no default to stand in for it. */
    private Configuration configuration() throws PlatformException {
        if (mConfiguration == null) {
            Configuration configuration;
            try {
                configuration = ActivityManager.getService().getConfiguration();
            } catch (Exception problem) {
                throw new PlatformException("the system's configuration cannot be read", problem);
            }
            if (configuration == null) {
                throw new PlatformException("the system's configuration cannot be read");
            }
            mConfiguration = configuration;
        }
        return mConfiguration;
    }

    @Override
    public AppsCommand.StartResult start(int user, Component component) throws Exception {
        ChildProcess.Result result = ChildProcess.run(AppsCommand.startArguments(user, component),
                AppsCommand.START_SECONDS * 1000L, OUTPUT_LIMIT, new KillByPid());
        return new AppsCommand.StartResult(result.exit, result.output);
    }

    @Override
    public long nowMillis() {
        return SystemClock.elapsedRealtime();
    }

    /**
     * SIGKILL for a child that SIGTERM did not end. This libcore's {@code Process.destroyForcibly()} is
     * {@code destroy()} (SIGTERM) again and the class has no {@code pid()}, so the pid is read from the child's
     * own field and {@code android.os.Process.killProcess} sends the signal (the child is the shell user's own, and
     * it has not been reaped, so the pid is still its). A failure here is not reported: the wait that follows says
     * whether the child is gone.
     */
    private static final class KillByPid implements ChildProcess.Kill {
        @Override
        public void kill(Process process) throws Exception {
            java.lang.reflect.Field field = process.getClass().getDeclaredField("pid");
            field.setAccessible(true);
            android.os.Process.killProcess(field.getInt(process));
        }
    }

    /**
     * {@link SourceBound.Graph} over an application's resources: which resource is an image file (its size from
     * the file's header, a bounds-only decode), which an XML file (its uncompressed length, then the resources its
     * attributes refer to), which is no file (a colour, a number, a string that names no file) and which cannot be
     * read. A resource is judged by its value, as the framework judges what it is asked to load, and not by the name
     * of its type or of its file: {@link SourceBound#fileKind}.
     */
    private static final class ResourceGraph implements SourceBound.Graph {
        /** The resource's value cannot be read, so what it costs cannot be established. */
        private static final int UNKNOWN = -1;

        private final Resources mResources;

        ResourceGraph(Resources resources) {
            mResources = resources;
        }

        /**
         * The kind of file the resource's value is ({@link SourceBound#fileKind}; {@code value} is then its value:
         * the path), or {@link #UNKNOWN} when the value cannot be read.
         */
        private int classify(int id, TypedValue value) {
            try {
                mResources.getValueForDensity(id, ICON_DENSITY, value, true);
            } catch (Resources.NotFoundException unreadable) {
                return UNKNOWN;
            }
            if (value.type == TypedValue.TYPE_REFERENCE) {
                return UNKNOWN;
            }
            boolean string = value.type == TypedValue.TYPE_STRING && value.string != null;
            return SourceBound.fileKind(string, string ? value.string.toString() : null, string && opens(id));
        }

        /** Whether the resource's value names a file of the package that opens (the framework opens it the same way). */
        private boolean opens(int id) {
            try {
                mResources.openRawResource(id, new TypedValue()).close();
                return true;
            } catch (Resources.NotFoundException | IOException none) {
                return false;
            }
        }

        @Override
        public SourceBound.Node node(final int id) throws Exception {
            TypedValue value = new TypedValue();
            switch (classify(id, value)) {
                case SourceBound.NOT_A_FILE:
                    return SourceBound.Node.other();
                case SourceBound.XML_FILE:
                    // The length first: the framework reads the whole file into memory when it opens a parser.
                    long length;
                    try (InputStream in = mResources.openRawResource(id, new TypedValue())) {
                        length = in.available();
                    }
                    return SourceBound.Node.xml(length, () -> references(id));
                case SourceBound.IMAGE_FILE:
                    BitmapFactory.Options options = new BitmapFactory.Options();
                    options.inJustDecodeBounds = true;
                    try (InputStream in = mResources.openRawResource(id, new TypedValue())) {
                        BitmapFactory.decodeStream(in, null, options);
                    }
                    return SourceBound.Node.bitmap(options.outWidth, options.outHeight, value.density);
                default:
                    return SourceBound.Node.unknown();
            }
        }

        /** The files an XML resource's attributes refer to (@drawable, @mipmap, a colour state list...), each use of one. */
        private int[] references(int id) throws Exception {
            List<Integer> found = new ArrayList<>();
            TypedValue scratch = new TypedValue();
            XmlResourceParser parser = mResources.getXml(id);
            try {
                int elements = 0;
                for (int event = parser.next(); event != XmlPullParser.END_DOCUMENT; event = parser.next()) {
                    if (event != XmlPullParser.START_TAG) {
                        continue;
                    }
                    if (++elements > MAX_XML_ELEMENTS) {
                        throw new PlatformException("an icon's XML resource is too large");
                    }
                    for (int i = 0; i < parser.getAttributeCount(); i++) {
                        int reference = parser.getAttributeResourceValue(i, 0);
                        if (reference != 0 && classify(reference, scratch) != SourceBound.NOT_A_FILE) {
                            if (found.size() >= MAX_REFERENCES) {
                                throw new PlatformException("an icon's XML resource refers to too much");
                            }
                            found.add(reference);
                        }
                    }
                }
            } finally {
                parser.close();
            }
            int[] ids = new int[found.size()];
            for (int i = 0; i < ids.length; i++) {
                ids[i] = found.get(i);
            }
            return ids;
        }
    }
}
