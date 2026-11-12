// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

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

/* The host-side memory a live session of a tier can hold at its worst, in
 * bytes, outside the guest's RAM. A live session takes it out of the GPU
 * budget B (gpu_pool_bytes in platform/gpu_budget.hpp), so the application's
 * memory limit holds it. Each part is derived from the tier's limits:
 *  - the capture pools: one NV12 frame of the largest video size for each of
 *    the tier's buffers, which the platform's capture session holds;
 *  - the device's staging: one NV12 frame of that size, where a frame is
 *    cropped, rotated and scaled before it is copied to the guest;
 *  - the still's transients, for one 12 MP RAW still with its DNG: the RAW
 *    pixels (2 bytes each) three times, the platform's buffer, the device's
 *    copy and the DNG that comes with it;
 *  - and the JPEG that goes with it, three copies of a half byte a pixel
 *    (the file, the device's copy and the copy that reserves the Exif gap). */
struct CameraHostMemory {
    std::uint64_t capturePools {};
    std::uint64_t staging {};
    std::uint64_t rawStill {};
    std::uint64_t jpegStill {};

    [[nodiscard]] constexpr std::uint64_t total() const noexcept
    {
        return capturePools + staging + rawStill + jpegStill;
    }
};

[[nodiscard]] constexpr CameraHostMemory cameraHostMemory(CameraTier tier) noexcept
{
    const CameraTierLimits limits = limitsOf(tier);
    const std::uint64_t frame = std::uint64_t {limits.maximumVideoWidth} * limits.maximumVideoHeight * 3 / 2;
    return {
        .capturePools = frame * limits.buffers,
        .staging = frame,
        .rawStill = limits.maximumStillPixels * 2 * 3,
        .jpegStill = limits.maximumStillPixels / 2 * 3,
    };
}

/* The reserve in whole mebibytes, rounded up: the figure the GPU budget gives
 * up while the cameras are live. */
[[nodiscard]] constexpr std::uint64_t cameraHostMemoryReserveMiB(CameraTier tier) noexcept
{
    constexpr std::uint64_t mebibyte = 1ull << 20;
    return (cameraHostMemory(tier).total() + mebibyte - 1) / mebibyte;
}

} // namespace uml::camera
