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

import java.io.IOException;
import java.util.HashSet;
import java.util.Set;

/**
 * The bound on what drawing an icon inflates: before an application's icon resource is loaded, the
 * resources it reaches (an adaptive icon's layers, a layer list's items, a bitmap's file) are walked and the
 * decoded size of every bitmap is worked out from the file's own header (a bounds-only decode), so that a
 * hostile or broken application cannot make the tool allocate far more than the picture it is asked for.
 * The framework decodes an image resource at full size, scaled to the density it is loaded for, and
 * only afterwards is the picture drawn into the small destination; this is the check that comes before.
 * The walk needs no Android: {@link Graph} answers for one resource, and the platform implements it.
 */
final class SourceBound {
    /** The most pixels one decoded bitmap may have: 64 MiB as ARGB_8888. */
    static final long MAX_BITMAP_PIXELS = 16L * 1024 * 1024;
    /** The most pixels all the bitmaps of one icon may have together: 128 MiB. */
    static final long MAX_TOTAL_PIXELS = 32L * 1024 * 1024;
    /** The most distinct resources one icon may reach. */
    static final int MAX_RESOURCES = 32;
    /** The deepest chain of resources one icon may reach. */
    static final int MAX_DEPTH = 6;
    /** A bitmap's density when its resource names none (the framework's default). */
    static final int DENSITY_DEFAULT = 160;
    /** Marks a resource that is never scaled (TypedValue.DENSITY_NONE). */
    static final int DENSITY_NONE = 0xFFFF;

    /** What one resource is, as far as this check needs. */
    static final class Node {
        static final int OTHER = 0;
        static final int BITMAP = 1;
        static final int XML = 2;

        final int kind;
        final long width;
        final long height;
        /** The density the bitmap's resource is declared for: 0 for none declared, {@link #DENSITY_NONE} for never scaled. */
        final int density;
        final int[] references;

        private Node(int kind, long width, long height, int density, int[] references) {
            this.kind = kind;
            this.width = width;
            this.height = height;
            this.density = density;
            this.references = references;
        }

        /** A colour, a string, anything the walk does not look into. */
        static Node other() {
            return new Node(OTHER, 0, 0, 0, new int[0]);
        }

        /** An image file of the given pixel size and declared density. */
        static Node bitmap(long width, long height, int density) {
            return new Node(BITMAP, width, height, density, new int[0]);
        }

        /** An XML file (an adaptive icon, a layer list, a vector...) and the resources its attributes refer to. */
        static Node xml(int... references) {
            return new Node(XML, 0, 0, 0, references);
        }
    }

    /** The platform's answer for one resource id; throws when the resource cannot be read. */
    interface Graph {
        Node node(int id) throws Exception;
    }

    private SourceBound() {}

    /**
     * Whether inflating {@code root} for {@code targetDensity} stays within the bounds. Throws when a
     * resource cannot be read (the icon is then an error, not a guess).
     */
    static boolean within(Graph graph, int root, int targetDensity) throws Exception {
        return visit(graph, root, targetDensity, 0, new HashSet<Integer>(), new long[1]);
    }

    private static boolean visit(Graph graph, int id, int targetDensity, int depth, Set<Integer> seen, long[] total)
            throws Exception {
        if (id == 0 || !seen.add(id)) {
            return true;
        }
        if (seen.size() > MAX_RESOURCES || depth > MAX_DEPTH) {
            return false;
        }
        Node node = graph.node(id);
        if (node.kind == Node.OTHER) {
            seen.remove(id); // a colour or a string costs nothing: only the files count towards the bound
            return true;
        }
        if (node.kind == Node.BITMAP) {
            long pixels = decodedPixels(node, targetDensity);
            if (pixels < 0 || pixels > MAX_BITMAP_PIXELS) {
                return false;
            }
            total[0] += pixels;
            return total[0] <= MAX_TOTAL_PIXELS;
        }
        if (node.kind == Node.XML) {
            for (int reference : node.references) {
                if (!visit(graph, reference, targetDensity, depth + 1, seen, total)) {
                    return false;
                }
            }
        }
        return true;
    }

    /** The pixels of the bitmap once decoded for the target density, or -1 when over what one bitmap may have. */
    static long decodedPixels(Node node, int targetDensity) throws IOException {
        if (node.width <= 0 || node.height <= 0) {
            throw new IOException("a bitmap's size is not known");
        }
        long width = node.width;
        long height = node.height;
        if (node.density != DENSITY_NONE) {
            long declared = node.density <= 0 ? DENSITY_DEFAULT : node.density;
            width = (width * targetDensity + declared - 1) / declared;
            height = (height * targetDensity + declared - 1) / declared;
        }
        if (width > MAX_BITMAP_PIXELS || height > MAX_BITMAP_PIXELS) {
            return -1;
        }
        return width * height;
    }
}
