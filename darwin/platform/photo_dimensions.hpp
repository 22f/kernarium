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

using FrameRateSpan = CameraRateSpan;
using VideoFormatFacts = CameraCaptureFormat;

// Whether a format's photo output makes `size`: the format lists it and the
// output's maximum, which is set to the largest size the format lists, covers
// it. This is the test a capture applies (exactPhotoSize).
[[nodiscard]] bool formatMakesPhoto(const VideoFormatFacts& format, CameraStillSize size);

// Every photo size within the guest's cap that some format makes, largest
// first.
[[nodiscard]] std::vector<CameraStillSize> photoSizesMade(std::span<const VideoFormatFacts> formats, const CameraLimits& limits);

// The largest photo within the guest's cap that some format makes: the size a
// stream's format is chosen to make. Nothing when no format makes one.
[[nodiscard]] std::optional<CameraStillSize> preferredPhotoSize(std::span<const VideoFormatFacts> formats,
                                                                const CameraLimits& limits);

// The format a stream of `width` by `height` at `frameRate` runs in: one that
// covers the stream and takes its rate. Of those, the ones that make the
// preferred photo come first, so that the photos the guest is offered can be
// made while it streams; among equals the smallest, and the full-range one
// among those of one size. Nothing when no format takes the stream. A camera
// in a multi-camera session takes only the formats such a session takes.
[[nodiscard]] std::optional<std::size_t> chooseVideoFormat(std::span<const VideoFormatFacts> formats, std::uint32_t width,
                                                           std::uint32_t height, std::uint32_t frameRate, bool multiCameraOnly,
                                                           const CameraLimits& limits);

// The format a camera that only takes stills runs in: the smallest that makes
// `wanted`, which is the size the guest asked for. With none wanted (a Bayer
// still alone), the one that makes the largest photo within the guest's cap.
// The size selects the format, so a size another format makes is not refused.
[[nodiscard]] std::optional<std::size_t> chooseStillFormat(std::span<const VideoFormatFacts> formats,
                                                           std::optional<CameraStillSize> wanted, bool multiCameraOnly,
                                                           const CameraLimits& limits);

// A stream a guest may configure: a size and the frame rates it takes.
struct VideoRequest {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t minimumFrameRate {};
    std::uint32_t maximumFrameRate {};
};

// The photo sizes a guest is offered, and the rate they hold up to.
struct StillOffer {
    // Largest first. Every size is made by the format every permitted run
    // chooses, and by a still taken with no stream running.
    std::vector<CameraStillSize> sizes;
    // The fastest frame rate (a whole number) of the streams the offer holds
    // for; a stream above it is not permitted. The highest rate of the
    // requests when none is held back.
    std::uint32_t maximumRate {};
};

// The photo sizes that are true for every run the guest can make. A run is a
// stream of one of `requests` at any whole rate in its range, and also its
// choice among the formats a multi-camera session takes when the camera may
// run with others (`concurrent`). A size is offered only when the format of
// every such run makes it, and when a still taken with no stream running can
// make it too (a format of a multi-camera session, in a concurrent camera). A
// size that only another format makes is therefore not offered: nothing is
// refused that was offered.
//
// Photos win over fast streams: the offer is made over the runs up to
// `holdsUntil` frames a second (or the fastest request, when that is less),
// and the largest size it contains then sets how fast the streams may run: the
// highest rate at which every run still makes it. The offer is then taken
// over the runs up to that rate, and `maximumRate` is that rate. A camera
// with no size that survives the runs up to `holdsUntil` offers none and
// restricts nothing.
[[nodiscard]] StillOffer offerStills(std::span<const VideoFormatFacts> formats, std::span<const VideoRequest> requests,
                                     bool concurrent, const CameraLimits& limits, std::uint32_t holdsUntil);

// The size a video output delivers frames at, for a stream of `width` by
// `height` running in a format of `formatWidth` by `formatHeight`: the
// smallest even multiple of the format's reduced aspect ratio that covers the
// stream, or the format's own size when no smaller one does. The ratio is
// kept exactly (the platform refuses a scaled size of another aspect), so a
// stream of another aspect is delivered larger than it asks in one dimension.
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
