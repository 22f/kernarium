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
import android.content.res.XmlResourceParser;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.drawable.AdaptiveIconDrawable;
import android.graphics.drawable.Drawable;
import android.os.LocaleList;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.util.TypedValue;

import org.xmlpull.v1.XmlPullParser;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
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
            result.add(new Entry(info.packageName, info.name, (application.flags & ApplicationInfo.FLAG_SYSTEM) != 0,
                    info.exported, true, permitted(info.permission), packageInfo.getLongVersionCode(),
                    packageInfo.lastUpdateTime, info.getIconResource(), ResourcePaths.fingerprint(paths), info));
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
     * itself): in the system's configuration, for the label; in a fixed configuration at the icon density, for the
     * icon. One per application and use. Null when the base, a split or a library cannot be added (the label is
     * then the package name and the icon an error); an overlay that cannot be added is skipped, as the framework
     * does.
     */
    private Resources resources(ApplicationInfo application, boolean icon) throws PlatformException {
        Map<String, Resources> cache = icon ? mIconResources : mLabelResources;
        if (!cache.containsKey(application.packageName)) {
            List<ResourcePaths.Item> paths = resourcePaths(application);
            Resources resources = null;
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
                complete = cookie != 0 || item.kind == ResourcePaths.Kind.OVERLAY;
            }
            if (complete) {
                DisplayMetrics metrics = new DisplayMetrics();
                metrics.setToDefaults();
                Configuration configuration;
                if (icon) {
                    metrics.densityDpi = ICON_DENSITY;
                    metrics.density = ICON_DENSITY / (float) DisplayMetrics.DENSITY_DEFAULT;
                    configuration = iconConfiguration();
                } else {
                    configuration = new Configuration(configuration());
                }
                resources = new Resources(assets, metrics, configuration);
            } else {
                assets.close();
            }
            cache.put(application.packageName, resources);
        }
        return cache.get(application.packageName);
    }

    /**
     * What an icon is drawn for: the highest density, day mode, a fixed locale, and no other qualifier
     * (screen size, orientation...), whatever the device's own state. One resource id is then one picture
     * for a package version, so the icon key needs no configuration in it.
     */
    private static Configuration iconConfiguration() {
        Configuration configuration = new Configuration();
        configuration.densityDpi = ICON_DENSITY;
        configuration.uiMode = Configuration.UI_MODE_TYPE_NORMAL | Configuration.UI_MODE_NIGHT_NO;
        configuration.setLocales(new LocaleList(ICON_LOCALE));
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
                AppsCommand.START_SECONDS * 1000L, OUTPUT_LIMIT);
        return new AppsCommand.StartResult(result.exit, result.output);
    }

    @Override
    public long nowMillis() {
        return SystemClock.elapsedRealtime();
    }

    /**
     * {@link SourceBound.Graph} over an application's resources: which resource is an image file (its size from
     * the file's header, a bounds-only decode), which an XML file (the resources its attributes refer to), and
     * which is neither.
     */
    private static final class ResourceGraph implements SourceBound.Graph {
        private final Resources mResources;

        ResourceGraph(Resources resources) {
            mResources = resources;
        }

        /** Whether the resource is a drawable or a mipmap, the kinds that are files worth looking into (not a colour or a string). */
        private boolean fileResource(int id) {
            try {
                String type = mResources.getResourceTypeName(id);
                return "drawable".equals(type) || "mipmap".equals(type);
            } catch (Resources.NotFoundException absent) {
                return false;
            }
        }

        @Override
        public SourceBound.Node node(int id) throws Exception {
            if (!fileResource(id)) {
                return SourceBound.Node.other();
            }
            TypedValue value = new TypedValue();
            try {
                mResources.getValueForDensity(id, ICON_DENSITY, value, true);
            } catch (Resources.NotFoundException absent) {
                return SourceBound.Node.other(); // loading the icon itself says so, for the one that matters
            }
            if (value.type != TypedValue.TYPE_STRING || value.string == null) {
                return SourceBound.Node.other();
            }
            String path = value.string.toString().toLowerCase(Locale.ROOT);
            if (path.endsWith(".xml")) {
                return SourceBound.Node.xml(references(id));
            }
            if (isImage(path)) {
                BitmapFactory.Options options = new BitmapFactory.Options();
                options.inJustDecodeBounds = true;
                try (InputStream in = mResources.openRawResource(id, new TypedValue())) {
                    BitmapFactory.decodeStream(in, null, options);
                }
                return SourceBound.Node.bitmap(options.outWidth, options.outHeight, value.density);
            }
            return SourceBound.Node.other();
        }

        private static boolean isImage(String path) {
            for (String extension : new String[] {".png", ".webp", ".jpg", ".jpeg", ".gif", ".bmp", ".heic", ".heif",
                    ".avif"}) {
                if (path.endsWith(extension)) {
                    return true;
                }
            }
            return false;
        }

        /** The resources an XML resource's attributes refer to (@drawable, @mipmap, @color...). */
        private int[] references(int id) throws Exception {
            List<Integer> found = new ArrayList<>();
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
                        if (reference != 0 && fileResource(reference)) {
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
