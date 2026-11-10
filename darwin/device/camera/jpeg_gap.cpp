// SPDX-License-Identifier: GPL-2.0-or-later
#include "jpeg_gap.hpp"

#include <cstring>

namespace uml::camera {

std::optional<JpegWithGap> reserveExifGap(std::span<const std::uint8_t> jpeg, std::uint32_t gapLength)
{
    if (gapLength < 6 || gapLength > 65533 || jpeg.size() < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8) return std::nullopt;
    JpegWithGap result;
    result.head = {0xff, 0xd8};
    result.head.push_back(0xff);
    result.head.push_back(0xe1);
    const std::uint32_t segmentLength = gapLength + 2;
    result.head.push_back(std::uint8_t(segmentLength >> 8));
    result.head.push_back(std::uint8_t(segmentLength));
    result.gapOffset = std::uint32_t(result.head.size());
    result.gapLength = gapLength;
    result.head.resize(result.head.size() + gapLength, 0);
    std::memcpy(result.head.data() + result.gapOffset, "Exif\0\0", 6);

    std::size_t at = 2;
    for (;;) {
        // A marker is 0xff, any number of 0xff fill bytes, then its code.
        if (at + 2 > jpeg.size() || jpeg[at] != 0xff) return std::nullopt;
        std::size_t code = at + 1;
        while (code < jpeg.size() && jpeg[code] == 0xff) ++code;
        if (code >= jpeg.size()) return std::nullopt;
        const std::uint8_t marker = jpeg[code];
        if (marker == 0xda) {
            result.tail = jpeg.subspan(code - 1);
            return result;
        }
        // Markers that stand alone have no place before a scan.
        if (marker == 0x00 || marker == 0x01 || marker == 0xd8 || marker == 0xd9 || (marker >= 0xd0 && marker <= 0xd7))
            return std::nullopt;
        if (code + 3 > jpeg.size()) return std::nullopt;
        const std::size_t length = std::size_t(jpeg[code + 1]) << 8 | jpeg[code + 2];
        if (length < 2 || code + 1 + length > jpeg.size()) return std::nullopt;
        if (marker != 0xe1)
            result.head.insert(result.head.end(), jpeg.begin() + std::ptrdiff_t(code - 1),
                               jpeg.begin() + std::ptrdiff_t(code + 1 + length));
        at = code + 1 + length;
    }
}

} // namespace uml::camera
