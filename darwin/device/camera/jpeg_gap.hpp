// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace uml::camera {

/* The payload length of the Exif segment the device reserves in a JPEG still.
 * The segment is an APP1 whose payload (the bytes after the two length
 * bytes, "Exif\0\0" first and zeros after it) the guest rewrites in place;
 * its length field stays as written. */
inline constexpr std::uint32_t exifGapLength = 65000;

/* A JPEG as the device writes it into a guest buffer: `head` (the start of
 * image, the reserved Exif segment and every other segment before the scan)
 * and `tail` (the scan and the end of image, exactly as the platform made it),
 * so the entropy-coded data is copied once. */
struct JpegWithGap {
    std::vector<std::uint8_t> head;
    std::span<const std::uint8_t> tail;
    std::uint32_t gapOffset {};  // of the payload, from the start of the file
    std::uint32_t gapLength {};

    [[nodiscard]] std::size_t size() const noexcept { return head.size() + tail.size(); }
};

/* Reserves the Exif gap in `jpeg` and drops its own Exif and XMP (APP1)
 * segments; the other segments (JFIF, quantization and Huffman tables, the
 * frame header) keep their order. Returns nothing for data that is not a
 * JPEG with a scan. The result's `tail` refers to `jpeg`. */
[[nodiscard]] std::optional<JpegWithGap> reserveExifGap(std::span<const std::uint8_t> jpeg,
                                                        std::uint32_t gapLength = exifGapLength);

} // namespace uml::camera
