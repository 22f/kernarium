// SPDX-License-Identifier: Apache-2.0
//
// What a camera's characteristics say, worked out from the camera's static
// description and the formats its capture node offers. The HAL's discovery
// turns the numbers into Android's static metadata, and the tests hold the rules
// (the capabilities, the stream configurations and their durations, the frame
// rate ranges, the optics) where no Android library is needed.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
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
};

inline Size largest(const std::vector<Size>& sizes) noexcept
{
    Size best;
    for (const auto& size : sizes)
        if (size.pixels() > best.pixels()) best = size;
    return best;
}

// A single lens has manual exposure and, once calibrated, RAW; the zoom camera
// is automatic only. BURST_CAPTURE is never declared: CTS wants the largest YUV
// size to be at least the largest JPEG size and within 3% of the pixel array
// (the host's video modes stop at 4K or 1080p, below its stills), the slow YUV
// sizes in the stall durations, and a sync latency of at most four frames (the
// host applies controls as soon as it can: UNKNOWN).
inline Capabilities capabilities_of(const abi::Desc& desc, const Formats& formats) noexcept
{
    Capabilities caps;
    caps.manual_sensor = (desc.flags & abi::feature::manual_exposure) != 0 && desc.lens != abi::lens::zoom;
    caps.read_sensor_settings = caps.manual_sensor;
    caps.raw = caps.manual_sensor && (desc.flags & abi::feature::raw) != 0 && formats.raw.pixels() != 0 &&
        desc.bayer_order != abi::bayer::none;
    return caps;
}

// android.control.maxRegions: the AE, AWB and AF regions the camera takes. The
// host meters and focuses on one point, and a camera without autofocus has none
// to move.
inline std::array<std::int32_t, 3> max_regions_of(const abi::Desc& desc) noexcept
{
    return {1, 0, (desc.flags & abi::feature::autofocus) != 0 ? 1 : 0};
}

// Tags of camera_metadata_tags.h that unlisted_keys names; the HAL asserts them.
namespace tag {
constexpr std::int32_t control_ae_regions = 0x10004;
constexpr std::int32_t control_af_regions = 0x10008;
constexpr std::int32_t control_awb_regions = 0x1000c;
constexpr std::int32_t sensor_frame_duration = 0xe0001;
constexpr std::int32_t black_level_lock = 0x160000;
} // namespace tag

// The keys of the template's request and result key lists that this camera does
// not honour; it lists them in neither. The frame duration and the black level
// lock are the sensor keys of MANUAL_SENSOR, so a camera without the capability
// has neither: CTS fails a camera that has every key of a capability it does
// not list. A region key is listed only for a kind of region the camera takes.
inline std::vector<std::int32_t> unlisted_keys(const abi::Desc& desc, const Capabilities& caps)
{
    std::vector<std::int32_t> keys;
    if (!caps.manual_sensor) {
        keys.push_back(tag::sensor_frame_duration);
        keys.push_back(tag::black_level_lock);
    }
    const auto regions = max_regions_of(desc);
    if (regions[0] == 0) keys.push_back(tag::control_ae_regions);
    if (regions[1] == 0) keys.push_back(tag::control_awb_regions);
    if (regions[2] == 0) keys.push_back(tag::control_af_regions);
    return keys;
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

// A window of a frame, in its pixels.
struct Window {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    friend bool operator==(const Window&, const Window&) = default;
};

// How a stream of one size is made. The host serves the sizes it lists itself (a
// video mode of the node, a still size). Any other size is cut from a native NV12
// video mode: the HAL scales `window` of that mode's frame to the stream's size
// and, for a JPEG, encodes the result itself.
struct SizeSource {
    Size size;
    bool host = true;
    Size source; // the native mode the frame comes from (the size itself when the host serves it)
    Window window; // the part of its frame (all of it when the host serves the size)
    friend bool operator==(const SizeSource&, const SizeSource&) = default;
};

// The sizes of the three CTS-visible lists. PRIVATE (IMPLEMENTATION_DEFINED) has
// the sizes of YUV_420_888.
struct StreamSizes {
    std::vector<SizeSource> yuv;
    std::vector<SizeSource> jpeg;
};

// The JPEG sizes the camera API requires of every camera, and the pair of sizes it
// guarantees a camera that streams beside another ("1440p" is 1920 x 1440, the
// 4:3 size, and "720p" is 1280 x 720).
inline constexpr Size size_1080p{1920, 1080};
inline constexpr Size size_720p{1280, 720};
inline constexpr Size size_480p{640, 480};
inline constexpr Size size_240p{320, 240};
inline constexpr Size size_1440p{1920, 1440};
inline constexpr Size standard_jpeg_sizes[] = {size_1080p, size_720p, size_480p, size_240p};

// The distinct sizes of the video modes, in the order the node lists them.
inline std::vector<Size> video_sizes(const Formats& formats)
{
    std::vector<Size> sizes;
    for (const auto& mode : formats.video) {
        const Size size{mode.width, mode.height};
        if (std::find(sizes.begin(), sizes.end(), size) == sizes.end()) sizes.push_back(size);
    }
    return sizes;
}

// The highest frame rate any mode of that size runs at; zero when there is none.
inline std::uint32_t fastest_rate(const Formats& formats, const Size& size) noexcept
{
    std::uint32_t fastest = 0;
    for (const auto& mode : formats.video)
        if (mode.width == size.width && mode.height == size.height) fastest = std::max(fastest, mode.max_fps);
    return fastest;
}

// The largest window of a frame with the aspect ratio of `shape`, centred, and the
// frame itself when it has that aspect already. A cropped window starts and ends on
// even pixels (the chroma of NV12 and I420 is half the size). A picture is cropped
// like this before it is scaled to a shape, never stretched to it.
inline Window centred_window(const Size& frame, const Size& shape) noexcept
{
    std::uint32_t width = frame.width;
    std::uint32_t height = frame.height;
    if (shape.width == 0 || shape.height == 0) return {0, 0, width, height};
    const std::uint64_t wide = std::uint64_t(frame.width) * shape.height;
    const std::uint64_t tall = std::uint64_t(frame.height) * shape.width;
    if (wide != tall) {
        if (wide > tall)
            width = std::uint32_t(std::uint64_t(frame.height) * shape.width / shape.height);
        else
            height = std::uint32_t(std::uint64_t(frame.width) * shape.height / shape.width);
        width &= ~1u;
        height &= ~1u;
    }
    return {((frame.width - width) / 2) & ~1u, ((frame.height - height) / 2) & ~1u, width, height};
}

// How `target` is cut from the native video modes `natives`: the smallest mode
// that covers it in both dimensions, one with the target's aspect ratio first;
// otherwise the largest centred window of that aspect (centred_window). Even sizes
// only (NV12's chroma is half the size). Nothing when no mode covers it.
inline std::optional<SizeSource> cut_from(const std::vector<Size>& natives, const Size& target)
{
    if (target.width == 0 || target.height == 0 || ((target.width | target.height) & 1u) != 0) return std::nullopt;
    const Size* best = nullptr;
    bool best_same = false;
    for (const auto& native : natives) {
        if (native.width < target.width || native.height < target.height) continue;
        const bool same = std::uint64_t(native.width) * target.height == std::uint64_t(native.height) * target.width;
        bool better = best == nullptr || (same && !best_same);
        if (!better && same == best_same)
            better = native.pixels() < best->pixels() || (native.pixels() == best->pixels() && native.width < best->width);
        if (better) {
            best = &native;
            best_same = same;
        }
    }
    if (best == nullptr) return std::nullopt;
    SizeSource source;
    source.size = target;
    source.host = false;
    source.source = *best;
    source.window = centred_window(*best, target);
    return source;
}

// The sizes of the output lists. The host's own sizes stay as they are. To them
// the HAL adds what CTS and the camera API require and the host does not list,
// each cut from a native video mode (cut_from):
//  - JPEG: 1920x1080, 1280x720, 640x480 and 320x240, and 1920x1440 with 1280x720
//    when the largest JPEG reaches 1920x1440 (a camera that streams beside another
//    is guaranteed those);
//  - YUV and PRIVATE: every JPEG size that is not larger than the largest video
//    size (a LIMITED camera lists them), and the same guaranteed pair of the largest
//    video size.
// A size no native mode covers is left out; the lists say what the camera can serve.
inline StreamSizes derive_sizes(const Formats& formats)
{
    StreamSizes result;
    const std::vector<Size> natives = video_sizes(formats);
    const auto contains = [](const std::vector<SizeSource>& list, const Size& size) {
        return std::any_of(list.begin(), list.end(), [&](const SizeSource& entry) { return entry.size == size; });
    };
    const auto host_entry = [](const Size& size) {
        SizeSource entry;
        entry.size = size;
        entry.source = size;
        entry.window = {0, 0, size.width, size.height};
        return entry;
    };
    const auto add_cut = [&](std::vector<SizeSource>& list, const Size& size) {
        if (contains(list, size)) return;
        if (const auto cut = cut_from(natives, size)) list.push_back(*cut);
    };
    const auto most = [](const std::vector<SizeSource>& list) {
        std::uint64_t pixels = 0;
        for (const auto& entry : list) pixels = std::max(pixels, entry.size.pixels());
        return pixels;
    };

    for (const auto& size : formats.jpeg)
        if (!contains(result.jpeg, size)) result.jpeg.push_back(host_entry(size));
    for (const auto& size : standard_jpeg_sizes) add_cut(result.jpeg, size);
    if (most(result.jpeg) >= size_1440p.pixels()) {
        add_cut(result.jpeg, size_1440p);
        add_cut(result.jpeg, size_720p);
    }

    for (const auto& size : natives) result.yuv.push_back(host_entry(size));
    const std::uint64_t largest_video = most(result.yuv);
    for (const auto& entry : result.jpeg)
        if (entry.size.pixels() <= largest_video) add_cut(result.yuv, entry.size);
    if (largest_video >= size_1440p.pixels()) {
        add_cut(result.yuv, size_1440p);
        add_cut(result.yuv, size_720p);
    } else if (largest_video >= size_720p.pixels()) {
        add_cut(result.yuv, size_720p);
    }

    const auto by_size = [](const SizeSource& a, const SizeSource& b) {
        if (a.size.pixels() != b.size.pixels()) return a.size.pixels() > b.size.pixels();
        if (a.size.width != b.size.width) return a.size.width > b.size.width;
        return a.size.height > b.size.height;
    };
    std::sort(result.yuv.begin(), result.yuv.end(), by_size);
    std::sort(result.jpeg.begin(), result.jpeg.end(), by_size);
    return result;
}

// The camera API guarantees a camera that streams beside another the sizes its
// own largest size implies, for YUV, PRIVATE and JPEG: both 1920x1440 and
// 1280x720 when the list reaches 1920x1440, 1280x720 when it reaches that.
inline bool meets_guaranteed_sizes(const std::vector<SizeSource>& list) noexcept
{
    if (list.empty()) return false;
    std::uint64_t largest_pixels = 0;
    bool has_1440p = false, has_720p = false;
    for (const auto& entry : list) {
        largest_pixels = std::max(largest_pixels, entry.size.pixels());
        has_1440p = has_1440p || entry.size == size_1440p;
        has_720p = has_720p || entry.size == size_720p;
    }
    if (largest_pixels >= size_1440p.pixels()) return has_1440p && has_720p;
    if (largest_pixels >= size_720p.pixels()) return has_720p;
    return true;
}

// Whether a camera meets the guaranteed concurrent stream sizes (the HAL's
// provider checks the same rule on the static metadata it publishes).
inline bool meets_concurrent_streams(const StreamSizes& sizes) noexcept
{
    return meets_guaranteed_sizes(sizes.yuv) && meets_guaranteed_sizes(sizes.jpeg);
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

// The stream configurations: YUV_420_888 and IMPLEMENTATION_DEFINED at every YUV
// size, BLOB at every JPEG size (derive_sizes) and RAW16 at the Bayer size. A size
// the HAL cuts from a native mode runs at that mode's rate.
inline StreamTables stream_tables(const Formats& formats, const Capabilities& caps)
{
    StreamTables tables;
    const StreamSizes sizes = derive_sizes(formats);
    auto add = [&](std::int32_t format, const Size& size, std::int64_t duration, std::int64_t stall) {
        tables.configurations.insert(tables.configurations.end(), {format, std::int32_t(size.width), std::int32_t(size.height), 0});
        tables.min_frame_durations.insert(tables.min_frame_durations.end(), {format, std::int64_t(size.width), std::int64_t(size.height), duration});
        tables.stall_durations.insert(tables.stall_durations.end(), {format, std::int64_t(size.width), std::int64_t(size.height), stall});
    };
    for (const auto& entry : sizes.yuv) {
        const auto duration = frame_duration_of(fastest_rate(formats, entry.source));
        add(pixel_format::yuv_420_888, entry.size, duration, 0);
        add(pixel_format::implementation_defined, entry.size, duration, 0);
    }
    // The still path keeps the rate of the slowest preview that runs at 30 frames per second.
    const std::int64_t still_duration = 33'333'333;
    for (const auto& entry : sizes.jpeg) add(pixel_format::blob, entry.size, still_duration, jpeg_stall_ns(entry.size));
    if (caps.raw) add(pixel_format::raw16, formats.raw, still_duration, raw_stall_ns(formats.raw));
    return tables;
}

// The auto-exposure target frame rate ranges (low, high pairs, ascending). r is
// the rate of the largest YUV size, from its minimum frame duration: CTS wants
// (r, r) and (15, r) for it. A fixed range stands for every rate the host runs
// a mode at, whatever the rate, and the usual variable ones for rates they fit.
inline std::vector<std::int32_t> fps_ranges(const Formats& formats)
{
    Size largest_video;
    for (const auto& size : video_sizes(formats))
        if (size.pixels() > largest_video.pixels()) largest_video = size;
    const double duration = double(frame_duration_of(fastest_rate(formats, largest_video)));
    const std::int32_t r = std::int32_t(std::floor(1e9 / duration + 0.05));

    std::set<std::pair<std::int32_t, std::int32_t>> ranges;
    for (const auto& mode : formats.video)
        if (mode.max_fps > 0) ranges.insert({std::int32_t(mode.max_fps), std::int32_t(mode.max_fps)});
    ranges.insert({r, r});
    if (r > 15) ranges.insert({15, r});
    if (r >= 15) ranges.insert({15, 15});
    if (r >= 30) {
        ranges.insert({15, 30});
        ranges.insert({30, 30});
    }
    if (r >= 60) ranges.insert({30, 60});
    std::vector<std::int32_t> flat;
    for (const auto& range : ranges) flat.insert(flat.end(), {range.first, range.second});
    return flat;
}

// The thumbnail sizes (width, height pairs, smallest first, (0, 0) first of all).
inline std::vector<std::int32_t> thumbnail_sizes()
{
    return {0, 0, 160, 90, 160, 120, 320, 180, 320, 240};
}

// The largest JPEG a stream of the largest still size needs, with the segment
// the host reserves for Exif and the blob header (a smaller stream's buffer is
// proportionally smaller, and always has room for its own reservation).
inline std::int32_t jpeg_max_size(const Formats& formats)
{
    std::uint64_t most = 0;
    for (const auto& entry : derive_sizes(formats).jpeg) most = std::max(most, entry.size.pixels());
    const std::uint64_t bytes = most * 3 / 2 + abi::exif_gap_length + 8;
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
// the HAL's default is, at either end. ISO below 100 is allowed (iOS has 25 or 32).
inline std::int32_t sensitivity_min(const abi::Desc& desc) noexcept
{
    return std::int32_t(std::clamp<std::uint32_t>(desc.iso_min, 1, 100));
}
inline std::int32_t sensitivity_max(const abi::Desc& desc) noexcept
{
    return std::int32_t(std::max<std::uint32_t>(desc.iso_max, 100));
}

} // namespace andarium::camera
