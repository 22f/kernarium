// SPDX-License-Identifier: GPL-2.0-or-later
#include "camera_description.hpp"

#include "photo_dimensions.hpp"
#include "vhost_user_device.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace uml::camera {
namespace {
using uml::platform::BayerOrder;
using uml::platform::CameraInfo;
using uml::platform::CameraLensKind;
using uml::platform::CameraPosition;
using uml::virtio::toBytes;

/* The pixel pitch the sensor size is derived from, as the sensor is not stated:
 * the ratio of the lenses stays exact, and the absolute figures are nominal
 * until a Bayer still replaces them. */
constexpr double nominalPitchMicrometres = 1.2;

/* The frame rate up to which a camera's streams are always allowed the photos
 * it offers: a format that cannot make them at a higher rate does not
 * restrict the photos, and a host with such a format offers the rates the
 * photos hold up to (offerStills). */
constexpr std::uint32_t stillsHoldUntilRate = 30;
constexpr std::uint32_t maximumCameras = VIRTIO_VIDEO_CAMERA_MAX_CAMERAS;

/* The sizes every camera offers beyond its native ones, so that a guest has
 * the usual preview and recording sizes; the device scales to them. */
constexpr std::pair<std::uint32_t, std::uint32_t> standardSizes[] = {
    {176, 144}, {320, 240}, {640, 480}, {1280, 720}, {1920, 1080}, {3840, 2160},
};

std::uint32_t lensOf(CameraLensKind kind) noexcept
{
    switch (kind) {
    case CameraLensKind::zoom: return VIRTIO_VIDEO_CAMERA_LENS_ZOOM;
    case CameraLensKind::wide: return VIRTIO_VIDEO_CAMERA_LENS_WIDE;
    case CameraLensKind::ultraWide: return VIRTIO_VIDEO_CAMERA_LENS_ULTRA_WIDE;
    case CameraLensKind::telephoto: return VIRTIO_VIDEO_CAMERA_LENS_TELEPHOTO;
    case CameraLensKind::external: return VIRTIO_VIDEO_CAMERA_LENS_EXTERNAL;
    }
    return VIRTIO_VIDEO_CAMERA_LENS_EXTERNAL;
}

std::uint32_t facingOf(CameraPosition position) noexcept
{
    switch (position) {
    case CameraPosition::back: return VIRTIO_VIDEO_CAMERA_FACING_BACK;
    case CameraPosition::front: return VIRTIO_VIDEO_CAMERA_FACING_FRONT;
    case CameraPosition::external: return VIRTIO_VIDEO_CAMERA_FACING_EXTERNAL;
    }
    return VIRTIO_VIDEO_CAMERA_FACING_EXTERNAL;
}

std::uint32_t bayerOf(BayerOrder order) noexcept
{
    switch (order) {
    case BayerOrder::rggb: return VIRTIO_VIDEO_CAMERA_BAYER_RGGB;
    case BayerOrder::grbg: return VIRTIO_VIDEO_CAMERA_BAYER_GRBG;
    case BayerOrder::gbrg: return VIRTIO_VIDEO_CAMERA_BAYER_GBRG;
    case BayerOrder::bggr: return VIRTIO_VIDEO_CAMERA_BAYER_BGGR;
    case BayerOrder::none: break;
    }
    return VIRTIO_VIDEO_CAMERA_BAYER_NONE;
}

std::uint32_t q16(double value) noexcept
{
    return std::uint32_t(std::int32_t(std::lround(value * 65536.0)));
}

/* The frame-rate ranges a size of the camera runs at: the union of the ranges
 * of every platform format that covers the size. */
std::vector<std::pair<std::uint32_t, std::uint32_t>> rangesOf(const CameraInfo& info, std::uint32_t width,
                                                              std::uint32_t height)
{
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    for (const auto& format : info.videoFormats)
        if (format.width >= width && format.height >= height) ranges.emplace_back(format.minimumFrameRate, format.maximumFrameRate);
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<std::uint32_t, std::uint32_t>> merged;
    for (const auto& range : ranges) {
        if (!merged.empty() && range.first <= merged.back().second + 1) merged.back().second = std::max(merged.back().second, range.second);
        else merged.push_back(range);
    }
    return merged;
}

std::vector<VideoMode> videoModes(const CameraInfo& info, const CameraTierLimits& limits)
{
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes;
    std::uint32_t largestWidth = 0, largestHeight = 0;
    for (const auto& format : info.videoFormats) {
        if (format.width > limits.maximumVideoWidth || format.height > limits.maximumVideoHeight) continue;
        if (format.width % 2 || format.height % 2) continue;
        if (std::find(sizes.begin(), sizes.end(), std::make_pair(format.width, format.height)) == sizes.end())
            sizes.emplace_back(format.width, format.height);
        largestWidth = std::max(largestWidth, format.width);
        largestHeight = std::max(largestHeight, format.height);
    }
    for (const auto& [width, height] : standardSizes) {
        if (width > largestWidth || height > largestHeight) continue;
        if (std::find(sizes.begin(), sizes.end(), std::make_pair(width, height)) == sizes.end()) sizes.emplace_back(width, height);
    }
    std::sort(sizes.begin(), sizes.end(), [](const auto& a, const auto& b) {
        return std::uint64_t(a.first) * a.second > std::uint64_t(b.first) * b.second ||
            (std::uint64_t(a.first) * a.second == std::uint64_t(b.first) * b.second && a.first > b.first);
    });
    std::vector<VideoMode> modes;
    for (const auto& [width, height] : sizes)
        for (const auto& range : rangesOf(info, width, height)) modes.push_back({width, height, range.first, range.second});
    return modes;
}

void fillWire(CameraDescription& camera)
{
    const auto& info = camera.info;
    auto& wire = camera.wire;
    wire = {};
    wire.index = camera.index;
    wire.facing = facingOf(info.position);
    wire.lens = lensOf(info.lens);
    std::uint32_t flags = 0;
    if (info.autofocus) flags |= VIRTIO_VIDEO_CAMERA_F_AUTOFOCUS;
    if (info.flash) flags |= VIRTIO_VIDEO_CAMERA_F_FLASH;
    if (info.torch) flags |= VIRTIO_VIDEO_CAMERA_F_TORCH;
    if (info.zoomMaximum > info.zoomMinimum) flags |= VIRTIO_VIDEO_CAMERA_F_ZOOM;
    if (info.manualExposure) flags |= VIRTIO_VIDEO_CAMERA_F_MANUAL_EXPOSURE;
    if (info.manualFocus) flags |= VIRTIO_VIDEO_CAMERA_F_MANUAL_FOCUS;
    if (info.manualWhiteBalance) flags |= VIRTIO_VIDEO_CAMERA_F_MANUAL_WHITE_BALANCE;
    if (camera.raw()) flags |= VIRTIO_VIDEO_CAMERA_F_RAW;
    wire.flags = flags;
    wire.sensor_orientation = camera.sensorOrientation;
    wire.pixel_array_width = info.pixelArrayWidth;
    wire.pixel_array_height = info.pixelArrayHeight;
    const double sensorWidth = info.pixelArrayWidth * nominalPitchMicrometres;
    const double sensorHeight = info.pixelArrayHeight * nominalPitchMicrometres;
    wire.sensor_width_um = std::uint32_t(std::lround(sensorWidth));
    wire.sensor_height_um = std::uint32_t(std::lround(sensorHeight));
    // The focal length is the one that gives the field of view the host states
    // for this sensor size; a Bayer still's own figure replaces it.
    double focal = 0;
    if (camera.calibration && camera.calibration->focalLengthMillimetres > 0) {
        focal = camera.calibration->focalLengthMillimetres * 1000.0;
    } else if (info.horizontalFieldOfView > 1.0f && info.horizontalFieldOfView < 179.0f) {
        focal = (sensorWidth / 2.0) / std::tan(double(info.horizontalFieldOfView) * std::numbers::pi / 360.0);
    } else {
        focal = sensorWidth;
    }
    wire.focal_length_um = std::uint32_t(std::lround(focal));
    wire.aperture_x100 = std::uint32_t(std::lround(info.aperture * 100.0f));
    wire.min_focus_distance_mm = std::uint32_t(std::lround(info.minimumFocusDistanceMillimetres));
    wire.iso_min = info.isoMinimum;
    wire.iso_max = info.isoMaximum;
    wire.exposure_min_ns = info.exposureMinimumNanoseconds;
    wire.exposure_max_ns = info.exposureMaximumNanoseconds;
    wire.zoom_min_q16 = q16(info.zoomMinimum);
    wire.zoom_max_q16 = q16(info.zoomMaximum);
    wire.num_switch_overs = std::uint32_t(std::min<std::size_t>(info.switchOvers.size(), VIRTIO_VIDEO_CAMERA_MAX_SWITCH_OVERS));
    for (std::uint32_t index = 0; index < wire.num_switch_overs; ++index) wire.switch_over_q16[index] = q16(info.switchOvers[index]);
    wire.max_wb_gain_q16 = q16(info.maximumWhiteBalanceGain);
    if (camera.calibration) {
        const auto& color = *camera.calibration;
        for (std::size_t index = 0; index < 4; ++index) wire.intrinsics_q16[index] = q16(color.intrinsics[index]);
        if (camera.raw()) {
            wire.bayer_order = bayerOf(color.bayer);
            wire.black_level = color.blackLevel;
            wire.white_level = color.whiteLevel;
            wire.raw_width = camera.rawSize.width;
            wire.raw_height = camera.rawSize.height;
            wire.reference_illuminant1 = color.illuminant1;
            wire.reference_illuminant2 = color.illuminant2;
            for (std::size_t index = 0; index < 9; ++index) {
                wire.color_matrix1_q16[index] = q16(color.colorMatrix1[index]);
                wire.color_matrix2_q16[index] = q16(color.colorMatrix2[index]);
                wire.forward_matrix1_q16[index] = q16(color.forwardMatrix1[index]);
                wire.forward_matrix2_q16[index] = q16(color.forwardMatrix2[index]);
            }
        }
    }
}

} // namespace

std::uint32_t rawWireFormat(BayerOrder order) noexcept
{
    switch (order) {
    case BayerOrder::rggb: return VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_RGGB;
    case BayerOrder::grbg: return VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_GRBG;
    case BayerOrder::gbrg: return VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_GBRG;
    case BayerOrder::bggr: return VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_BGGR;
    case BayerOrder::none: break;
    }
    return 0;
}

BayerOrder bayerOrderOf(std::uint32_t wireFormat) noexcept
{
    switch (wireFormat) {
    case VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_RGGB: return BayerOrder::rggb;
    case VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_GRBG: return BayerOrder::grbg;
    case VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_GBRG: return BayerOrder::gbrg;
    case VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_BGGR: return BayerOrder::bggr;
    default: return BayerOrder::none;
    }
}

namespace {

/* The photo sizes a camera whose still sizes depend on its format offers, and
 * the streams that go with them: a size is offered when the format of every
 * stream the camera permits makes it (uml::platform::offerStills), and a
 * stream faster than the photos hold for is not permitted. */
void offerCameraStills(CameraDescription& camera, const CameraTierLimits& limits,
                 std::span<const std::vector<std::size_t>> concurrentSets)
{
    std::vector<uml::platform::VideoRequest> requests;
    std::uint32_t fastest = 0;
    for (const auto& mode : camera.video) {
        requests.push_back({mode.width, mode.height, mode.minimumFrameRate, mode.maximumFrameRate});
        fastest = std::max(fastest, mode.maximumFrameRate);
    }
    const bool concurrent = std::any_of(concurrentSets.begin(), concurrentSets.end(), [&](const auto& set) {
        return set.size() > 1 && std::find(set.begin(), set.end(), camera.platformIndex) != set.end();
    });
    const auto offer = uml::platform::offerStills(camera.info.captureFormats, requests, concurrent,
                                                  uml::platform::CameraLimits {limits.maximumStillPixels}, stillsHoldUntilRate);
    camera.jpegSizes = offer.sizes;
    camera.info.stillSizes = offer.sizes;
    if (offer.maximumRate >= fastest) return;
    // The streams that are faster are not permitted.
    auto& formats = camera.info.videoFormats;
    std::erase_if(formats, [&](const auto& format) { return format.minimumFrameRate > offer.maximumRate; });
    for (auto& format : formats) format.maximumFrameRate = std::min(format.maximumFrameRate, offer.maximumRate);
    camera.video = videoModes(camera.info, limits);
}

} // namespace

std::vector<CameraDescription> arrangeCameras(std::span<const CameraInfo> infos,
                                              std::span<const std::vector<std::size_t>> concurrentSets,
                                              std::string_view hostModel, const CalibrationStore& calibration,
                                              const ArrangementOptions& options)
{
    // Wire order.
    std::vector<std::size_t> order;
    const auto contains = [&](std::size_t index) { return std::find(order.begin(), order.end(), index) != order.end(); };
    const auto take = [&](auto&& wanted) {
        for (std::size_t index = 0; index < infos.size(); ++index)
            if (!contains(index) && wanted(infos[index])) {
                order.push_back(index);
                return true;
            }
        return false;
    };
    const auto lens = [](CameraPosition position, CameraLensKind kind) {
        return [position, kind](const CameraInfo& info) { return info.position == position && info.lens == kind; };
    };
    // The primary back camera.
    if (!take(lens(CameraPosition::back, CameraLensKind::zoom)))
        for (const auto kind : {CameraLensKind::wide, CameraLensKind::ultraWide, CameraLensKind::telephoto})
            if (take(lens(CameraPosition::back, kind))) break;
    // The front camera.
    if (!take(lens(CameraPosition::front, CameraLensKind::wide)))
        take([](const CameraInfo& info) { return info.position == CameraPosition::front; });
    // The other back lenses.
    for (const auto kind : {CameraLensKind::wide, CameraLensKind::ultraWide, CameraLensKind::telephoto})
        take(lens(CameraPosition::back, kind));
    // External cameras.
    while (order.size() < maximumCameras && take([](const CameraInfo& info) { return info.position == CameraPosition::external; })) {
    }
    order.resize(std::min<std::size_t>(order.size(), maximumCameras));

    const auto limits = limitsOf(options.tier);
    std::vector<CameraDescription> cameras;
    for (std::size_t wire = 0; wire < order.size(); ++wire) {
        CameraDescription camera;
        camera.index = std::uint32_t(wire);
        camera.platformIndex = order[wire];
        camera.info = infos[order[wire]];
        const auto& info = camera.info;
        camera.sensorOrientation = options.sensorOrientation ? options.sensorOrientation(info) : info.sensorOrientation;
        camera.video = videoModes(info, limits);
        if (info.captureFormats.empty()) {
            // Every size works with every format.
            for (const auto& size : info.stillSizes)
                if (std::uint64_t(size.width) * size.height <= limits.maximumStillPixels) camera.jpegSizes.push_back(size);
        } else {
            offerCameraStills(camera, limits, concurrentSets);
        }
        // RAW is advertised only for a lens whose colour data is known.
        if (info.rawStills) {
            camera.calibration = calibration.load(hostModel, CalibrationStore::lensKey(info));
            if (camera.calibration && camera.calibration->bayer != BayerOrder::none && camera.calibration->rawWidth != 0 &&
                std::uint64_t(camera.calibration->rawWidth) * camera.calibration->rawHeight <= limits.maximumStillPixels &&
                camera.calibration->whiteLevel > camera.calibration->blackLevel)
                camera.rawSize = {camera.calibration->rawWidth, camera.calibration->rawHeight};
        }
        cameras.push_back(std::move(camera));
    }

    for (auto& camera : cameras) fillWire(camera);
    // Which cameras run together: those a platform set names.
    const auto together = [&](std::size_t a, std::size_t b) {
        return std::any_of(concurrentSets.begin(), concurrentSets.end(), [&](const auto& set) {
            return std::find(set.begin(), set.end(), a) != set.end() && std::find(set.begin(), set.end(), b) != set.end();
        });
    };
    for (auto& camera : cameras) {
        for (const auto& other : cameras) {
            if (other.index == camera.index) continue;
            if (together(camera.platformIndex, other.platformIndex)) camera.wire.concurrent_mask |= 1u << other.index;
            else camera.wire.conflict_mask |= 1u << other.index;
        }
        for (const auto& name : camera.info.constituents)
            for (const auto& other : cameras)
                if (other.info.identifier == name) {
                    camera.wire.constituent_mask |= 1u << other.index;
                    // A zoom camera and its lenses share the sensors.
                    camera.wire.concurrent_mask &= ~(1u << other.index);
                    camera.wire.conflict_mask |= 1u << other.index;
                }
    }
    // A conflict is mutual.
    for (auto& camera : cameras)
        for (auto& other : cameras)
            if (camera.wire.conflict_mask >> other.index & 1u) {
                other.wire.conflict_mask |= 1u << camera.index;
                other.wire.concurrent_mask &= ~(1u << camera.index);
            }
    return cameras;
}

std::vector<std::uint8_t> cameraQueryResponse(std::span<const CameraDescription> cameras, std::uint32_t start,
                                              std::size_t capacity, std::uint32_t sessionFlags)
{
    virtio_video_camera_query_resp response {};
    response.hdr = {VIRTIO_VIDEO_RESP_OK_CAMERA_QUERY, 0};
    response.num_cameras = std::uint32_t(cameras.size());
    response.session_flags = sessionFlags;
    response.desc_size = sizeof(virtio_video_camera_desc);
    const std::size_t room = capacity > sizeof(response) ? (capacity - sizeof(response)) / sizeof(virtio_video_camera_desc) : 0;
    const std::size_t count = start >= cameras.size() ? 0 : std::min<std::size_t>(cameras.size() - start, room);
    // A buffer with no room for a record would answer "complete" and tell the
    // driver there is nothing more.
    if (count == 0 && start < cameras.size()) throw std::invalid_argument("camera response is too small");
    response.num_descs = std::uint32_t(count);
    response.next_index = start + count < cameras.size() ? std::uint32_t(start + count) : 0;
    auto bytes = toBytes(response);
    for (std::size_t index = 0; index < count; ++index) {
        const auto record = toBytes(cameras[start + index].wire);
        bytes.insert(bytes.end(), record.begin(), record.end());
    }
    return bytes;
}

std::vector<std::uint8_t> cameraFormatsResponse(const CameraDescription& camera, std::uint32_t kind)
{
    std::vector<std::uint8_t> descs;
    std::uint32_t count = 0;
    const auto append = [&descs](const auto& value) {
        const auto bytes = toBytes(value);
        descs.insert(descs.end(), bytes.begin(), bytes.end());
    };
    const auto range = [](std::uint32_t low, std::uint32_t high, std::uint32_t step) {
        return virtio_video_format_range {low, high, step, {}};
    };
    const auto descriptor = [&](std::uint32_t format, std::uint32_t frames) {
        virtio_video_format_desc desc {};
        desc.format = format;
        desc.planes_layout = VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER;
        desc.plane_align = 16;
        desc.num_frames = frames;
        return desc;
    };
    if (kind == VIRTIO_VIDEO_CAMERA_STREAM_VIDEO) {
        // One frame record per size, with its rate ranges.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes;
        for (const auto& mode : camera.video)
            if (std::find(sizes.begin(), sizes.end(), std::make_pair(mode.width, mode.height)) == sizes.end())
                sizes.emplace_back(mode.width, mode.height);
        if (!sizes.empty()) {
            append(descriptor(VIRTIO_VIDEO_FORMAT_NV12, std::uint32_t(sizes.size())));
            for (const auto& [width, height] : sizes) {
                std::uint32_t rates = 0;
                for (const auto& mode : camera.video) rates += mode.width == width && mode.height == height;
                append(virtio_video_format_frame {range(width, width, 1), range(height, height, 1), rates, {}});
                for (const auto& mode : camera.video)
                    if (mode.width == width && mode.height == height) append(range(mode.minimumFrameRate, mode.maximumFrameRate, 1));
            }
            ++count;
        }
    } else if (kind == VIRTIO_VIDEO_CAMERA_STREAM_STILL) {
        if (!camera.jpegSizes.empty()) {
            append(descriptor(VIRTIO_VIDEO_FORMAT_CAMERA_JPEG, std::uint32_t(camera.jpegSizes.size())));
            for (const auto& size : camera.jpegSizes)
                append(virtio_video_format_frame {range(size.width, size.width, 1), range(size.height, size.height, 1), 0, {}});
            ++count;
        }
        if (camera.raw()) {
            append(descriptor(rawWireFormat(camera.calibration->bayer), 1));
            append(virtio_video_format_frame {range(camera.rawSize.width, camera.rawSize.width, 1),
                                              range(camera.rawSize.height, camera.rawSize.height, 1), 0, {}});
            ++count;
        }
    }
    virtio_video_query_capability_resp response {};
    response.hdr = {VIRTIO_VIDEO_RESP_OK_QUERY_CAPABILITY, 0};
    response.num_descs = count;
    auto bytes = toBytes(response);
    bytes.insert(bytes.end(), descs.begin(), descs.end());
    return bytes;
}

std::uint32_t supportedFrameRate(const CameraDescription& camera, std::uint32_t width, std::uint32_t height,
                                 std::uint32_t rate) noexcept
{
    if (!videoSizeAllowed(camera, width, height)) return 0;
    std::uint32_t best = 0;
    std::uint32_t bestDistance = ~0u;
    for (const auto& range : rangesOf(camera.info, width, height)) {
        const auto clamped = std::clamp(rate, range.first, range.second);
        const auto distance = clamped > rate ? clamped - rate : rate - clamped;
        if (distance < bestDistance) {
            best = clamped;
            bestDistance = distance;
        }
    }
    return best;
}

bool videoSizeAllowed(const CameraDescription& camera, std::uint32_t width, std::uint32_t height) noexcept
{
    if (width < 16 || height < 16 || (width & 1) || (height & 1)) return false;
    for (const auto& mode : camera.video)
        if (mode.width >= width && mode.height >= height) return true;
    return false;
}

CameraLayout summarizeLayout(std::span<const CameraDescription> cameras, CameraTier tier) noexcept
{
    CameraLayout layout;
    layout.tier = tier;
    for (const auto& camera : cameras) {
        const auto& info = camera.info;
        if (info.position == CameraPosition::back) {
            layout.back = true;
            layout.autofocus |= info.autofocus;
            layout.flash |= info.flash;
        }
        if (info.position == CameraPosition::front) {
            layout.front = true;
            for (const auto& mode : camera.video)
                layout.frontMaximumVideoHeight = std::max(layout.frontMaximumVideoHeight, mode.height);
        }
        layout.manualSensor |= info.manualExposure;
        layout.raw |= camera.raw();
    }
    for (const auto& a : cameras)
        for (const auto& b : cameras)
            if (a.info.position == CameraPosition::back && b.info.position == CameraPosition::front &&
                (a.wire.concurrent_mask >> b.index & 1u))
                layout.concurrent = true;
    return layout;
}

} // namespace uml::camera
