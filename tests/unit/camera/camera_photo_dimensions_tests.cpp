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

void theStreamRunsInTheSmallestFormatThatListsTheLargestStill()
{
    const auto chosen = chooseVideoFormat(tabletFormats, 1440, 1080, 30, false, twelveMegapixels);
    require(chosen && *chosen == 1, "1920x1440 is the smallest covering format that lists 12 MP");
    const auto plain = chooseVideoFormat(tabletFormats, 1440, 1080, 30, false, {});
    require(plain && *plain == 0, "without a preferred still it is the smallest covering format");
}

void aFormatThatCannotTakeTheRateOrTheSizeIsNotChosen()
{
    // 60 fps: only the 1080p format takes it, and it lists no 12 MP; the
    // preference does not reach past a format that cannot carry the stream.
    const auto fast = chooseVideoFormat(tabletFormats, 1280, 720, 60, false, twelveMegapixels);
    require(fast && *fast == 3, "the only format that takes 60 fps is chosen without the large still");
    require(!chooseVideoFormat(tabletFormats, 4096, 3072, 30, false, twelveMegapixels), "nothing covers 4096x3072");
    require(!chooseVideoFormat(tabletFormats, 640, 480, 120, false, twelveMegapixels), "nothing takes 120 fps");
}

void aMultiCameraSessionChoosesAmongItsFormatsOnly()
{
    auto formats = tabletFormats;
    formats[1].multiCamera = true;  // only the 1920x1440 format joins
    const auto chosen = chooseVideoFormat(formats, 1440, 1080, 30, true, twelveMegapixels);
    require(chosen && *chosen == 1, "the multi-camera format is chosen");
    formats[1].multiCamera = false;
    require(!chooseVideoFormat(formats, 1440, 1080, 30, true, twelveMegapixels), "none joins: nothing is chosen");
}

void theFullRangeFormatWinsAmongEqualOnes()
{
    std::vector<VideoFormatFacts> formats {format(1920, 1440, {{4032, 3024}}, 30, false), format(1920, 1440, {{4032, 3024}}, 30, true)};
    const auto chosen = chooseVideoFormat(formats, 1920, 1440, 30, false, twelveMegapixels);
    require(chosen && *chosen == 1, "the full-range format of the same size and sizes");
}

void theSizesOfferedAreTheOnesEveryRunMakes()
{
    // Streams the guest may configure: preview sizes at 30 fps, and 1080p
    // video up to 60 fps, which runs in the 1080p format.
    const std::vector<VideoRequest> requests {{1440, 1080, 1, 30}, {1920, 1080, 1, 60}};
    const auto offered = photoSizesOfEveryRun(tabletFormats, requests, twelveMegapixels);
    // 1440x1080 runs in 1920x1440 {1920x1440, 4032x3024}; 1920x1080 at 60 fps
    // runs in the 1080p format {1920x1080, 2016x1134}: nothing is common.
    require(offered.empty(), "a size one run cannot make is not offered");
    const std::vector<VideoRequest> previews {{1440, 1080, 1, 30}, {640, 480, 1, 30}, {1920, 1440, 1, 30}};
    const auto previewSizes = photoSizesOfEveryRun(tabletFormats, previews, twelveMegapixels);
    require(previewSizes == Sizes({{4032, 3024}, {1920, 1440}}), "the sizes of the shared format, largest first");
}

void aSizeOnlyAnotherFormatListsIsNotOffered()
{
    // 4224x2376 is listed by the 12 MP video format only; the camera's streams
    // run in 1920x1440, so it is not offered, while 4032x3024 is.
    const std::vector<VideoRequest> requests {{1440, 1080, 1, 30}, {1920, 1440, 1, 30}};
    const auto offered = photoSizesOfEveryRun(tabletFormats, requests, twelveMegapixels);
    require(std::find(offered.begin(), offered.end(), CameraStillSize {4224, 2376}) == offered.end(), "the 16:9 size is not offered");
    require(!offered.empty() && offered.front() == twelveMegapixels, "the 12 MP size is");
}

void aCameraWithoutTheLargestStillOffersWhatItsFormatMakes()
{
    // No format lists 12 MP: the preference finds nothing and the smallest
    // covering format decides, so the offer shrinks to its sizes.
    std::vector<VideoFormatFacts> formats {format(1440, 1080, {{1440, 1080}, {2016, 1512}}), format(1920, 1440, {{1920, 1440}})};
    const std::vector<VideoRequest> requests {{1440, 1080, 1, 30}};
    const auto offered = photoSizesOfEveryRun(formats, requests, {5712, 4284});
    require(offered == Sizes({{2016, 1512}, {1440, 1080}}), "the running format's own sizes");
}

void aCameraWithoutRequestsOffersNothing()
{
    require(photoSizesOfEveryRun(tabletFormats, {}, twelveMegapixels).empty(), "no stream, no run");
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
    theStreamRunsInTheSmallestFormatThatListsTheLargestStill();
    aFormatThatCannotTakeTheRateOrTheSizeIsNotChosen();
    aMultiCameraSessionChoosesAmongItsFormatsOnly();
    theFullRangeFormatWinsAmongEqualOnes();
    theSizesOfferedAreTheOnesEveryRunMakes();
    aSizeOnlyAnotherFormatListsIsNotOffered();
    aCameraWithoutTheLargestStillOffersWhatItsFormatMakes();
    aCameraWithoutRequestsOffersNothing();
    return 0;
}
