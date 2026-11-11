// SPDX-License-Identifier: Apache-2.0
//
// The two edits the HAL makes to a JPEG that the host encoded. The host leaves
// an Exif segment (an APP1 whose length field already says its payload is
// abi::exif_gap_length bytes) in the picture, and the HAL writes the Exif data
// and the thumbnail into it in place, so the picture's entropy-coded data is not
// moved again. Android's JPEG buffer then ends with a camera3_jpeg_blob_t that
// states the size of the picture.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "camera_abi.hpp"

namespace andarium::camera {

// camera3_jpeg_blob_t: the last bytes of a BLOB buffer that holds a JPEG.
struct JpegBlobHeader {
    std::uint16_t blob_id;
    std::uint32_t jpeg_size;
};
static_assert(sizeof(JpegBlobHeader) == 8);
constexpr std::uint16_t jpeg_blob_id = 0x00FF;

// Writes the Exif data (what libexif saves: "Exif\0\0" and the TIFF structure)
// at the start of the gap and zeroes the rest of it. Returns false and changes
// nothing when the gap does not lie inside the buffer, is not the length the
// device reserves or is too small for the data.
inline bool place_exif(std::span<std::uint8_t> jpeg, std::uint32_t gap_offset, std::uint32_t gap_length,
                       std::span<const std::uint8_t> exif)
{
    if (gap_length != abi::exif_gap_length || gap_offset < 4) return false;
    if (gap_offset > jpeg.size() || gap_length > jpeg.size() - gap_offset) return false;
    if (exif.size() > gap_length) return false;
    // The two bytes before the payload are the segment's length, and the two
    // before those the APP1 marker; a gap anywhere else is not the device's.
    if (jpeg[gap_offset - 4] != 0xFF || jpeg[gap_offset - 3] != 0xE1) return false;
    if (!exif.empty()) std::memcpy(jpeg.data() + gap_offset, exif.data(), exif.size());
    std::memset(jpeg.data() + gap_offset + exif.size(), 0, gap_length - exif.size());
    return true;
}

// Writes the blob header at the end of a BLOB buffer. The picture must leave
// room for it.
inline bool write_blob_header(std::span<std::uint8_t> buffer, std::uint32_t jpeg_size)
{
    if (buffer.size() < sizeof(JpegBlobHeader) || jpeg_size > buffer.size() - sizeof(JpegBlobHeader)) return false;
    JpegBlobHeader header{};
    header.blob_id = jpeg_blob_id;
    header.jpeg_size = jpeg_size;
    std::memcpy(buffer.data() + buffer.size() - sizeof(header), &header, sizeof(header));
    return true;
}

} // namespace andarium::camera
