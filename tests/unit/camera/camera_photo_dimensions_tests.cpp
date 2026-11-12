// SPDX-License-Identifier: GPL-2.0-or-later
// Which photo size a camera is asked for, and whether the photo is that size,
// on tables of the shape the platform lists per format.
#include "photo_dimensions.hpp"
#include "require.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace uml::platform;
using uml::testing::require;

namespace {

using Sizes = std::vector<CameraStillSize>;

// A back camera's active format as the photo output lists it: 12 MP, 24 MP and
// 48 MP, and a binned 3 MP.
const Sizes backFormat {{4032, 3024}, {5712, 4284}, {8064, 6048}, {2016, 1512}};
// A high-frame-rate format of the same camera, which lists no 48 MP.
const Sizes fastFormat {{4032, 3024}, {2016, 1512}};

void anExactSizeTheFormatListsIsAsked()
{
    const CameraStillSize maximum {8064, 6048};
    for (const auto& size : backFormat) {
        const auto chosen = exactPhotoSize(backFormat, size, maximum);
        require(chosen && *chosen == size, "every listed size is asked for as it is");
    }
}

void aSizeAnotherFormatListsIsNotAsked()
{
    // The union of the two formats is what discovery advertises; the fast
    // format cannot make the 48 MP the union has.
    const CameraStillSize maximum {4032, 3024};
    require(!exactPhotoSize(fastFormat, {8064, 6048}, {8064, 6048}).has_value(), "the active format lacks the size");
    require(!exactPhotoSize(fastFormat, {5712, 4284}, {8064, 6048}), "nor the 24 MP");
    require(exactPhotoSize(fastFormat, {4032, 3024}, maximum).has_value(), "but it makes the 12 MP");
}

void aLargerSizeThatCoversIsNotTheRequest()
{
    const CameraStillSize maximum {8064, 6048};
    require(!exactPhotoSize(backFormat, {4000, 3000}, maximum), "4000 by 3000 is not 4032 by 3024");
    require(!exactPhotoSize(backFormat, {4032, 3000}, maximum), "one side that differs");
    require(!exactPhotoSize(backFormat, {3024, 4032}, maximum), "a portrait picture is not the listed landscape one");
    require(!exactPhotoSize(backFormat, {1, 1}, maximum), "nor does a small one fit a larger size");
}

void theOutputsMaximumBoundsTheRequest()
{
    require(!exactPhotoSize(backFormat, {8064, 6048}, {4032, 3024}), "an output that makes up to 12 MP cannot make 48 MP");
    require(exactPhotoSize(backFormat, {4032, 3024}, {4032, 3024}).has_value(), "and makes its own maximum");
    require(!exactPhotoSize(backFormat, {4032, 3024}, {}), "an output that states no maximum makes nothing");
}

void anEmptyOrZeroSizedTableAsksForNothing()
{
    require(!exactPhotoSize({}, {4032, 3024}, {8064, 6048}), "no sizes listed");
    require(!exactPhotoSize(backFormat, {}, {8064, 6048}), "a request of no size");
    const Sizes zeroes {{0, 0}, {4032, 0}, {0, 3024}};
    require(!exactPhotoSize(zeroes, {0, 0}, {8064, 6048}) && !exactPhotoSize(zeroes, {4032, 0}, {8064, 6048}),
            "a listed size of zero is no size");
    require(!coveringPhotoSize(zeroes, {}, {8064, 6048}), "and covers nothing");
}

void aCaptureWithoutAJpegTakesTheSmallestSize()
{
    const auto smallest = coveringPhotoSize(backFormat, {}, {8064, 6048});
    require(smallest && *smallest == CameraStillSize(2016, 1512), "the smallest listed size, as the output defaults to");
    const auto covering = coveringPhotoSize(backFormat, {3000, 2000}, {8064, 6048});
    require(covering && *covering == CameraStillSize(4032, 3024), "the smallest that covers a minimum");
    require(!coveringPhotoSize(backFormat, {9000, 7000}, {8064, 6048}), "none covers a larger minimum");
    require(!coveringPhotoSize(backFormat, {5000, 4000}, {4032, 3024}), "and the output's maximum bounds the choice");
    require(!coveringPhotoSize({}, {}, {8064, 6048}), "an empty table");
}

void theResolvedSizeIsTheRequestOrItIsRefused()
{
    require(resolvedSizeIsRequested({4032, 3024}, {4032, 3024}), "the size asked for");
    require(!resolvedSizeIsRequested({2016, 1512}, {4032, 3024}), "a smaller photo than the one configured");
    require(!resolvedSizeIsRequested({8064, 6048}, {4032, 3024}), "a larger one");
    require(!resolvedSizeIsRequested({3024, 4032}, {4032, 3024}), "a turned one");
    require(resolvedSizeIsRequested({0, 0}, {4032, 3024}), "the platform stated no size (a Bayer still alone)");
    require(!resolvedSizeIsRequested({4032, 0}, {4032, 3024}), "half a size is a size that differs");
}

// A photo output as AVFoundation treats it: its maximum is refused (it raises)
// unless the output is attached to the session with a source whose active
// format lists the size.
struct FakeOutput {
    explicit FakeOutput(Sizes format) : sourceFormat(std::move(format)) {}
    Sizes sourceFormat;
    bool attachable {true};
    bool attached {false};
    std::vector<std::string> calls;
    std::optional<CameraStillSize> maximum;

    PhotoOutputSteps steps()
    {
        return {
            [this] {
                calls.push_back("attach");
                attached = attachable;
                return attached;
            },
            [this](CameraStillSize size) {
                calls.push_back("set " + std::to_string(size.width) + "x" + std::to_string(size.height));
                if (!attached) return false;  // no source device: the setter raises
                for (const auto& listed : sourceFormat)
                    if (listed == size) {
                        maximum = size;
                        return true;
                    }
                return false;
            },
            [this] {
                calls.push_back("detach");
                attached = false;
            },
        };
    }
};

void theOutputIsAttachedBeforeItsMaximumIsSet()
{
    FakeOutput output(backFormat);
    const auto largest = largestPhotoSize(backFormat);
    require(largest && *largest == CameraStillSize(8064, 6048), "the largest by area");
    require(preparePhotoOutput(largest, output.steps()) == PhotoOutputPreparation::ready, "an attached output takes its maximum");
    require(output.calls == std::vector<std::string> {"attach", "set 8064x6048"}, "attach, then set the maximum");
    require(output.attached && output.maximum && *output.maximum == CameraStillSize(8064, 6048), "and keeps it");
}

void aMaximumTheSourceRefusesDetachesTheOutput()
{
    FakeOutput output(fastFormat);
    // A maximum the source's active format does not list: the setter raises.
    require(preparePhotoOutput(CameraStillSize {8064, 6048}, output.steps()) == PhotoOutputPreparation::maximumRefused,
            "the refused maximum is reported");
    require(output.calls == std::vector<std::string> {"attach", "set 8064x6048", "detach"}, "attach, set, then detach");
    require(!output.attached && !output.maximum, "the output is not left on the session");
}

void anOutputThatCannotBeAttachedGetsNoMaximum()
{
    FakeOutput output(backFormat);
    output.attachable = false;
    require(preparePhotoOutput(CameraStillSize {4032, 3024}, output.steps()) == PhotoOutputPreparation::notAttached,
            "the session took no output");
    require(output.calls == std::vector<std::string> {"attach"}, "nothing is set on it and nothing is detached");
}

void aFormatThatListsNothingLeavesTheDefault()
{
    FakeOutput output((Sizes()));
    require(!largestPhotoSize(output.sourceFormat) && !largestPhotoSize(Sizes {{0, 0}, {4032, 0}}), "no usable size");
    require(preparePhotoOutput(std::nullopt, output.steps()) == PhotoOutputPreparation::ready, "the output stands as it is");
    require(output.calls == std::vector<std::string> {"attach"}, "attached, with no maximum set");
}

// The order the platform requires, stated against a fake that enforces it: the
// same steps in the wrong order are refused.
void aMaximumSetBeforeAttachIsRefused()
{
    FakeOutput output(backFormat);
    const auto steps = output.steps();
    require(!steps.setMaximum({4032, 3024}), "an unattached output refuses its maximum");
    require(steps.attach() && steps.setMaximum({4032, 3024}), "and takes it once attached");
}

// An iPad mini's back camera as the platform lists it: the binned format the
// smallest covering choice picks for a 1440x1080 stream lists photos up to
// 2016x1512 only; the 12 MP photo sizes belong to the larger formats.
VideoFormatFacts format(std::uint32_t width, std::uint32_t height, Sizes photos, double fastest = 30, bool full = true,
                        bool multi = false)
{
    VideoFormatFacts facts;
    facts.width = width;
    facts.height = height;
    facts.frameRates = {{1, fastest}};
    facts.photoSizes = std::move(photos);
    facts.fullRange = full;
    facts.multiCamera = multi;
    return facts;
}
const std::vector<VideoFormatFacts> tabletFormats {
    format(1440, 1080, {{1440, 1080}, {2016, 1512}}),
    format(1920, 1440, {{1920, 1440}, {4032, 3024}}),
    format(4032, 3024, {{4032, 3024}, {4224, 2376}}, 30),
    format(1920, 1080, {{1920, 1080}, {2016, 1134}}, 60),
};
const CameraStillSize twelveMegapixels {4032, 3024};
// The guest's cap: 12 MP stills (the small tier's 12.7 million pixels), and no
// cap at all.
const CameraLimits capped {12'700'000};
const CameraLimits uncapped {};

void theStreamRunsInTheSmallestFormatThatMakesTheLargestStill()
{
    const auto chosen = chooseVideoFormat(tabletFormats, 1440, 1080, 30, false, capped);
    require(chosen && *chosen == 1, "1920x1440 is the smallest covering format that makes 12 MP");
    const std::vector<VideoFormatFacts> equal {format(1920, 1440, {{1440, 1080}}), format(1440, 1080, {{1440, 1080}})};
    const auto first = chooseVideoFormat(equal, 1440, 1080, 30, false, capped);
    require(first && *first == 1, "formats that make the same photos: the smallest covering format");
}

void aFormatThatCannotTakeTheRateOrTheSizeIsNotChosen()
{
    // 60 fps: only the 1080p format takes it, and it lists no 12 MP; the
    // preference does not reach past a format that cannot carry the stream.
    const auto fast = chooseVideoFormat(tabletFormats, 1280, 720, 60, false, capped);
    require(fast && *fast == 3, "the only format that takes 60 fps is chosen without the large still");
    require(!chooseVideoFormat(tabletFormats, 4096, 3072, 30, false, capped), "nothing covers 4096x3072");
    require(!chooseVideoFormat(tabletFormats, 640, 480, 120, false, capped), "nothing takes 120 fps");
}

void aMultiCameraSessionChoosesAmongItsFormatsOnly()
{
    auto formats = tabletFormats;
    formats[1].multiCamera = true;  // only the 1920x1440 format joins
    const auto chosen = chooseVideoFormat(formats, 1440, 1080, 30, true, capped);
    require(chosen && *chosen == 1, "the multi-camera format is chosen");
    formats[1].multiCamera = false;
    require(!chooseVideoFormat(formats, 1440, 1080, 30, true, capped), "none joins: nothing is chosen");
}

void theFullRangeFormatWinsAmongEqualOnes()
{
    std::vector<VideoFormatFacts> formats {format(1920, 1440, {{4032, 3024}}, 30, false), format(1920, 1440, {{4032, 3024}}, 30, true)};
    const auto chosen = chooseVideoFormat(formats, 1920, 1440, 30, false, capped);
    require(chosen && *chosen == 1, "the full-range format of the same size and sizes");
}

// The guest never asks for more than its cap, so a format is not chosen for a
// size above it: the cheaper format that makes the largest permitted still wins
// over the larger one that makes more.
void aFormatThatWouldDeliverMoreThanTheBoundIsNotChosen()
{
    // A 1:1 format that makes 12 MP and a 16:9 one that makes none: a 1080p
    // stream from the square one is delivered 1920x1920, more than the
    // reserve's frame (the 4:3 frame of the tier's width, 1920x1440).
    const std::vector<VideoFormatFacts> formats {
        format(1920, 1920, {{4032, 3024}}),
        format(1920, 1080, {{1920, 1080}}),
    };
    const CameraLimits bounded {12'700'000, 1920ull * 1440 * 3 / 2};
    const auto unbounded = chooseVideoFormat(formats, 1920, 1080, 30, false, capped);
    require(unbounded && *unbounded == 0, "with no bound the square format that makes 12 MP is chosen");
    const auto chosen = chooseVideoFormat(formats, 1920, 1080, 30, false, bounded);
    require(chosen && *chosen == 1, "the bound chooses the format whose frame fits");
    require(deliveredFrameBytes(formats[0], 1920, 1080) == 1920ull * 1920 * 3 / 2, "1920x1920 NV12");
    require(!chooseVideoFormat({formats.data(), 1}, 1920, 1080, 30, false, bounded), "and where none fits nothing runs");
    // The frame a 4:3 format delivers to a 1080p stream is the bound itself.
    require(deliveredFrameBytes(tabletFormats[1], 1920, 1080) == 1920ull * 1440 * 3 / 2, "4:3 at 1080p is 4147200 bytes");
    require(deliveredFrameBytes(format(1920, 1080, {}), 640, 480) == 864ull * 486 * 3 / 2, "scaled, 864x486");
    require(chooseVideoFormat(tabletFormats, 1920, 1080, 30, false, bounded) == std::optional<std::size_t>(1),
            "the 4:3 format the iPad mini streams 1080p from fits the bound");
}

void theCapDecidesWhichStillAFormatIsChosenFor()
{
    const std::vector<VideoFormatFacts> formats {
        format(1920, 1440, {{1920, 1440}, {4032, 3024}}),
        format(4032, 3024, {{4032, 3024}, {8064, 6048}}),
    };
    require(preferredPhotoSize(formats, capped) == std::optional<CameraStillSize>(CameraStillSize {4032, 3024}),
            "the largest permitted still is 12 MP");
    require(preferredPhotoSize(formats, uncapped) == std::optional<CameraStillSize>(CameraStillSize {8064, 6048}),
            "with no cap it is the largest listed anywhere");
    const auto chosen = chooseVideoFormat(formats, 1440, 1080, 30, false, capped);
    require(chosen && *chosen == 0, "the smaller format that makes 12 MP");
    const auto unbounded = chooseVideoFormat(formats, 1440, 1080, 30, false, uncapped);
    require(unbounded && *unbounded == 1, "where the larger one is chosen for 48 MP when no cap says otherwise");
    require(!preferredPhotoSize(formats, {100}), "a cap below every size leaves no preferred still");
}

// The output's maximum is the largest size by area, and the output takes no
// size above it in either dimension: a listed size wider than that maximum
// (4224 against 4032) is not one the format makes.
void aSizeWiderThanTheOutputMaximumIsNotMade()
{
    const VideoFormatFacts wide = format(4032, 3024, {{4032, 3024}, {4224, 2376}});
    require(formatMakesPhoto(wide, {4032, 3024}) && !formatMakesPhoto(wide, {4224, 2376}), "12.19 MP is the maximum, 4224 is wider");
    require(!formatMakesPhoto(wide, {2016, 1512}) && !formatMakesPhoto(format(1, 1, {}), {4032, 3024}), "a size the format does not list");
    const std::vector<VideoFormatFacts> formats {wide};
    require(photoSizesMade(formats, uncapped) == Sizes({{4032, 3024}}), "so it is not among the sizes the host makes");
}

// The streams the guest may run on the iPad mini's back camera, as the
// platform reports its formats: preview sizes at up to 30 fps and 1080p video
// up to 60.
const std::vector<VideoRequest> tabletStreams {{1920, 1080, 1, 60}, {1440, 1080, 1, 30}, {640, 480, 1, 30}, {176, 144, 1, 30}};

void theOfferIsWhatEveryRunMakes()
{
    // Runs up to 30 fps choose the 1920x1440 format ({1920x1440, 4032x3024});
    // 1080p at 31 to 60 chooses the 1080p format ({1920x1080, 2016x1134}).
    const auto offer = offerStills(tabletFormats, tabletStreams, false, capped, 30);
    require(offer.sizes == Sizes({{4032, 3024}, {1920, 1440}}), "the sizes of the format the streams run in, largest first");
    require(offer.maximumRate == 30, "streams above 30 fps are not permitted: the 1080p format makes no 12 MP");
}

// A size that the format of a run lacks is not offered. The unit test of the
// first review: 4224x2376 is made by the 12 MP video format only, and a
// 1440x1080 stream runs in 1920x1440; discovery used to fall back to it.
void aSizeOnlyAnotherFormatMakesIsNotOffered()
{
    const std::vector<VideoRequest> streams {{1440, 1080, 1, 30}, {1920, 1440, 1, 30}};
    const auto offer = offerStills(tabletFormats, streams, false, capped, 30);
    require(std::find(offer.sizes.begin(), offer.sizes.end(), CameraStillSize {4224, 2376}) == offer.sizes.end(),
            "the 16:9 size of the 12 MP video format");
    require(!offer.sizes.empty() && offer.sizes.front() == twelveMegapixels, "12 MP, which every run makes");
    // The same table with the 1080p60 mode: no size is common to every run, as
    // the rate ceiling is what keeps the photos, nothing is offered unconditionally.
    const auto fast = offerStills(tabletFormats, tabletStreams, false, capped, 60);
    require(fast.sizes.empty(), "with 60 fps held too, no size survives every run");
    require(fast.maximumRate == 60, "and nothing is restricted for a camera that offers none");
}

void aRateInsideTheRangeDecidesWhichFormatARunUses()
{
    // 1080p at 1 to 60 fps, and a 4K format that makes 12 MP at 1 to 10 and 50
    // to 60 fps only. The ends of the range choose the 4K format; 30 fps, in
    // the middle, runs in the 1080p one, which lists no 12 MP.
    const std::vector<VideoFormatFacts> formats {
        format(1920, 1080, {{1920, 1080}}, 60),
        VideoFormatFacts {3840, 2160, {{1, 10}, {50, 60}}, {{4032, 3024}}, true, false},
    };
    const std::vector<VideoRequest> streams {{1920, 1080, 1, 60}};
    const auto offer = offerStills(formats, streams, false, capped, 60);
    require(offer.sizes.empty(), "12 MP is refused at 30 fps, so it is not offered, whatever the ends of the range do");
    require(chooseVideoFormat(formats, 1920, 1080, 30, false, capped) == std::optional<std::size_t>(0), "the middle runs in the 1080p format");
    require(chooseVideoFormat(formats, 1920, 1080, 5, false, capped) == std::optional<std::size_t>(1), "the low end in the 4K one");
    require(chooseVideoFormat(formats, 1920, 1080, 55, false, capped) == std::optional<std::size_t>(1), "the high end in the 4K one");
}

void aCameraKeepsItsPhotosOverTheFastStreamsThatCannotMakeThem()
{
    // The 1080p60 format lists no 12 MP: 12 MP wins and the streams stop at 30.
    const auto offer = offerStills(tabletFormats, tabletStreams, false, capped, 30);
    require(!offer.sizes.empty() && offer.sizes.front() == twelveMegapixels, "12 MP is offered");
    require(offer.maximumRate == 30, "up to 30 fps");
    // The streams after the ceiling are what the offer holds for: the same
    // camera with its fast mode removed offers the same sizes at the same rate.
    const std::vector<VideoRequest> clipped {{1920, 1080, 1, 30}, {1440, 1080, 1, 30}, {640, 480, 1, 30}, {176, 144, 1, 30}};
    const auto again = offerStills(tabletFormats, clipped, false, capped, 30);
    require(again.sizes == offer.sizes && again.maximumRate == 30, "the offer holds for what remains");
}

void aFormatThatMakesTheLargestStillAtAHigherRateRaisesTheCeiling()
{
    std::vector<VideoFormatFacts> formats = tabletFormats;
    formats[1].frameRates = {{1, 60}};  // the 4:3 format reaches 60 fps too
    const auto offer = offerStills(formats, tabletStreams, false, capped, 30);
    require(!offer.sizes.empty() && offer.sizes.front() == twelveMegapixels, "12 MP");
    require(offer.maximumRate == 60, "held up to 60 fps, where the 4:3 format carries it");
}

void aConcurrentCameraOffersWhatAMultiCameraFormatMakes()
{
    std::vector<VideoFormatFacts> formats = tabletFormats;
    formats[1].multiCamera = false;
    formats[0].multiCamera = true;  // the binned format is the only one a multi-camera session takes
    const std::vector<VideoRequest> streams {{1440, 1080, 1, 30}};
    const auto alone = offerStills(formats, streams, false, capped, 30);
    require(!alone.sizes.empty() && alone.sizes.front() == twelveMegapixels, "alone it runs in the 12 MP format");
    const auto together = offerStills(formats, streams, true, capped, 30);
    // Running with others the stream is in 1440x1080, which makes nothing the
    // format it runs in alone makes.
    require(together.sizes == Sizes({}), "no size is made by both formats the stream runs in");
    formats[1].multiCamera = true;
    const auto both = offerStills(formats, streams, true, capped, 30);
    require(both.sizes == Sizes({{4032, 3024}, {1920, 1440}}), "once the 12 MP format joins too, alone and together alike");
}

void aStillWithoutAStreamSelectsTheFormatThatMakesItsSize()
{
    const auto widest = chooseStillFormat(tabletFormats, CameraStillSize {4032, 3024}, false, capped);
    require(widest && *widest == 1, "12 MP: the smallest format that makes it");
    const auto binned = chooseStillFormat(tabletFormats, CameraStillSize {1440, 1080}, false, capped);
    require(binned && *binned == 0, "1440x1080: the smallest of all");
    require(!chooseStillFormat(tabletFormats, CameraStillSize {5712, 4284}, false, capped), "a size no format makes is refused");
    const auto bayer = chooseStillFormat(tabletFormats, std::nullopt, false, capped);
    require(bayer && *bayer == 1, "with no JPEG asked, the format that makes the largest permitted photo");
    std::vector<VideoFormatFacts> formats = tabletFormats;
    formats[1].multiCamera = false;
    formats[2].multiCamera = true;
    require(chooseStillFormat(formats, CameraStillSize {4032, 3024}, true, capped) == std::optional<std::size_t>(2),
            "among the formats a multi-camera session takes: the one that does");
    formats[2].multiCamera = false;
    require(!chooseStillFormat(formats, CameraStillSize {4032, 3024}, true, capped), "none does: refused");
}

void aCameraWithoutStreamsOffersNothing()
{
    require(offerStills(tabletFormats, {}, false, capped, 30).sizes.empty(), "no stream, no run");
    require(offerStills({}, tabletStreams, false, capped, 30).sizes.empty(), "no format, no photo");
}

// The platform raises for a scaled size of another aspect, so the delivered
// size is compared with the format's by cross multiplication.
bool keepsAspect(FrameSize delivered, std::uint32_t formatWidth, std::uint32_t formatHeight)
{
    return std::uint64_t(delivered.width) * formatHeight == std::uint64_t(delivered.height) * formatWidth;
}

void aStreamSmallerThanItsFormatIsDeliveredScaled()
{
    require(deliveredFrameSize(1920, 1440, 1440, 1080) == FrameSize({1440, 1080}), "the preview of a 12 MP format is 1440x1080");
    require(deliveredFrameSize(1920, 1440, 1280, 720) == FrameSize({1280, 960}), "a wider stream keeps the format's aspect ratio");
    require(deliveredFrameSize(4032, 3024, 1920, 1080) == FrameSize({1920, 1440}), "and covers the stream's longer side");
}

void aStreamOfAnotherAspectIsDeliveredAtTheFormatsAspect()
{
    // 640x480 from a 16:9 format: a multiple of 16:9 that covers 480 lines.
    require(deliveredFrameSize(1920, 1080, 640, 480) == FrameSize({864, 486}), "an even multiple of 16:9, not 854x480");
    require(deliveredFrameSize(1920, 1440, 1000, 1000) == FrameSize({1336, 1002}), "a square stream from 4:3");
    // The old rule rounded each side on its own: 1334x1000 is not 4:3.
    require(!keepsAspect({1334, 1000}, 1920, 1440), "independent rounding does not keep the ratio");
    require(keepsAspect(deliveredFrameSize(1920, 1440, 1000, 1000), 1920, 1440), "the delivered size does");
}

void everyStandardSizeIsDeliveredAtEveryFormatsAspect()
{
    const std::pair<std::uint32_t, std::uint32_t> standard[] {{176, 144}, {320, 240}, {640, 480}, {1280, 720}, {1920, 1080}, {3840, 2160}};
    const std::pair<std::uint32_t, std::uint32_t> formats[] {{1440, 1080}, {1920, 1440}, {4032, 3024}, {1920, 1080}, {3840, 2160}, {4224, 2376}, {1088, 1088}};
    for (const auto& [formatWidth, formatHeight] : formats)
        for (const auto& [width, height] : standard) {
            if (formatWidth < width || formatHeight < height) continue;  // no format of this size takes the stream
            const auto delivered = deliveredFrameSize(formatWidth, formatHeight, width, height);
            require(keepsAspect(delivered, formatWidth, formatHeight), "the format's aspect, by cross multiplication");
            require(delivered.width >= width && delivered.height >= height, "covers the stream");
            require(delivered.width <= formatWidth && delivered.height <= formatHeight, "never above the format");
            require(delivered.width % 2 == 0 && delivered.height % 2 == 0, "even sides");
        }
}

void aRatioWithAnOddSideTakesAnEvenMultiple()
{
    // A 3:2 ratio has an odd side, so its multiple is even: 427 would give
    // the odd width 1281.
    require(deliveredFrameSize(1088, 1088, 640, 480) == FrameSize({640, 640}), "the multiple of a 1:1 format");
    require(deliveredFrameSize(3000, 2000, 1280, 720) == FrameSize({1284, 856}), "3:2 at 1280x720 is the multiple 428");
    require(keepsAspect(deliveredFrameSize(3000, 2000, 1280, 720), 3000, 2000), "of 3:2");
}

void aStreamTheFormatJustCoversIsDeliveredAsItIs()
{
    require(deliveredFrameSize(1440, 1080, 1440, 1080) == FrameSize({1440, 1080}), "the same size");
    require(deliveredFrameSize(1440, 1080, 1920, 1440) == FrameSize({1440, 1080}), "never above the format's own size");
    require(deliveredFrameSize(1440, 1080, 0, 0) == FrameSize({1440, 1080}), "a stream of no size leaves the format's");
    require(deliveredFrameSize(0, 0, 640, 480) == FrameSize({0, 0}), "a format of no size has none");
}

} // namespace

int main()
{
    anExactSizeTheFormatListsIsAsked();
    aSizeAnotherFormatListsIsNotAsked();
    aLargerSizeThatCoversIsNotTheRequest();
    theOutputsMaximumBoundsTheRequest();
    anEmptyOrZeroSizedTableAsksForNothing();
    aCaptureWithoutAJpegTakesTheSmallestSize();
    theResolvedSizeIsTheRequestOrItIsRefused();
    theOutputIsAttachedBeforeItsMaximumIsSet();
    aMaximumTheSourceRefusesDetachesTheOutput();
    anOutputThatCannotBeAttachedGetsNoMaximum();
    aFormatThatListsNothingLeavesTheDefault();
    aMaximumSetBeforeAttachIsRefused();
    theStreamRunsInTheSmallestFormatThatMakesTheLargestStill();
    aFormatThatCannotTakeTheRateOrTheSizeIsNotChosen();
    aMultiCameraSessionChoosesAmongItsFormatsOnly();
    theFullRangeFormatWinsAmongEqualOnes();
    aFormatThatWouldDeliverMoreThanTheBoundIsNotChosen();
    theCapDecidesWhichStillAFormatIsChosenFor();
    aSizeWiderThanTheOutputMaximumIsNotMade();
    theOfferIsWhatEveryRunMakes();
    aSizeOnlyAnotherFormatMakesIsNotOffered();
    aRateInsideTheRangeDecidesWhichFormatARunUses();
    aCameraKeepsItsPhotosOverTheFastStreamsThatCannotMakeThem();
    aFormatThatMakesTheLargestStillAtAHigherRateRaisesTheCeiling();
    aConcurrentCameraOffersWhatAMultiCameraFormatMakes();
    aStillWithoutAStreamSelectsTheFormatThatMakesItsSize();
    aCameraWithoutStreamsOffersNothing();
    aStreamSmallerThanItsFormatIsDeliveredScaled();
    aStreamOfAnotherAspectIsDeliveredAtTheFormatsAspect();
    everyStandardSizeIsDeliveredAtEveryFormatsAspect();
    aRatioWithAnOddSideTakesAnEvenMultiple();
    aStreamTheFormatJustCoversIsDeliveredAsItIs();
    return 0;
}
