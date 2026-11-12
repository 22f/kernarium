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

} // namespace uml::camera
