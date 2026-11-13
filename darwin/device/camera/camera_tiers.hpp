// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The application's runtime loader includes this header with no include path.
#include "../../platform/frame_bytes.hpp"

#include <cstdint>

namespace uml::camera {

/* The size caps of a session, fixed by the host's fixed GPU budget (the
 * runtime's launch budget, which a small device has little of). The tier is a
 * dimension of the camera layout, because the media profiles depend on it. */
enum class CameraTier : std::uint8_t {
    /* Video up to 1080p, stills up to 12 MP, four buffers per stream. */
    small,
    /* Video up to 4K, stills up to 48 MP, six buffers per stream. */
    large,
};

struct CameraTierLimits {
    std::uint32_t maximumVideoWidth {};
    std::uint32_t maximumVideoHeight {};
    std::uint64_t maximumStillPixels {};
    /* The buffers a stream is asked to queue (the pipeline depth). */
    std::uint32_t buffers {};
};

/* How many frames of one camera the host holds at once: the device's inbox
 * (`inboxFramesPerCamera`, the newest few of a camera), the frame the pump is
 * writing to the guest, the frame the platform's callback is delivering and the
 * one the capture is filling. */
inline constexpr std::uint32_t inboxFramesPerCamera = 3;
inline constexpr std::uint32_t heldFramesPerCamera = inboxFramesPerCamera + 3;
/* The cameras that stream at once: the front and the back camera. */
inline constexpr std::uint32_t concurrentCameras = 2;
/* The row alignment the platform's capture buffers were measured at: a 1920x1440
 * NV12 buffer takes 4,423,680 bytes where its pixels are 4,147,200. The device
 * checks the first buffer of each configuration against it. */
inline constexpr std::uint32_t captureRowAlignment = 256;

inline constexpr std::uint64_t largeTierBudgetBytes = 1ull << 30;

/* The largest tier any host is given for now: 1080p video and 12 MP stills,
 * whatever its GPU budget. The tier code and the layouts of the larger tier
 * stay, so lifting the cap is changing this constant. */
inline constexpr CameraTier largestOfferedTier = CameraTier::small;

[[nodiscard]] constexpr CameraTier cameraTierForBudget(std::uint64_t gpuBudgetBytes) noexcept
{
    const CameraTier byBudget = gpuBudgetBytes < largeTierBudgetBytes ? CameraTier::small : CameraTier::large;
    return byBudget > largestOfferedTier ? largestOfferedTier : byBudget;
}

[[nodiscard]] constexpr CameraTierLimits limitsOf(CameraTier tier) noexcept
{
    // 12 MP is 4032 x 3024 (12.19 million pixels); 48 MP is 8064 x 6048 (48.77 million).
    return tier == CameraTier::small ? CameraTierLimits {1920, 1080, 12'700'000, 4}
                                     : CameraTierLimits {3840, 2160, 50'000'000, 6};
}

/* The bytes of an NV12 frame of `width` by `height` in a capture buffer whose
 * rows are padded to captureRowAlignment: the platform model's byte count
 * (uml::platform::alignedFrameBytes), the one the offer's frame bound and this
 * reserve share. */
[[nodiscard]] constexpr std::uint64_t alignedFrameBytes(std::uint32_t width, std::uint32_t height) noexcept
{
    return uml::platform::alignedFrameBytes(width, height, captureRowAlignment);
}

/* The largest capture buffer the host holds for a tier, and so the largest frame
 * the offer lets a stream deliver: a frame of the tier's width at 4:3, the
 * tallest aspect ratio a camera format has, at the aligned row size. The
 * platform scales a format's frames to the stream's size at the format's own
 * aspect ratio, so a 4:3 format delivers a 1920x1440 frame to a 1080p stream; a
 * format whose frame for a size it offers would take more is not offered that
 * size (uml::platform::offerOf, with this bound and captureRowAlignment). */
[[nodiscard]] constexpr std::uint64_t maximumDeliveredFrameBytes(const CameraTierLimits& limits) noexcept
{
    return alignedFrameBytes(limits.maximumVideoWidth, (limits.maximumVideoWidth * 3 + 3) / 4);
}

/* What a layout lets the host hold at once, which the reserve counts: the
 * cameras that stream together (two only for back and front cameras that run
 * together, one otherwise) and whether a Bayer RAW still, with its DNG, can be
 * in flight beside the JPEG. The default is the fullest use there is, the
 * ceiling of a tier. */
struct CameraUse {
    std::uint32_t streams {concurrentCameras};
    bool rawStill {true};

    friend bool operator==(const CameraUse&, const CameraUse&) = default;
};

/* The host-side memory a live session of a tier can hold at its worst, in
 * bytes, outside the guest's RAM, for what a layout lets it use (`CameraUse`).
 * A live session takes it out of the GPU budget B (gpu_pool_bytes in
 * platform/gpu_budget.hpp), so the application's memory limit holds it. Each
 * part is derived from the tier's limits:
 *  - the capture pools: the largest capture buffer the offer lets a stream
 *    take (maximumDeliveredFrameBytes), for each frame the host holds
 *    (heldFramesPerCamera) of each camera that streams at once
 *    (`CameraUse::streams`);
 *  - the device's staging: one NV12 frame of the largest video size, where a frame is
 *    cropped, rotated and scaled before it is copied to the guest;
 *  - the transformer's own scratch, which the platform's transformer keeps between
 *    frames and which the staging does not cover: a scaled pixel buffer and a rotation
 *    scratch of one such frame each, and vImage's temporaries, measured at 3,691,696
 *    bytes for the largest scale of the small tier (a 1080p frame turned a quarter),
 *    which is 1.19 frames and counted as 1.2;
 *  - the still's transients, for one 12 MP RAW still with its DNG, only where the
 *    layout has RAW (`CameraUse::rawStill`): the RAW pixels (2 bytes each) three
 *    times, the platform's buffer, the device's copy and the DNG that comes with it;
 *  - and the JPEG that goes with a still, three copies of a half byte a pixel
 *    (the file, the device's copy and the copy that reserves the Exif gap).
 * Nothing the guest is offered depends on it: the offer is bounded by the same
 * buffer (maximumDeliveredFrameBytes), whatever the layout. */
struct CameraHostMemory {
    std::uint64_t capturePools {};
    std::uint64_t staging {};
    std::uint64_t transformerScratch {};
    std::uint64_t rawStill {};
    std::uint64_t jpegStill {};

    [[nodiscard]] constexpr std::uint64_t total() const noexcept
    {
        return capturePools + staging + transformerScratch + rawStill + jpegStill;
    }
};

[[nodiscard]] constexpr CameraHostMemory cameraHostMemory(CameraTier tier, const CameraUse& use = {}) noexcept
{
    const CameraTierLimits limits = limitsOf(tier);
    const std::uint64_t frame = std::uint64_t {limits.maximumVideoWidth} * limits.maximumVideoHeight * 3 / 2;
    return {
        .capturePools = maximumDeliveredFrameBytes(limits) * heldFramesPerCamera * use.streams,
        .staging = frame,
        .transformerScratch = frame * 2 + frame * 6 / 5,
        .rawStill = use.rawStill ? limits.maximumStillPixels * 2 * 3 : 0,
        .jpegStill = limits.maximumStillPixels / 2 * 3,
    };
}

/* The reserve in whole mebibytes, rounded up: the figure the GPU budget gives
 * up while the cameras are live. With no `CameraUse` it is the tier's ceiling,
 * which a layout that is not known, and the application's plan before the
 * layout is composed, count. */
[[nodiscard]] constexpr std::uint64_t cameraHostMemoryReserveMiB(CameraTier tier, const CameraUse& use = {}) noexcept
{
    constexpr std::uint64_t mebibyte = 1ull << 20;
    return (cameraHostMemory(tier, use).total() + mebibyte - 1) / mebibyte;
}

} // namespace uml::camera
