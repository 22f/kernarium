// SPDX-License-Identifier: Apache-2.0
//
// The crop region that a result of the camera HAL states. For a HAL that lists
// the zoom ratio the camera service splits an application's zoom into
// CONTROL_ZOOM_RATIO and a residual SCALER_CROP_REGION (what the ratio cannot
// express, whole pixels of the active array) and joins them again in the result:
// it unscales the result's crop region by the result's ratio. A result that
// states the whole array instead of the request's region loses the residual, and
// the application's region is a pixel or three off once the ratio is high (about
// 57x on a 4032 x 3024 array, CTS testDigitalZoom). So the result states the
// region the request asked for, clamped into the array; a request with no region,
// an empty one, or one outside the array gets the array.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace andarium::camera {

// The layout of android.scaler.cropRegion: left, top, width, height.
struct CropRegion {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

// `requested` is the request's SCALER_CROP_REGION entry (null or short when the
// request has none), `array` the region the result falls back to and is clamped
// into: the active array of the sensor mode in use.
[[nodiscard]] constexpr CropRegion result_crop_region(const std::int32_t* requested, std::size_t count,
                                                      const CropRegion& array) noexcept
{
    if (requested == nullptr || count < 4 || requested[2] <= 0 || requested[3] <= 0) {
        return array;
    }
    const std::int64_t left = std::max<std::int64_t>(requested[0], array.left);
    const std::int64_t top = std::max<std::int64_t>(requested[1], array.top);
    const std::int64_t right = std::min<std::int64_t>(std::int64_t{requested[0]} + requested[2],
                                                      std::int64_t{array.left} + array.width);
    const std::int64_t bottom = std::min<std::int64_t>(std::int64_t{requested[1]} + requested[3],
                                                       std::int64_t{array.top} + array.height);
    if (right <= left || bottom <= top) {
        return array;
    }
    return {static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
            static_cast<std::int32_t>(right - left), static_cast<std::int32_t>(bottom - top)};
}

} // namespace andarium::camera
