// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// How a host's capture formats become what a guest is offered, and what a
// camera session does with them. Every function here is pure: the Apple
// backend (apple/camera_capture.mm) and the tests call the same code, and the
// backend's glue only turns a decision into platform calls.
//
// One format per camera, chosen once. A camera runs in its anchor, the one
// capture format that anchorKeyOf ranks best, whatever it is asked: a stream, a
// still alone, a still while streaming, alone or with other cameras. Everything
// the guest is offered (stills, video sizes, frame rates) is derived from the
// anchor (offerOf), so every offered run starts and every offered still is made.
// A session has no per-run choice and a camera's active format changes only when
// its input joins the session.

#include "camera.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace uml::platform {

using VideoFormatFacts = CameraCaptureFormat;

// ---------------------------------------------------------------------------
// Photo sizes
// ---------------------------------------------------------------------------

// The size to ask the photo output for when a JPEG of `request` is wanted: the
// request itself, when the active format lists it and the output's maximum
// covers it. Nothing otherwise; the nearest larger size is not the request.
[[nodiscard]] std::optional<CameraStillSize> exactPhotoSize(std::span<const CameraStillSize> listed,
                                                            CameraStillSize request, CameraStillSize outputMaximum);

// For a capture that names no JPEG size (a Bayer still alone): the smallest
// listed size that covers `minimum` within the output's maximum.
[[nodiscard]] std::optional<CameraStillSize> coveringPhotoSize(std::span<const CameraStillSize> listed,
                                                               CameraStillSize minimum, CameraStillSize outputMaximum);

// The largest of the listed sizes by area, then by width; nothing for a table
// with none.
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

// The largest photo a format's output is set to make: the largest size the
// format lists that is within the still cap (`maximumStillPixels`, zero for
// none). A format that lists a 48 MP size under a 12 MP cap is set to its 12 MP
// one, so no larger pipeline is prepared.
[[nodiscard]] std::optional<CameraStillSize> photoMaximum(const VideoFormatFacts& format, std::uint64_t maximumStillPixels);

// The sizes a format makes within the cap, largest first: those it lists that
// are within the cap and no larger than photoMaximum in either dimension, the
// test a capture applies (exactPhotoSize against the output's maximum).
[[nodiscard]] std::vector<CameraStillSize> stillsOf(const VideoFormatFacts& format, std::uint64_t maximumStillPixels);

// Whether a format's photo output makes `size`: one of stillsOf.
[[nodiscard]] bool formatMakesPhoto(const VideoFormatFacts& format, CameraStillSize size, std::uint64_t maximumStillPixels);

// Every size some format makes (stillsOf), largest first: what a host can do at
// all, before a camera's anchor decides what it offers.
[[nodiscard]] std::vector<CameraStillSize> stillsMadeByAnyFormat(std::span<const VideoFormatFacts> formats,
                                                                 std::uint64_t maximumStillPixels);

// Whether the size the platform resolved for a photo is the size asked for. A
// resolved size of zero by zero is the platform stating none (a Bayer still
// alone has no processed photo), which is not a different size.
[[nodiscard]] bool resolvedSizeIsRequested(CameraStillSize resolved, CameraStillSize requested) noexcept;

// ---------------------------------------------------------------------------
// Video sizes
// ---------------------------------------------------------------------------

// The size a video output delivers frames at, for a stream of `width` by
// `height` running in a format of `formatWidth` by `formatHeight`: the
// smallest even multiple of the format's reduced aspect ratio that covers the
// stream, or the format's own size when no smaller one does. The ratio is
// kept exactly (the platform refuses a scaled size of another aspect), so a
// stream of another aspect is delivered larger than it asks in one dimension.
// The result grows with the stream in each dimension.
struct FrameSize {
    std::uint32_t width {};
    std::uint32_t height {};
    friend bool operator==(const FrameSize&, const FrameSize&) = default;
};
[[nodiscard]] FrameSize deliveredFrameSize(std::uint32_t formatWidth, std::uint32_t formatHeight, std::uint32_t width,
                                           std::uint32_t height) noexcept;

// The bytes of an NV12 frame of `width` by `height` in a capture buffer whose
// rows are padded to `rowAlignment` bytes (zero or one: not padded): the luma
// rows and the chroma rows, which are half as many, rounded up. The one
// byte count the offer's frame bound and the host's memory reserve share.
[[nodiscard]] constexpr std::uint64_t alignedFrameBytes(std::uint32_t width, std::uint32_t height, std::uint32_t rowAlignment) noexcept
{
    const std::uint64_t alignment = rowAlignment != 0 ? rowAlignment : 1;
    const std::uint64_t pitch = (std::uint64_t(width) + alignment - 1) / alignment * alignment;
    return pitch * (std::uint64_t(height) + (std::uint64_t(height) + 1) / 2);
}

// The bytes of the capture buffer a format delivers for a stream of `width` by
// `height` (deliveredFrameSize), by alignedFrameBytes.
[[nodiscard]] std::uint64_t deliveredFrameBytes(const VideoFormatFacts& format, std::uint32_t width, std::uint32_t height,
                                                std::uint32_t rowAlignment) noexcept;

// What the anchor decides about the outputs of the camera that runs in it: the
// video output's explicit size, which AVFoundation requires to be the aspect
// ratio of the device's active format (it raises otherwise), and the photo
// output's maximum, which must be one of the active format's sizes.
struct OutputSizes {
    // The size the video output is told to deliver; nothing when it delivers
    // the format's own size (or the camera has no stream, `width` of zero).
    std::optional<FrameSize> frame;
    // photoMaximum; nothing when the format lists none within the cap, which
    // leaves the output's own maximum.
    std::optional<CameraStillSize> photoMaximum;
};
[[nodiscard]] OutputSizes outputSizesFor(const VideoFormatFacts& format, std::uint32_t width, std::uint32_t height,
                                         std::uint64_t maximumStillPixels);

// ---------------------------------------------------------------------------
// Frame rates
// ---------------------------------------------------------------------------

// The fastest frame rate a guest is offered: photos win over fast streams, so a
// rate above it is neither offered nor asked of the device.
inline constexpr std::uint32_t videoRateCap = 30;

// The whole rates of a span. A span's end within 0.15 % of a whole number counts
// as that number (29.97 is 30, 30.00003 is 30, 23.976 is 24); otherwise the
// rates run from the ceiling of the minimum to the floor of the maximum. Nothing
// when the span holds no whole rate.
struct WholeRates {
    std::uint32_t minimum {};
    std::uint32_t maximum {};
    friend bool operator==(const WholeRates&, const WholeRates&) = default;
};
[[nodiscard]] std::optional<WholeRates> wholeRatesOf(const CameraRateSpan& span) noexcept;

// A format's rates as offered: its whole rates up to `rateCap`, ascending and
// merged. A format with whole rates only above the cap offers its slowest alone;
// a format with none offers nothing.
[[nodiscard]] std::vector<WholeRates> offeredRatesOf(const VideoFormatFacts& format, std::uint32_t rateCap);

// How a whole rate is programmed on a span of a format (the index is the span's
// in `frameRates`, which is the platform's own order). A rate inside the span
// sets the reciprocal duration. A rate a hair above the span's maximum (30
// against 29.97) sets the span's own shortest duration, and a hair below its
// minimum (30 against 30.00003) the span's own longest, which the platform
// accepts where the nominal reciprocal may fall outside the span.
enum class FrameDuration : std::uint8_t { reciprocal, spanShortest, spanLongest };
struct RateSetting {
    std::size_t span {};
    FrameDuration duration {};
    friend bool operator==(const RateSetting&, const RateSetting&) = default;
};
[[nodiscard]] std::optional<RateSetting> rateSettingFor(const VideoFormatFacts& format, std::uint32_t rate) noexcept;

// The duration, in seconds, that `setting` programs for `rate` on its span.
[[nodiscard]] double programmedFrameDuration(const CameraRateSpan& span, FrameDuration duration, std::uint32_t rate) noexcept;

// The rate a camera runs at while the system's pressure caps it (`pressureCap`,
// zero for none): the largest offered rate that is not above the lower of the
// request and the cap, else the slowest offered. Zero when nothing is offered.
[[nodiscard]] std::uint32_t throttledRate(std::span<const WholeRates> offered, std::uint32_t requested,
                                          std::uint32_t pressureCap) noexcept;

// A rate as programmed on a format: the whole rate and how the span sets it.
struct ProgrammedRate {
    std::uint32_t rate {};
    RateSetting setting;
    friend bool operator==(const ProgrammedRate&, const ProgrammedRate&) = default;
};

// What a stream asked for at `requested` frames a second runs at under the
// pressure cap (throttledRate, from the format's offered rates) and how that
// rate is programmed (rateSettingFor); nothing when no offered rate can be.
[[nodiscard]] std::optional<ProgrammedRate> programmedRate(const VideoFormatFacts& format, std::span<const WholeRates> offered,
                                                           std::uint32_t requested, std::uint32_t pressureCap);

// What the system's pressure leaves of a running stream's rate. The pressure
// observer (any thread) and the session's queue share one, and both hold it by
// shared ownership: it owns the facts the rate is programmed from, so a
// notification that comes after its session has gone finds them alive, and
// `end()` makes whatever is still queued program nothing.
class PressureWatch {
public:
    PressureWatch(VideoFormatFacts format, std::vector<WholeRates> offered);

    // The rate the guest asked for.
    void request(std::uint32_t rate) noexcept;
    [[nodiscard]] std::uint32_t requested() const noexcept;
    // The most frames a second the system's pressure allows; zero is no limit.
    [[nodiscard]] std::uint32_t cap() const noexcept;
    // The system now allows `cap`: whether that changed the cap of a live watch,
    // which means the rate is to be programmed again.
    [[nodiscard]] bool pressure(std::uint32_t cap) noexcept;
    // The rate to program now; nothing once the watch has ended.
    [[nodiscard]] std::optional<ProgrammedRate> programmed() const;
    [[nodiscard]] bool live() const noexcept;
    // The stream is over or its session is: nothing is programmed afterwards.
    void end() noexcept;

private:
    const VideoFormatFacts format_;
    const std::vector<WholeRates> offered_;
    std::atomic<std::uint32_t> requested_ {0};
    std::atomic<std::uint32_t> cap_ {0};
    std::atomic<bool> live_ {true};
};

// ---------------------------------------------------------------------------
// The anchor and the offer
// ---------------------------------------------------------------------------

// What a guest of a tier may ask, and the sizes video is cut from.
struct OfferRules {
    // The still cap and the largest frame the host may deliver for a stream.
    CameraLimits limits;
    // The tier's video size.
    std::uint32_t maximumVideoWidth {};
    std::uint32_t maximumVideoHeight {};
    std::uint32_t rateCap {videoRateCap};
    // The standard sizes and the native sizes of the camera's formats; sizes the
    // offer cannot use (odd, below 16, above the tier) are dropped by offerOf.
    std::vector<FrameSize> candidateSizes;
};

// What a format offers a guest.
struct CaptureOffer {
    std::vector<CameraStillSize> stills;  // largest first: stillsOf
    std::vector<FrameSize> videoSizes;    // largest first; never empty for a candidate
    std::vector<WholeRates> rates;        // ascending; never empty for a candidate
    // The largest request a stream may make of the format: the width of the
    // widest and the height of the tallest of videoSizes. The frame the format
    // delivers for the box is within the limits' frame bound, and so is the frame
    // for any request inside it (deliveredFrameSize grows with the request).
    FrameSize box;
};
[[nodiscard]] CaptureOffer offerOf(const VideoFormatFacts& format, const OfferRules& rules);

// How a format ranks as an anchor. The fields compare in this order, larger
// first; each is true to its name:
//  1. rate: the format has a whole rate within the cap, then the highest;
//  2. still: the area of the largest still it makes, then its width;
//  3. video: the area of the box of its video sizes;
//  4. cost: the smaller frame (the format's own width by height);
//  5. low rates: the lower minimum whole rate;
//  6. a full-range format.
struct AnchorKey {
    bool hasCappedRate {};
    std::uint32_t rate {};
    std::uint64_t stillArea {};
    std::uint32_t stillWidth {};
    std::uint64_t boxArea {};
    std::int64_t negativeCost {};
    std::int64_t negativeMinimumRate {};
    bool fullRange {};

    friend bool operator==(const AnchorKey&, const AnchorKey&) = default;
    friend auto operator<=>(const AnchorKey&, const AnchorKey&) = default;
    // Whether two formats offer the guest as much: equal on the first three.
    [[nodiscard]] bool offersAsMuchAs(const AnchorKey& other) const noexcept
    {
        return hasCappedRate == other.hasCappedRate && rate == other.rate && stillArea == other.stillArea &&
            stillWidth == other.stillWidth && boxArea == other.boxArea;
    }
};

// The key of a format; nothing for a format that cannot be an anchor: one with
// no whole rate or no video size in its box.
[[nodiscard]] std::optional<AnchorKey> anchorKeyOf(const VideoFormatFacts& format, const OfferRules& rules);

// The anchor of a camera: the index of the best-ranked candidate (the lowest
// index among equals); among the formats a multi-camera session takes when
// `multiCameraOnly`. Nothing when there is no candidate.
[[nodiscard]] std::optional<std::size_t> chooseAnchor(std::span<const VideoFormatFacts> formats, const OfferRules& rules,
                                                      bool multiCameraOnly);

// Whether a camera that could run with others keeps that: the best key among
// its multi-camera formats offers as much as the best key among all its
// formats. A camera with no multi-camera candidate does not.
[[nodiscard]] bool keepsConcurrency(std::span<const VideoFormatFacts> formats, const OfferRules& rules);

// ---------------------------------------------------------------------------
// The session's transactions
// ---------------------------------------------------------------------------

// Whether a camera may join a session that holds other cameras' inputs: a
// multi-camera session takes only the formats it lists, for every input, so
// both this camera's anchor and every other input's must be one of them.
[[nodiscard]] bool mayJoin(bool otherInputsInSession, bool ownAnchorMultiCamera,
                           std::span<const bool> otherAnchorsMultiCamera) noexcept;

// What an input, its outputs and its format need for a camera to run a stream,
// a still or both, and the platform's calls that do it. The platform's calls
// report a refusal (an exception included) as false. Anything a call added when
// it refuses is removed by its `remove` step, which does nothing for what was
// never added. The device's frame durations, which `activate` changes, are
// taken before it runs (`snapshotDurations`, the device's own values, not what
// a rate would program: a long manual exposure lengthens the maximum) and put
// back by `restoreDurations` when the call refuses.
struct AdmitPlan {
    bool addInput {};  // the camera has no input in the session
    bool activate {};  // the anchor and the rate are set on the device
    bool addVideo {};
    bool addPhoto {};
};
struct AdmitSteps {
    std::function<void()> begin;   // beginConfiguration
    std::function<void()> commit;  // commitConfiguration
    std::function<bool()> addInput;
    std::function<void()> snapshotDurations;  // empty: nothing to take
    std::function<bool()> activate;
    std::function<bool()> addVideo;
    // Attaches the photo output and sets its maximum (preparePhotoOutput).
    std::function<bool()> addPhoto;
    // Whether the session is over its hardware budget with what was added.
    std::function<bool()> overBudget;
    std::function<void()> removeVideo;
    std::function<void()> removePhoto;
    std::function<void()> removeInput;
    std::function<void()> restoreDurations;  // empty: nothing to put back
};
enum class Admission : std::uint8_t { admitted, inputRefused, formatRefused, videoRefused, photoRefused, overBudget };

// Runs the plan inside one configuration. A refusal, or a session over budget
// once the configuration is committed, removes exactly what this call added and
// puts the device's frame durations back as they were before `activate`, and
// nothing else changes: the format and outputs of a camera that was already in
// the session stay as they were.
[[nodiscard]] Admission admit(const AdmitPlan& plan, const AdmitSteps& steps);

// A running stream changes size or rate: the durations first, then the video
// output's size; a refusal puts the durations back as `snapshotDurations` took
// them before `setDurations` ran (the device's own values, as for AdmitSteps).
struct ResizeSteps {
    std::function<void()> begin;
    std::function<void()> commit;
    std::function<void()> snapshotDurations;  // empty: nothing to take
    std::function<bool()> setDurations;       // empty: the rate does not change
    std::function<bool()> setVideoSize;       // empty: the size does not change
    std::function<void()> restoreDurations;
};
enum class Resizing : std::uint8_t { resized, durationsRefused, sizeRefused };
[[nodiscard]] Resizing resizeStream(const ResizeSteps& steps);

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

enum class Refusal : std::uint8_t {
    noAnchor,
    formatChanged,
    notJoinable,
    inputRefused,
    outputRefused,
    overBudget,
    stillNotMade,
    rawUnavailable,
    raised,
};
[[nodiscard]] std::string_view refusalName(Refusal reason) noexcept;

// One line per camera and reason until the camera's configuration changes: a
// guest that asks again and again must not fill the log.
class RefusalLog {
public:
    // True the first time a camera is refused for a reason since the camera was
    // last reset.
    [[nodiscard]] bool first(std::size_t camera, Refusal reason);
    void reset(std::size_t camera);

private:
    std::map<std::size_t, std::uint32_t> seen_;
};

} // namespace uml::platform
