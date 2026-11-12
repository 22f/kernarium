// SPDX-License-Identifier: GPL-2.0-or-later
#include "photo_dimensions.hpp"

#include <algorithm>
#include <numeric>

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

bool formatMakesPhoto(const VideoFormatFacts& format, CameraStillSize size)
{
    const auto largest = largestPhotoSize(format.photoSizes);
    return largest && exactPhotoSize(format.photoSizes, size, *largest).has_value();
}

namespace {

[[nodiscard]] std::uint64_t areaOf(CameraStillSize size) noexcept
{
    return std::uint64_t(size.width) * size.height;
}

[[nodiscard]] bool largerPhoto(CameraStillSize a, CameraStillSize b) noexcept
{
    return areaOf(a) != areaOf(b) ? areaOf(a) > areaOf(b) : a.width > b.width;
}

[[nodiscard]] bool withinCap(CameraStillSize size, const CameraLimits& limits) noexcept
{
    return limits.maximumStillPixels == 0 || areaOf(size) <= limits.maximumStillPixels;
}

[[nodiscard]] bool takesRate(const VideoFormatFacts& format, std::uint32_t frameRate) noexcept
{
    return std::any_of(format.frameRates.begin(), format.frameRates.end(), [&](const FrameRateSpan& span) {
        return span.minimum <= frameRate + 1e-3 && span.maximum >= frameRate - 1e-3;
    });
}

} // namespace

std::vector<CameraStillSize> photoSizesMade(std::span<const VideoFormatFacts> formats, const CameraLimits& limits)
{
    std::vector<CameraStillSize> sizes;
    for (const auto& format : formats)
        for (const auto& size : format.photoSizes)
            if (size.width != 0 && size.height != 0 && withinCap(size, limits) && formatMakesPhoto(format, size) &&
                std::find(sizes.begin(), sizes.end(), size) == sizes.end())
                sizes.push_back(size);
    std::sort(sizes.begin(), sizes.end(), largerPhoto);
    return sizes;
}

std::uint64_t deliveredFrameBytes(const VideoFormatFacts& format, std::uint32_t width, std::uint32_t height) noexcept
{
    const FrameSize frame = deliveredFrameSize(format.width, format.height, width, height);
    return std::uint64_t(frame.width) * frame.height * 3 / 2;
}

std::optional<CameraStillSize> preferredPhotoSize(std::span<const VideoFormatFacts> formats, const CameraLimits& limits)
{
    const auto sizes = photoSizesMade(formats, limits);
    if (sizes.empty()) return std::nullopt;
    return sizes.front();
}

std::optional<std::size_t> chooseVideoFormat(std::span<const VideoFormatFacts> formats, std::uint32_t width, std::uint32_t height,
                                             std::uint32_t frameRate, bool multiCameraOnly, const CameraLimits& limits)
{
    const auto preferred = preferredPhotoSize(formats, limits);
    std::optional<std::size_t> best;
    std::uint64_t bestArea = 0;
    bool bestMakes = false;
    for (std::size_t index = 0; index < formats.size(); ++index) {
        const auto& format = formats[index];
        if (format.width < width || format.height < height) continue;
        if (multiCameraOnly && !format.multiCamera) continue;
        if (!takesRate(format, frameRate)) continue;
        if (limits.maximumFrameBytes != 0 && deliveredFrameBytes(format, width, height) > limits.maximumFrameBytes) continue;
        const std::uint64_t area = std::uint64_t(format.width) * format.height;
        const bool makes = preferred && formatMakesPhoto(format, *preferred);
        const bool better = !best || (makes && !bestMakes) ||
            (makes == bestMakes && (area < bestArea || (area == bestArea && format.fullRange && !formats[*best].fullRange)));
        if (better) {
            best = index;
            bestArea = area;
            bestMakes = makes;
        }
    }
    return best;
}

std::optional<std::size_t> chooseStillFormat(std::span<const VideoFormatFacts> formats, std::optional<CameraStillSize> wanted,
                                             bool multiCameraOnly, const CameraLimits& limits)
{
    std::optional<std::size_t> best;
    std::uint64_t bestPhoto = 0, bestArea = 0;
    for (std::size_t index = 0; index < formats.size(); ++index) {
        const auto& format = formats[index];
        if (multiCameraOnly && !format.multiCamera) continue;
        std::uint64_t photo = 0;
        if (wanted) {
            if (!formatMakesPhoto(format, *wanted)) continue;
        } else {
            for (const auto& size : format.photoSizes)
                if (withinCap(size, limits) && formatMakesPhoto(format, size)) photo = std::max(photo, areaOf(size));
        }
        const std::uint64_t area = std::uint64_t(format.width) * format.height;
        const bool better = !best || photo > bestPhoto ||
            (photo == bestPhoto && (area < bestArea || (area == bestArea && format.fullRange && !formats[*best].fullRange)));
        if (better) {
            best = index;
            bestPhoto = photo;
            bestArea = area;
        }
    }
    return best;
}

StillOffer offerStills(std::span<const VideoFormatFacts> formats, std::span<const VideoRequest> requests, bool concurrent,
                       const CameraLimits& limits, std::uint32_t holdsUntil)
{
    StillOffer offer;
    // The formats of the runs, by the rate of the run.
    struct Run {
        std::uint32_t rate;
        std::size_t format;
    };
    std::vector<Run> runs;
    for (const auto& request : requests) {
        offer.maximumRate = std::max(offer.maximumRate, request.maximumFrameRate);
        for (std::uint32_t rate = std::max<std::uint32_t>(1, request.minimumFrameRate); rate <= request.maximumFrameRate; ++rate)
            for (const bool several : {false, true}) {
                if (several && !concurrent) continue;
                if (const auto index = chooseVideoFormat(formats, request.width, request.height, rate, several, limits))
                    runs.push_back({rate, *index});
            }
    }
    if (runs.empty()) return offer;
    // A still with no stream running is a run of its own: it takes the
    // format of the size asked for, among the formats of a multi-camera
    // session when the camera may run with others.
    auto candidates = photoSizesMade(formats, limits);
    std::erase_if(candidates, [&](CameraStillSize size) {
        return !chooseStillFormat(formats, size, false, limits) ||
            (concurrent && !chooseStillFormat(formats, size, true, limits));
    });
    const auto holds = [&](CameraStillSize size, std::uint32_t upTo) {
        return std::all_of(runs.begin(), runs.end(), [&](const Run& run) {
            return run.rate > upTo || formatMakesPhoto(formats[run.format], size);
        });
    };
    const std::uint32_t floorRate = std::min(holdsUntil, offer.maximumRate);
    const auto largest = std::find_if(candidates.begin(), candidates.end(), [&](CameraStillSize size) { return holds(size, floorRate); });
    if (largest == candidates.end()) return offer;
    // How fast the streams may run with the largest size still made.
    std::uint32_t ceiling = floorRate;
    while (ceiling < offer.maximumRate && holds(*largest, ceiling + 1)) ++ceiling;
    offer.maximumRate = ceiling;
    for (const auto& size : candidates)
        if (holds(size, ceiling)) offer.sizes.push_back(size);
    return offer;
}

FrameSize deliveredFrameSize(std::uint32_t formatWidth, std::uint32_t formatHeight, std::uint32_t width,
                            std::uint32_t height) noexcept
{
    if (formatWidth == 0 || formatHeight == 0 || width == 0 || height == 0) return {formatWidth, formatHeight};
    // Sizes of the format's aspect are the multiples of its reduced ratio, and
    // an exact ratio is the only scaling the platform accepts.
    const std::uint32_t common = std::gcd(formatWidth, formatHeight);
    const std::uint32_t ratioWidth = formatWidth / common, ratioHeight = formatHeight / common;
    std::uint64_t multiple = std::max((std::uint64_t(width) + ratioWidth - 1) / ratioWidth,
                                      (std::uint64_t(height) + ratioHeight - 1) / ratioHeight);
    // Both sides stay even (a video frame has chroma pairs): a ratio with an
    // odd side wants an even multiple.
    if (((ratioWidth | ratioHeight) & 1u) != 0 && (multiple & 1u) != 0) ++multiple;
    if (multiple >= common) return {formatWidth, formatHeight};
    return {std::uint32_t(multiple * ratioWidth), std::uint32_t(multiple * ratioHeight)};
}

bool resolvedSizeIsRequested(CameraStillSize resolved, CameraStillSize requested) noexcept
{
    return (resolved.width == 0 && resolved.height == 0) || resolved == requested;
}

} // namespace uml::platform
