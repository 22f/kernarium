// SPDX-License-Identifier: Apache-2.0
//
// What a camera's characteristics say, worked out from the camera's static
// description and the formats its capture node offers. The HAL's discovery
// turns the numbers into Android's static metadata, and the tests hold the rules
// (the capabilities, the stream configurations and their durations, the frame
// rate ranges, the optics) where no Android library is needed.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "camera_abi.hpp"

namespace andarium::camera {

// Android's pixel formats (HAL_PIXEL_FORMAT_*) in the stream configurations.
namespace pixel_format {
constexpr std::int32_t raw16 = 0x20;
constexpr std::int32_t blob = 0x21;
constexpr std::int32_t implementation_defined = 0x22;
constexpr std::int32_t yuv_420_888 = 0x23;
} // namespace pixel_format

struct Size {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    [[nodiscard]] std::uint64_t pixels() const noexcept { return std::uint64_t(width) * height; }
    friend bool operator==(const Size&, const Size&) = default;
};

// A size of the video kind with the frame-rate range the host runs it at.
struct VideoMode {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t min_fps = 0;
    std::uint32_t max_fps = 0;
};

// What the capture node's ENUM_FMT, ENUM_FRAMESIZES and ENUM_FRAMEINTERVALS say.
struct Formats {
    std::vector<VideoMode> video;
    std::vector<Size> jpeg;
    Size raw; // zero when the camera takes no Bayer stills
};

inline float from_q16(std::uint32_t value) noexcept { return float(double(std::int32_t(value)) / 65536.0); }
inline std::uint32_t to_q16(double value) noexcept
{
    return std::uint32_t(std::int32_t(std::lround(value * 65536.0)));
}

// The capabilities the camera's description earns.
struct Capabilities {
    bool manual_sensor = false;
    bool read_sensor_settings = false;
    bool raw = false;
    bool burst_capture = false;
};

inline Size largest(const std::vector<Size>& sizes) noexcept
{
    Size best;
    for (const auto& size : sizes)
        if (size.pixels() > best.pixels()) best = size;
    return best;
}

// A single lens has manual exposure and, once calibrated, RAW; the zoom camera
// is automatic only. BURST_CAPTURE needs a YUV size of 8 megapixels or more at 20
// frames per second or more.
inline Capabilities capabilities_of(const abi::Desc& desc, const Formats& formats) noexcept
{
    Capabilities caps;
    caps.manual_sensor = (desc.flags & abi::feature::manual_exposure) != 0 && desc.lens != abi::lens::zoom;
    caps.read_sensor_settings = caps.manual_sensor;
    caps.raw = caps.manual_sensor && (desc.flags & abi::feature::raw) != 0 && formats.raw.pixels() != 0 &&
        desc.bayer_order != abi::bayer::none;
    for (const auto& mode : formats.video)
        if (std::uint64_t(mode.width) * mode.height >= 8'000'000 && mode.max_fps >= 20) caps.burst_capture = true;
    return caps;
}

// The size of the array that RAW and the field of view are stated in: the Bayer
// still's own size when the camera has one, else the description's pixel array.
inline Size array_size(const abi::Desc& desc, const Capabilities& caps) noexcept
{
    if (caps.raw) return {desc.raw_width, desc.raw_height};
    return {desc.pixel_array_width, desc.pixel_array_height};
}

// The pipeline depth: the buffers the host's memory tier gives a stream. The
// small tier stops at 1080p video, so a larger video size is the large tier.
inline std::uint32_t pipeline_depth(const Formats& formats) noexcept
{
    for (const auto& mode : formats.video)
        if (mode.height > 1080 || mode.width > 1920) return 6;
    return 4;
}

// The longest frame duration Android may be told: one second, or the exposure
// limit when that is longer.
inline std::int64_t max_frame_duration_ns(const abi::Desc& desc) noexcept
{
    return std::int64_t(std::max<std::uint64_t>(desc.exposure_max_ns, 1'000'000'000ull));
}

// STALL of a still: the capture and the host's encoder, proportional to the
// number of pixels.
inline std::int64_t jpeg_stall_ns(const Size& size) noexcept
{
    return 100'000'000 + std::int64_t(size.pixels()) * 20;
}
inline std::int64_t raw_stall_ns(const Size& size) noexcept
{
    return 150'000'000 + std::int64_t(size.pixels()) * 10;
}

struct StreamTables {
    // (format, width, height, direction) for each output; direction 0 is OUTPUT.
    std::vector<std::int32_t> configurations;
    // (format, width, height, duration in ns)
    std::vector<std::int64_t> min_frame_durations;
    std::vector<std::int64_t> stall_durations;
};

inline std::int64_t frame_duration_of(std::uint32_t fps) noexcept
{
    return fps == 0 ? 33'333'333 : std::int64_t(1'000'000'000ull / fps);
}

// The stream configurations: YUV_420_888 and IMPLEMENTATION_DEFINED at every
// video size, BLOB at every JPEG size and RAW16 at the Bayer size.
inline StreamTables stream_tables(const Formats& formats, const Capabilities& caps)
{
    StreamTables tables;
    std::vector<Size> sizes;
    std::vector<std::uint32_t> fastest;
    for (const auto& mode : formats.video) {
        auto found = std::find(sizes.begin(), sizes.end(), Size{mode.width, mode.height});
        if (found == sizes.end()) {
            sizes.push_back({mode.width, mode.height});
            fastest.push_back(mode.max_fps);
        } else {
            auto& fps = fastest[std::size_t(found - sizes.begin())];
            fps = std::max(fps, mode.max_fps);
        }
    }
    auto add = [&](std::int32_t format, const Size& size, std::int64_t duration, std::int64_t stall) {
        tables.configurations.insert(tables.configurations.end(), {format, std::int32_t(size.width), std::int32_t(size.height), 0});
        tables.min_frame_durations.insert(tables.min_frame_durations.end(), {format, std::int64_t(size.width), std::int64_t(size.height), duration});
        tables.stall_durations.insert(tables.stall_durations.end(), {format, std::int64_t(size.width), std::int64_t(size.height), stall});
    };
    for (std::size_t index = 0; index < sizes.size(); ++index) {
        const auto duration = frame_duration_of(fastest[index]);
        add(pixel_format::yuv_420_888, sizes[index], duration, 0);
        add(pixel_format::implementation_defined, sizes[index], duration, 0);
    }
    // The still path keeps the rate of the slowest preview that runs at 30 frames per second.
    const std::int64_t still_duration = 33'333'333;
    for (const auto& size : formats.jpeg) add(pixel_format::blob, size, still_duration, jpeg_stall_ns(size));
    if (caps.raw) add(pixel_format::raw16, formats.raw, still_duration, raw_stall_ns(formats.raw));
    return tables;
}

// The auto-exposure target frame rate ranges: a fixed range at each rate the
// host runs at, and the usual variable ones.
inline std::vector<std::int32_t> fps_ranges(const Formats& formats)
{
    std::uint32_t highest = 0;
    for (const auto& mode : formats.video) highest = std::max(highest, mode.max_fps);
    std::vector<std::int32_t> ranges = {15, 15, 15, 30, 30, 30};
    if (highest >= 60) ranges.insert(ranges.end(), {30, 60, 60, 60});
    if (highest >= 120) ranges.insert(ranges.end(), {120, 120});
    if (highest >= 240) ranges.insert(ranges.end(), {240, 240});
    return ranges;
}

// The thumbnail sizes (width, height pairs, smallest first, (0, 0) first of all).
inline std::vector<std::int32_t> thumbnail_sizes()
{
    return {0, 0, 160, 90, 160, 120, 320, 180, 320, 240};
}

// The largest JPEG a stream of the largest still size needs, with the segment
// the host reserves for Exif and the blob header (a smaller stream's buffer is
// proportionally smaller, and always has room for its own reservation).
inline std::int32_t jpeg_max_size(const Formats& formats) noexcept
{
    const auto best = largest(formats.jpeg);
    const std::uint64_t bytes = best.pixels() * 3 / 2 + abi::exif_gap_length + 8;
    return std::int32_t(std::min<std::uint64_t>(bytes, 0x7fffffffu));
}

// The focal length in millimetres and the focus distances in dioptres.
inline float focal_length_mm(const abi::Desc& desc) noexcept { return float(desc.focal_length_um) / 1000.0f; }
inline float aperture(const abi::Desc& desc) noexcept { return float(desc.aperture_x100) / 100.0f; }
inline float minimum_focus_dioptres(const abi::Desc& desc) noexcept
{
    return desc.min_focus_distance_mm == 0 ? 0.0f : 1000.0f / float(desc.min_focus_distance_mm);
}

// The Android sensitivity range: the description's, widened to include the 100
// the sensor's default is. ISO below 100 is allowed (iOS has 25 or 32).
inline std::int32_t sensitivity_min(const abi::Desc& desc) noexcept { return std::int32_t(std::max<std::uint32_t>(desc.iso_min, 1)); }
inline std::int32_t sensitivity_max(const abi::Desc& desc) noexcept
{
    return std::int32_t(std::max<std::uint32_t>(desc.iso_max, 100));
}

} // namespace andarium::camera
