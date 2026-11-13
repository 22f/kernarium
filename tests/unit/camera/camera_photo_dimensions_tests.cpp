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
    aStreamSmallerThanItsFormatIsDeliveredScaled();
    aStreamOfAnotherAspectIsDeliveredAtTheFormatsAspect();
    everyStandardSizeIsDeliveredAtEveryFormatsAspect();
    aRatioWithAnOddSideTakesAnEvenMultiple();
    aStreamTheFormatJustCoversIsDeliveredAsItIs();
    return 0;
}
