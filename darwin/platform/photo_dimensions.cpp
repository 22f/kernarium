// SPDX-License-Identifier: GPL-2.0-or-later
#include "photo_dimensions.hpp"

namespace uml::platform {
namespace {

[[nodiscard]] bool within(CameraStillSize size, CameraStillSize maximum) noexcept
{
    return size.width <= maximum.width && size.height <= maximum.height;
}

} // namespace

std::optional<CameraStillSize> exactPhotoSize(std::span<const CameraStillSize> listed, CameraStillSize request,
                                              CameraStillSize outputMaximum)
{
    if (request.width == 0 || request.height == 0 || !within(request, outputMaximum)) return std::nullopt;
    for (const auto& size : listed)
        if (size == request) return size;
    return std::nullopt;
}

std::optional<CameraStillSize> coveringPhotoSize(std::span<const CameraStillSize> listed, CameraStillSize minimum,
                                                 CameraStillSize outputMaximum)
{
    std::optional<CameraStillSize> chosen;
    for (const auto& size : listed) {
        if (size.width == 0 || size.height == 0 || size.width < minimum.width || size.height < minimum.height ||
            !within(size, outputMaximum))
            continue;
        if (!chosen || std::uint64_t(size.width) * size.height < std::uint64_t(chosen->width) * chosen->height) chosen = size;
    }
    return chosen;
}

bool resolvedSizeIsRequested(CameraStillSize resolved, CameraStillSize requested) noexcept
{
    return (resolved.width == 0 && resolved.height == 0) || resolved == requested;
}

} // namespace uml::platform
