// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The one byte count of an NV12 capture buffer: the camera offer's frame bound
// (photo_dimensions.hpp) and the host memory reserve (device/camera/
// camera_tiers.hpp) both use it. It stands alone because the application's
// runtime loader compiles the reserve without the platform's include path.

#include <cstdint>

namespace uml::platform {

// The bytes of an NV12 frame of `width` by `height` in a capture buffer whose
// rows are padded to `rowAlignment` bytes (zero or one: not padded): the luma
// rows and the chroma rows, which are half as many, rounded up.
[[nodiscard]] constexpr std::uint64_t alignedFrameBytes(std::uint32_t width, std::uint32_t height, std::uint32_t rowAlignment) noexcept
{
    const std::uint64_t alignment = rowAlignment != 0 ? rowAlignment : 1;
    const std::uint64_t pitch = (std::uint64_t(width) + alignment - 1) / alignment * alignment;
    return pitch * (std::uint64_t(height) + (std::uint64_t(height) + 1) / 2);
}

} // namespace uml::platform
