// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Which photo size a camera is asked for, and whether the photo that came is
// the one asked for. A guest configures a JPEG stream at one size and gets
// that size or an error: a platform picture of another size is not passed on.
// The platform lists its photo sizes per device format, and the format a
// camera runs is not the one that was listed first, so a size another format
// lists is not one the active format can make.

#include "camera.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace uml::platform {

// The size to ask the photo output for when a JPEG of `request` is wanted: the
// request itself, when the active format lists it and the output's maximum
// covers it. Nothing otherwise; the nearest larger size is not the request.
[[nodiscard]] std::optional<CameraStillSize> exactPhotoSize(std::span<const CameraStillSize> listed,
                                                            CameraStillSize request, CameraStillSize outputMaximum);

// For a capture that names no JPEG size (a Bayer still alone): the smallest
// listed size that covers `minimum` within the output's maximum.
[[nodiscard]] std::optional<CameraStillSize> coveringPhotoSize(std::span<const CameraStillSize> listed,
                                                               CameraStillSize minimum, CameraStillSize outputMaximum);

// The largest of the listed sizes by area; nothing for a table with none.
[[nodiscard]] std::optional<CameraStillSize> largestPhotoSize(std::span<const CameraStillSize> listed);

// How a photo output is made ready, in the order the platform requires. The
// output's maximum size must be one of the sizes of its source device's active
// format, which the output knows only once it is attached to the session with
// its connection: AVFoundation raises for the maximum of an output that has
// no source. So the output is attached first, the maximum is set second, and
// an output whose maximum is refused is detached again. The steps are the
// platform's calls; `setMaximum` reports a refusal (an exception) as false.
struct PhotoOutputSteps {
    std::function<bool()> attach;
    std::function<bool(CameraStillSize)> setMaximum;
    std::function<void()> detach;
};
enum class PhotoOutputPreparation : std::uint8_t {
    ready,
    notAttached,    // the session took no output: attach failed, nothing else ran
    maximumRefused  // attached, the maximum was refused, and the output was detached
};
[[nodiscard]] PhotoOutputPreparation preparePhotoOutput(std::optional<CameraStillSize> largest,
                                                        const PhotoOutputSteps& steps);

// A device format as data: what the choice of the format a camera runs in
// needs to know of it. `photoSizes` are the sizes its photo output can make.
struct FrameRateSpan {
    double minimum {};
    double maximum {};
};
struct VideoFormatFacts {
    std::uint32_t width {};
    std::uint32_t height {};
    std::vector<FrameRateSpan> frameRates;
    std::vector<CameraStillSize> photoSizes;
    bool fullRange {};
    bool multiCamera {};  // a session that runs several cameras takes it
};

// The format a stream of `width` by `height` at `frameRate` runs in: one that
// covers the stream and takes its rate. Of those, the ones that list
// `preferredStill` (the largest still the camera offers) come first, so that
// the photos the guest is offered can be made while it streams; among equals
// the smallest, and the full-range one among those of one size. Nothing when
// no format takes the stream. A camera in a multi-camera session takes only
// the formats such a session takes.
[[nodiscard]] std::optional<std::size_t> chooseVideoFormat(std::span<const VideoFormatFacts> formats, std::uint32_t width,
                                                           std::uint32_t height, std::uint32_t frameRate, bool multiCameraOnly,
                                                           CameraStillSize preferredStill);

// A stream a guest may configure: a size and the frame rates it takes.
struct VideoRequest {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t minimumFrameRate {};
    std::uint32_t maximumFrameRate {};
};

// The photo sizes that are true whenever the camera streams: those that every
// format a request runs in lists (at the request's slowest and fastest rate).
// A size that another format lists would be refused whenever the camera runs
// in this one, which is what the sizes offered to a guest must never be.
// Largest first.
[[nodiscard]] std::vector<CameraStillSize> photoSizesOfEveryRun(std::span<const VideoFormatFacts> formats,
                                                                std::span<const VideoRequest> requests,
                                                                CameraStillSize preferredStill);

// The size a video output delivers frames at, for a stream of `width` by
// `height` running in a format of `formatWidth` by `formatHeight`: the
// format's aspect ratio scaled down to just cover the stream, in even
// dimensions, and never above the format's own size. The platform scales the
// frames itself, so a format larger than the stream (one chosen for the photos
// it lists) costs the host no more than the smaller format would.
struct FrameSize {
    std::uint32_t width {};
    std::uint32_t height {};
    friend bool operator==(const FrameSize&, const FrameSize&) = default;
};
[[nodiscard]] FrameSize deliveredFrameSize(std::uint32_t formatWidth, std::uint32_t formatHeight, std::uint32_t width,
                                           std::uint32_t height) noexcept;

// Whether the size the platform resolved for a photo is the size asked for. A
// resolved size of zero by zero is the platform stating none (a Bayer still
// alone has no processed photo), which is not a different size.
[[nodiscard]] bool resolvedSizeIsRequested(CameraStillSize resolved, CameraStillSize requested) noexcept;

} // namespace uml::platform
