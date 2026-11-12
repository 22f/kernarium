// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "calibration_store.hpp"
#include "camera.hpp"
#include "camera_tiers.hpp"
#include "virtio_video.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace uml::camera {

/* How the camera device names what the platform's cameras can do on the Virtio
 * wire. Every function here is a pure function of the platform's description,
 * the tier and the calibration, so a test checks it without a device. */

/* What a guest of a tier may ask of the platform's cameras, which the formats
 * the platform runs them in must serve: the offered stills and the frames
 * delivered. */
[[nodiscard]] constexpr uml::platform::CameraLimits cameraLimitsOf(const CameraTierLimits& limits) noexcept
{
    return {limits.maximumStillPixels, maximumDeliveredFrameBytes(limits)};
}

/* A video size and the frame rates it runs at. */
struct VideoMode {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t minimumFrameRate {};
    std::uint32_t maximumFrameRate {};
};

/* One camera as the guest sees it. */
struct CameraDescription {
    /* What every wire command calls the camera. */
    std::uint32_t index {};
    /* The platform's index (`Camera::cameras()`). */
    std::size_t platformIndex {};
    uml::platform::CameraInfo info;
    /* The colour data learned for the lens, when it has some. */
    std::optional<uml::platform::CameraColorData> calibration;
    std::vector<VideoMode> video;
    std::vector<uml::platform::CameraStillSize> jpegSizes;
    /* The Bayer still's size; zero when the camera has none (or none is calibrated). */
    uml::platform::CameraStillSize rawSize;
    std::uint32_t sensorOrientation {};
    virtio_video_camera_desc wire {};

    [[nodiscard]] bool raw() const noexcept { return rawSize.width != 0; }
};

struct ArrangementOptions {
    CameraTier tier {CameraTier::small};
    /* The application's statement of a camera's SENSOR_ORIENTATION; the
     * platform's own is used where it states none. */
    std::function<std::uint32_t(const uml::platform::CameraInfo&)> sensorOrientation;
};

/* The wire's cameras for a host, in wire order: the back zoom camera (or the
 * back wide lens where there is none), the front camera, the other back lenses
 * and then any external camera. At most VIRTIO_VIDEO_CAMERA_MAX_CAMERAS. A
 * zoom camera and its lenses are separate cameras that conflict with each
 * other; any two cameras conflict unless the host runs them together. */
[[nodiscard]] std::vector<CameraDescription> arrangeCameras(
    std::span<const uml::platform::CameraInfo> infos, std::span<const std::vector<std::size_t>> concurrentSets,
    std::string_view hostModel, const CalibrationStore& calibration, const ArrangementOptions& options);

/* The answer to VIRTIO_VIDEO_CMD_CAMERA_QUERY from `start`, as many records as
 * fit in `capacity` bytes. Throws std::invalid_argument when not one fits. */
[[nodiscard]] std::vector<std::uint8_t> cameraQueryResponse(std::span<const CameraDescription> cameras,
                                                            std::uint32_t start, std::size_t capacity,
                                                            std::uint32_t sessionFlags);

/* The answer to VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS. */
[[nodiscard]] std::vector<std::uint8_t> cameraFormatsResponse(const CameraDescription& camera, std::uint32_t kind);

/* The wire format of a Bayer order, and the order of a wire format. */
[[nodiscard]] std::uint32_t rawWireFormat(uml::platform::BayerOrder order) noexcept;
[[nodiscard]] uml::platform::BayerOrder bayerOrderOf(std::uint32_t wireFormat) noexcept;

/* Whether `rate` frames per second is one the camera runs a stream of this size
 * at, and the nearest rate that is. Zero when the size is not a size of the
 * camera at all. */
[[nodiscard]] std::uint32_t supportedFrameRate(const CameraDescription& camera, std::uint32_t width,
                                               std::uint32_t height, std::uint32_t rate) noexcept;

/* The largest picture size a stream of the camera may have. */
[[nodiscard]] bool videoSizeAllowed(const CameraDescription& camera, std::uint32_t width, std::uint32_t height) noexcept;

/* What the layout of the guest's image depends on. */
struct CameraLayout {
    CameraTier tier {CameraTier::small};
    bool back {};
    bool front {};
    bool autofocus {};   // a back camera focuses
    bool flash {};       // a back camera has a flash
    bool manualSensor {};  // some camera takes manual exposure
    bool raw {};         // some camera has a calibrated Bayer still
    bool concurrent {};  // front and back cameras run together
    std::uint32_t frontMaximumVideoHeight {};
};

[[nodiscard]] CameraLayout summarizeLayout(std::span<const CameraDescription> cameras, CameraTier tier) noexcept;

} // namespace uml::camera
