// SPDX-License-Identifier: GPL-2.0-or-later
#include "photo_dimensions.hpp"

#include <algorithm>

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

std::optional<std::size_t> chooseVideoFormat(std::span<const VideoFormatFacts> formats, std::uint32_t width, std::uint32_t height,
                                             std::uint32_t frameRate, bool multiCameraOnly, CameraStillSize preferredStill)
{
    std::optional<std::size_t> best;
    std::uint64_t bestArea = 0;
    bool bestLists = false;
    for (std::size_t index = 0; index < formats.size(); ++index) {
        const auto& format = formats[index];
        if (format.width < width || format.height < height) continue;
        if (multiCameraOnly && !format.multiCamera) continue;
        const bool rate = std::any_of(format.frameRates.begin(), format.frameRates.end(), [&](const FrameRateSpan& span) {
            return span.minimum <= frameRate + 1e-3 && span.maximum >= frameRate - 1e-3;
        });
        if (!rate) continue;
        const std::uint64_t area = std::uint64_t(format.width) * format.height;
        const bool lists = preferredStill.width != 0 &&
            std::find(format.photoSizes.begin(), format.photoSizes.end(), preferredStill) != format.photoSizes.end();
        const bool better = !best || (lists && !bestLists) ||
            (lists == bestLists && (area < bestArea || (area == bestArea && format.fullRange && !formats[*best].fullRange)));
        if (better) {
            best = index;
            bestArea = area;
            bestLists = lists;
        }
    }
    return best;
}

std::vector<CameraStillSize> photoSizesOfEveryRun(std::span<const VideoFormatFacts> formats, std::span<const VideoRequest> requests,
                                                  CameraStillSize preferredStill)
{
    std::vector<CameraStillSize> common;
    bool first = true;
    for (const auto& request : requests) {
        for (const std::uint32_t rate : {request.minimumFrameRate, request.maximumFrameRate}) {
            const auto index = chooseVideoFormat(formats, request.width, request.height, rate, false, preferredStill);
            if (!index) continue;
            const auto& listed = formats[*index].photoSizes;
            if (first) {
                common = listed;
                first = false;
            } else {
                std::erase_if(common, [&](CameraStillSize size) { return std::find(listed.begin(), listed.end(), size) == listed.end(); });
            }
        }
    }
    std::erase_if(common, [](CameraStillSize size) { return size.width == 0 || size.height == 0; });
    std::sort(common.begin(), common.end(), [](CameraStillSize a, CameraStillSize b) {
        const auto areaA = std::uint64_t(a.width) * a.height, areaB = std::uint64_t(b.width) * b.height;
        return areaA != areaB ? areaA > areaB : a.width > b.width;
    });
    common.erase(std::unique(common.begin(), common.end()), common.end());
    return common;
}

bool resolvedSizeIsRequested(CameraStillSize resolved, CameraStillSize requested) noexcept
{
    return (resolved.width == 0 && resolved.height == 0) || resolved == requested;
}

} // namespace uml::platform
