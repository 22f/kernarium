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
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * The bound on what drawing an icon inflates: before an application's icon resource is loaded, the
 * resources it reaches (an adaptive icon's layers, a layer list's items, a bitmap's file) are walked and what
 * the framework will read is worked out from the files' own headers (a bounds-only decode for a bitmap, the
 * uncompressed length for an XML file), so that a hostile or broken application cannot make the tool allocate
 * far more than the picture it is asked for. The framework decodes an image resource at full size, scaled to
 * the density it is loaded for, and only afterwards is the picture drawn into the small destination; this is
 * the check that comes before.
 *
 * <p>Every use of a file is charged: a bitmap that three layers refer to is decoded three times, so it counts
 * three times. The resources on the path being walked are remembered only to notice a cycle (the framework
 * would not end on one either), never to skip a use. A file whose cost cannot be established is over the
 * bound, not free.
 *
 * <p>The walk needs no Android: {@link Graph} answers for one resource, and the platform implements it.
 */
final class SourceBound {
    /** The most pixels one decoded bitmap may have: 64 MiB as ARGB_8888. */
    static final long MAX_BITMAP_PIXELS = 16L * 1024 * 1024;
    /** The most pixels all the bitmap decodes of one icon may have together: 128 MiB. */
    static final long MAX_TOTAL_PIXELS = 32L * 1024 * 1024;
    /** The most uncompressed bytes one XML resource may have: the framework reads the whole file into memory. */
    static final long MAX_XML_BYTES = 256L * 1024;
    /** The most uncompressed bytes all the XML reads of one icon may have together. */
    static final long MAX_XML_TOTAL_BYTES = 1024L * 1024;
    /** The most files one icon may use: every use counts, the same file twice is two. */
    static final int MAX_RESOURCES = 32;
    /** The deepest chain of resources one icon may reach. */
    static final int MAX_DEPTH = 6;
    /** A bitmap's density when its resource names none (the framework's default). */
    static final int DENSITY_DEFAULT = 160;
    /** Marks a resource that is never scaled (TypedValue.DENSITY_NONE). */
    static final int DENSITY_NONE = 0xFFFF;

    /** What a resource's value is, for {@link #fileKind}: no file (a colour, a number, a string that names none). */
    static final int NOT_A_FILE = 0;
    /** A file the framework inflates: its path ends in ".xml" (case-sensitively). */
    static final int XML_FILE = 1;
    /** Any other file: the framework decodes it as an image, whatever its type name or extension. */
    static final int IMAGE_FILE = 2;

    /**
     * The framework's own rule for a resource's value (ResourcesImpl.loadDrawableForCookie), judged by the value and
     * not by the resource's type: a string value that names a file that opens is a file, XML if its name ends in
     * ".xml" and an image if not. The resource's type name decides nothing: a table can give any type a file.
     */
    static int fileKind(boolean stringValue, String path, boolean opens) {
        if (!stringValue || path == null || !opens) {
            return NOT_A_FILE;
        }
        return path.endsWith(".xml") ? XML_FILE : IMAGE_FILE;
    }

    /** The resources an XML resource's attributes refer to; reads (opens a parser on) the file. */
    interface References {
        int[] read() throws Exception;
    }

    /** What one resource is, as far as this check needs. */
    static final class Node {
        static final int OTHER = 0;
        static final int BITMAP = 1;
        static final int XML = 2;
        static final int UNKNOWN = 3;

        final int kind;
        final long width;
        final long height;
        /** The density the bitmap's resource is declared for: 0 for none declared, {@link #DENSITY_NONE} for never scaled. */
        final int density;
        /** The uncompressed length of an XML file; 0 or less when it could not be established. */
        final long bytes;
        final References references;

        private Node(int kind, long width, long height, int density, long bytes, References references) {
            this.kind = kind;
            this.width = width;
            this.height = height;
            this.density = density;
            this.bytes = bytes;
            this.references = references;
        }

        /** A colour, a string, anything that is not a file: the walk does not look into it. */
        static Node other() {
            return new Node(OTHER, 0, 0, 0, 0, null);
        }

        /** An image file of the given pixel size and declared density. */
        static Node bitmap(long width, long height, int density) {
            return new Node(BITMAP, width, height, density, 0, null);
        }

        /** An XML file (an adaptive icon, a layer list, a vector...) of the given uncompressed length. */
        static Node xml(long bytes, References references) {
            return new Node(XML, 0, 0, 0, bytes, references);
        }

        /** A file of a cost that cannot be established: the icon is over the bound. */
        static Node unknown() {
            return new Node(UNKNOWN, 0, 0, 0, 0, null);
        }
    }

    /** The platform's answer for one resource id; throws when the resource cannot be read. */
    interface Graph {
        Node node(int id) throws Exception;
    }

    private SourceBound() {}

    /** One walk: what was measured (once for each resource) and what has been charged so far. */
    private static final class Walk {
        final Graph graph;
        final int targetDensity;
        final Set<Integer> path = new HashSet<>();
        final Map<Integer, Node> measured = new HashMap<>();
        final Map<Integer, int[]> referenced = new HashMap<>();
        int uses;
        long pixels;
        long xmlBytes;

        Walk(Graph graph, int targetDensity) {
            this.graph = graph;
            this.targetDensity = targetDensity;
        }

        Node node(int id) throws Exception {
            Node node = measured.get(id);
            if (node == null) {
                node = graph.node(id);
                measured.put(id, node);
            }
            return node;
        }

        int[] references(int id, Node node) throws Exception {
            int[] found = referenced.get(id);
            if (found == null) {
                found = node.references.read();
                referenced.put(id, found);
            }
            return found;
        }
    }

    /**
     * Whether inflating {@code root} for {@code targetDensity} stays within the bounds. Throws when a
     * resource cannot be read (the icon is then an error, not a guess).
     */
    static boolean within(Graph graph, int root, int targetDensity) throws Exception {
        return visit(new Walk(graph, targetDensity), root, 0);
    }

    private static boolean visit(Walk walk, int id, int depth) throws Exception {
        if (id == 0) {
            return true;
        }
        if (depth > MAX_DEPTH || !walk.path.add(id)) {
            return false; // too deep, or a resource that reaches itself
        }
        try {
            Node node = walk.node(id);
            if (node.kind == Node.OTHER) {
                return true; // a colour or a string costs nothing: only the files count towards the bound
            }
            if (node.kind == Node.UNKNOWN) {
                return false; // a file whose cost cannot be established is over the bound, not free
            }
            if (++walk.uses > MAX_RESOURCES) {
                return false;
            }
            if (node.kind == Node.BITMAP) {
                long pixels = decodedPixels(node, walk.targetDensity);
                if (pixels < 0 || pixels > MAX_BITMAP_PIXELS) {
                    return false;
                }
                walk.pixels += pixels;
                return walk.pixels <= MAX_TOTAL_PIXELS;
            }
            // An XML file: its length is checked before anything opens a parser on it.
            if (node.bytes <= 0 || node.bytes > MAX_XML_BYTES) {
                return false;
            }
            walk.xmlBytes += node.bytes;
            if (walk.xmlBytes > MAX_XML_TOTAL_BYTES) {
                return false;
            }
            for (int reference : walk.references(id, node)) {
                if (!visit(walk, reference, depth + 1)) {
                    return false;
                }
            }
            return true;
        } finally {
            walk.path.remove(id);
        }
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
