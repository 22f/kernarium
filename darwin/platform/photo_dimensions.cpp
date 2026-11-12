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

std::optional<CameraStillSize> largestPhotoSize(std::span<const CameraStillSize> listed)
{
    std::optional<CameraStillSize> largest;
    for (const auto& size : listed)
        if (size.width != 0 && size.height != 0 &&
            (!largest || std::uint64_t(size.width) * size.height > std::uint64_t(largest->width) * largest->height))
            largest = size;
    return largest;
}

PhotoOutputPreparation preparePhotoOutput(std::optional<CameraStillSize> largest, const PhotoOutputSteps& steps)
{
    if (!steps.attach()) return PhotoOutputPreparation::notAttached;
    // A format that lists no size leaves the output's own default.
    if (!largest) return PhotoOutputPreparation::ready;
    if (steps.setMaximum(*largest)) return PhotoOutputPreparation::ready;
    steps.detach();
    return PhotoOutputPreparation::maximumRefused;
}

bool resolvedSizeIsRequested(CameraStillSize resolved, CameraStillSize requested) noexcept
{
    return (resolved.width == 0 && resolved.height == 0) || resolved == requested;
}

} // namespace uml::platform
