// SPDX-License-Identifier: GPL-2.0-or-later
// The anchor model of photo_dimensions.hpp: the one format a camera runs in,
// what it offers, the rates it takes, and the session's transactions. Every
// reviewer counter-example of the earlier choosers is a named test built from
// the reviewer's table, and the invariants hold over generated tables (the seed
// is in a failure's message).
#include "photo_dimensions.hpp"
#include "require.hpp"

#include <Block.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace uml::platform;
using uml::testing::require;

namespace {

using Sizes = std::vector<CameraStillSize>;
using Spans = std::vector<CameraRateSpan>;

VideoFormatFacts fmt(std::uint32_t width, std::uint32_t height, Sizes photos, Spans rates = {{1, 30}}, bool full = true,
                     bool multi = false)
{
    VideoFormatFacts facts;
    facts.width = width;
    facts.height = height;
    facts.frameRates = std::move(rates);
    facts.photoSizes = std::move(photos);
    facts.fullRange = full;
    facts.multiCamera = multi;
    return facts;
}

constexpr std::pair<std::uint32_t, std::uint32_t> standardSizes[] = {
    {176, 144}, {320, 240}, {640, 480}, {1280, 720}, {1920, 1080}, {3840, 2160},
};

// The sizes a camera's video is cut from: the standard sizes and the native sizes
// of its formats (candidateVideoSizes in the device).
std::vector<FrameSize> candidatesFor(const std::vector<VideoFormatFacts>& formats)
{
    std::vector<FrameSize> sizes;
    for (const auto& [width, height] : standardSizes) sizes.push_back({width, height});
    for (const auto& format : formats)
        if (std::find(sizes.begin(), sizes.end(), FrameSize {format.width, format.height}) == sizes.end())
            sizes.push_back({format.width, format.height});
    return sizes;
}

// The capture buffer the reserve counts, by hand: rows padded to 256 bytes, so a
// 1920-wide row is 2048 bytes.
constexpr std::uint32_t rowAlignment = 256;
constexpr std::uint64_t smallFrameBound = 2048ull * 1440 * 3 / 2;
constexpr std::uint64_t largeFrameBound = 3840ull * 2880 * 3 / 2;

// The small tier: 1080p video, 12 MP stills (12.7 million pixels), and the
// frame of 1920x1440 at most.
OfferRules smallRules(const std::vector<VideoFormatFacts>& formats)
{
    OfferRules rules;
    rules.limits = {12'700'000, smallFrameBound, rowAlignment};
    rules.maximumVideoWidth = 1920;
    rules.maximumVideoHeight = 1080;
    rules.candidateSizes = candidatesFor(formats);
    return rules;
}

OfferRules largeRules(const std::vector<VideoFormatFacts>& formats)
{
    OfferRules rules;
    rules.limits = {50'000'000, largeFrameBound, rowAlignment};
    rules.maximumVideoWidth = 3840;
    rules.maximumVideoHeight = 2160;
    rules.candidateSizes = candidatesFor(formats);
    return rules;
}

// ---------------------------------------------------------------------------
// Rates
// ---------------------------------------------------------------------------

void aSpansWholeRatesSnapToTheNearWholeNumber()
{
    const auto whole = [](double low, double high) { return wholeRatesOf({low, high}); };
    require(whole(29.97, 29.97) == std::optional<WholeRates>(WholeRates {30, 30}), "29.97 is 30");
    require(whole(30.00003, 30.00003) == std::optional<WholeRates>(WholeRates {30, 30}), "30.00003 is 30");
    require(whole(23.976, 23.976) == std::optional<WholeRates>(WholeRates {24, 24}), "23.976 is 24");
    require(whole(14.99, 14.99) == std::optional<WholeRates>(WholeRates {15, 15}), "14.99 is 15");
    require(whole(1, 30) == std::optional<WholeRates>(WholeRates {1, 30}), "a plain range keeps its ends");
    require(whole(29.97, 59.94) == std::optional<WholeRates>(WholeRates {30, 60}), "both ends snap");
    require(whole(0.5, 30) == std::optional<WholeRates>(WholeRates {1, 30}), "a range from below one starts at one");
    require(whole(1.0005, 30) == std::optional<WholeRates>(WholeRates {1, 30}), "a hair above one is one");
    require(whole(7.5, 7.9) == std::nullopt, "a span between two whole numbers holds none");
    require(whole(7.5, 8.2) == std::optional<WholeRates>(WholeRates {8, 8}), "but one that spans a whole number holds it");
    require(whole(29.9, 29.9) == std::nullopt, "29.9 is not 30: it is a third of a percent away");
    require(whole(30, 20) == std::nullopt, "an inverted span holds none");
    require(whole(std::nan(""), 30) == std::nullopt && whole(1, std::nan("")) == std::nullopt, "no number, no rate");
    require(whole(0, 0) == std::nullopt, "a span of nothing");
    require(whole(1, 1e12)->maximum == 4'000'000'000u, "an absurd rate is bounded");
}

void aRateAHairOutsideASpanIsProgrammedFromTheSpansOwnDuration()
{
    const auto facts = fmt(1920, 1080, {}, {{29.97, 29.97}, {1, 15}, {30.00003, 30.00003}});
    const auto up = rateSettingFor(facts, 30);
    require(up && up->span == 0 && up->duration == FrameDuration::spanShortest,
            "30 on a 29.97 span is the span's shortest duration");
    require(std::fabs(programmedFrameDuration(facts.frameRates[0], up->duration, 30) - 1.0 / 29.97) < 1e-12, "which is 1 / 29.97");
    const auto down = rateSettingFor(fmt(1920, 1080, {}, {{30.00003, 30.00003}}), 30);
    require(down && down->span == 0 && down->duration == FrameDuration::spanLongest,
            "30 on a 30.00003 span is the span's longest duration");
    const auto inside = rateSettingFor(facts, 10);
    require(inside && inside->span == 1 && inside->duration == FrameDuration::reciprocal, "a rate inside a span is its reciprocal");
    require(!rateSettingFor(facts, 20), "no span holds 20");
    require(!rateSettingFor(fmt(1920, 1080, {}, {}), 30), "a format with no span takes no rate");
    // An exact span is preferred to a snapped one.
    const auto both = rateSettingFor(fmt(1920, 1080, {}, {{1, 29.97}, {30, 60}}), 30);
    require(both && both->span == 1 && both->duration == FrameDuration::reciprocal, "the span that holds 30 exactly wins");
}

void theOfferedRatesAreWholeRatesUpToTheCap()
{
    const auto rates = [](Spans spans, std::uint32_t cap = 30) { return offeredRatesOf(fmt(1920, 1080, {}, std::move(spans)), cap); };
    require(rates({{1, 60}}) == std::vector<WholeRates> {{1, 30}}, "a range is clipped at the cap");
    require(rates({{1, 10}, {50, 60}}) == std::vector<WholeRates> {{1, 10}}, "a span above the cap is not offered");
    require(rates({{24, 24}, {1, 15}, {15, 24}}) == std::vector<WholeRates> {{1, 24}}, "overlapping and adjacent ranges merge");
    require(rates({{29.97, 29.97}, {23.976, 23.976}, {14.99, 14.99}}) == std::vector<WholeRates> {{15, 15}, {24, 24}, {30, 30}},
            "near-integer rates are whole rates");
    require(rates({{60, 60}, {120, 120}}) == std::vector<WholeRates> {{60, 60}}, "a format with whole rates only above the cap offers its slowest");
    require(rates({{7.5, 7.9}}).empty(), "a format with no whole rate offers none");
    require(rates({{1, 60}}, 0) == std::vector<WholeRates> {{1, 60}}, "a cap of zero is none");
}

void aPressureCapChoosesTheLargestOfferedRateBelowIt()
{
    const std::vector<WholeRates> offered {{1, 10}, {24, 24}, {30, 30}};
    require(throttledRate(offered, 30, 0) == 30, "no cap");
    require(throttledRate(offered, 30, 20) == 10, "20 is not offered: the largest below it");
    require(throttledRate(offered, 30, 24) == 24, "an offered rate");
    require(throttledRate(offered, 15, 0) == 10, "a request between two ranges");
    require(throttledRate(offered, 30, 15) == 10, "a cap of 15");
    require(throttledRate(offered, 60, 0) == 30, "above every range");
    const std::vector<WholeRates> fast {{120, 120}};
    require(throttledRate(fast, 30, 15) == 120, "nothing offered below the cap: the slowest offered");
    require(throttledRate({}, 30, 0) == 0, "nothing offered, nothing to run");
    require(throttledRate(offered, 30, 99) == 30, "a cap above the request does nothing");
}

// ---------------------------------------------------------------------------
// Photos
// ---------------------------------------------------------------------------

void aFormatsPhotoMaximumIsTheLargestSizeWithinTheCap()
{
    const auto phone = fmt(4032, 3024, {{4032, 3024}, {8064, 6048}, {2016, 1512}});
    require(photoMaximum(phone, 12'700'000) == std::optional<CameraStillSize>(CameraStillSize {4032, 3024}),
            "a 48 MP size is not the maximum under a 12 MP cap");
    require(photoMaximum(phone, 0) == std::optional<CameraStillSize>(CameraStillSize {8064, 6048}), "without a cap it is");
    require(!photoMaximum(fmt(1920, 1080, {{8064, 6048}}), 12'700'000), "no size within the cap, no maximum");
    require(!photoMaximum(fmt(1920, 1080, {}), 0) && !photoMaximum(fmt(1920, 1080, {{0, 0}, {4032, 0}}), 0), "no usable size");
    // Equal areas order by width.
    require(photoMaximum(fmt(1, 1, {{3000, 2000}, {2000, 3000}}), 0) == std::optional<CameraStillSize>(CameraStillSize {3000, 2000}),
            "the wider of two sizes of one area");
}

void theStillsAFormatMakesAreWithinTheMaximumInBothDimensions()
{
    const auto format = fmt(4032, 3024, {{4032, 3024}, {4224, 2376}, {1920, 1080}, {1920, 1080}, {0, 0}});
    const auto stills = stillsOf(format, 12'700'000);
    require(stills == Sizes({{4032, 3024}, {1920, 1080}}),
            "4224x2376 is wider than the maximum, the duplicate and the empty size are not sizes");
    require(formatMakesPhoto(format, {4032, 3024}, 12'700'000) && formatMakesPhoto(format, {1920, 1080}, 12'700'000), "made");
    require(!formatMakesPhoto(format, {4224, 2376}, 12'700'000), "listed, but wider than the output's maximum");
    require(!formatMakesPhoto(format, {2016, 1512}, 12'700'000), "not listed");
    require(stillsOf(fmt(1, 1, {}), 0).empty(), "a format listing none makes none");
}

void theSizesAnyFormatMakesAreTheUnionLargestFirst()
{
    const std::vector<VideoFormatFacts> formats {fmt(1920, 1440, {{1920, 1440}, {4032, 3024}}), fmt(1920, 1080, {{1920, 1080}, {2016, 1134}})};
    const auto all = stillsMadeByAnyFormat(formats, 12'700'000);
    require(all == Sizes({{4032, 3024}, {1920, 1440}, {2016, 1134}, {1920, 1080}}), "the union, by area then width");
}

void theOutputSizesFollowTheAnchor()
{
    const auto none = outputSizesFor(fmt(1920, 1080, {}), 1920, 1080, 12'700'000);
    require(!none.frame && !none.photoMaximum, "the format's own size and no photo size to set");
    const auto scaled = outputSizesFor(fmt(1920, 1080, {{1920, 1080}}), 640, 480, 12'700'000);
    require(scaled.frame == std::optional<FrameSize>(FrameSize {864, 486}) && scaled.photoMaximum == CameraStillSize({1920, 1080}),
            "864x486 and the format's largest");
    require(!outputSizesFor(fmt(1920, 1080, {}), 0, 0, 0).frame, "a camera with no stream has no video output to size");
    const auto phone = outputSizesFor(fmt(4032, 3024, {{4032, 3024}, {8064, 6048}}), 1920, 1080, 12'700'000);
    require(phone.photoMaximum == CameraStillSize({4032, 3024}), "the output is capped at 12 MP: no 48 MP pipeline is prepared");
}

// ---------------------------------------------------------------------------
// The anchor: each counter-example the reviewers built, as a table
// ---------------------------------------------------------------------------

struct Chosen {
    std::size_t index {};
    CaptureOffer offer;
};
Chosen anchorOf(const std::vector<VideoFormatFacts>& formats, bool multiOnly = false, bool large = false)
{
    const OfferRules rules = large ? largeRules(formats) : smallRules(formats);
    const auto index = chooseAnchor(formats, rules, multiOnly);
    require(index.has_value(), "the table has an anchor");
    return {*index, offerOf(formats[*index], rules)};
}

const std::vector<VideoFormatFacts> tabletFormats {
    fmt(1440, 1080, {{1440, 1080}, {2016, 1512}}, {{1, 30}}, true, true),
    fmt(1920, 1440, {{1920, 1440}, {4032, 3024}}, {{1, 30}}, true, true),
    fmt(4032, 3024, {{4032, 3024}, {4224, 2376}}, {{1, 30}}, true, false),
    fmt(1920, 1080, {{1920, 1080}, {2016, 1134}}, {{1, 60}}, true, true),
};

// R1, the tablet fallback (recheck3): a stream of every size runs in 1920x1440 at
// every rate, and the 12 MP photo and the 1920x1440 one are made in every run.
// 4224x2376, which only the 4032x3024 format lists, is not offered.
void theTabletRunsInTheFormatThatMakesTheLargestStillAtTheSmallestCost()
{
    const auto chosen = anchorOf(tabletFormats);
    require(chosen.index == 1, "1920x1440: K2 ties it with the 4032x3024 format, K4 takes the smaller");
    require(chosen.offer.stills == Sizes({{4032, 3024}, {1920, 1440}}), "4224x2376 is never offered");
    require(chosen.offer.videoSizes.front() == FrameSize({1920, 1080}) && chosen.offer.box == FrameSize({1920, 1080}), "video up to 1080p");
    require(chosen.offer.rates == std::vector<WholeRates> {{1, 30}}, "the anchor's rates, to the cap");
    const auto multi = anchorOf(tabletFormats, true);
    require(multi.index == 1, "the same among the formats a multi-camera session takes");
}

// R1 (recheck3, interior rate): 4K lists 12 MP at 1-10 and 50-60 fps only.
void aFormatThatHasNoRateWithinTheCapLosesToOneThatDoes()
{
    const std::vector<VideoFormatFacts> formats {fmt(1920, 1080, {{1920, 1080}}, {{1, 60}}),
                                                 fmt(3840, 2160, {{4032, 3024}}, {{1, 10}, {50, 60}})};
    const auto chosen = anchorOf(formats);
    require(chosen.index == 0, "30 fps beats 10 (K1 before K2)");
    require(chosen.offer.stills == Sizes({{1920, 1080}}) && chosen.offer.rates == std::vector<WholeRates> {{1, 30}},
            "its stills and its rates to the cap");
}

// R1 (recheck4, combined request): 1600x900 and 1440x1080 make 12 MP, 1600x1200
// only 1080p. A stream of 1600x900 and one of 1440x1080 together asked for
// 1600x1080 in the old model, a size no format made 12 MP in.
void everyAdmittedStreamSetFitsInsideTheAnchor()
{
    const std::vector<VideoFormatFacts> formats {fmt(1600, 900, {{4032, 3024}}), fmt(1440, 1080, {{4032, 3024}}), fmt(1600, 1200, {{1920, 1080}})};
    const auto chosen = anchorOf(formats);
    require(chosen.index == 1, "1440x1080: its box is larger than 1600x900's (K3)");
    require(chosen.offer.stills == Sizes({{4032, 3024}}), "12 MP");
    for (const auto& size : chosen.offer.videoSizes)
        require(size.width <= 1440 && size.height <= 1080, "1600x900 is not offered, so 1600x1080 cannot be asked for");
}

// R1 (recheck4, under-offer): 1920x1440 at exactly 30 fps makes 12 MP; the
// 1080p format at 1-30 makes 1080p only. Every offered run can use the first.
void aFormatAtExactlyTheCapStillOffersItsPhotos()
{
    const std::vector<VideoFormatFacts> formats {fmt(1920, 1440, {{4032, 3024}}, {{30, 30}}), fmt(1920, 1080, {{1920, 1080}}, {{1, 30}})};
    const auto chosen = anchorOf(formats);
    require(chosen.index == 0 && chosen.offer.stills == Sizes({{4032, 3024}}), "12 MP is offered");
    require(chosen.offer.rates == std::vector<WholeRates> {{30, 30}} && chosen.offer.videoSizes.front() == FrameSize({1920, 1080}),
            "at 30 fps and up to 1080p: the rates below 30 are given up");
}

// R4 (a) (recheck4): a small format makes 12 MP and a larger one lists 12 and
// 48 MP. The camera runs only the anchor, and the photo output is capped.
void aCameraNeverRunsInAFormatLargerThanItsAnchor()
{
    const std::vector<VideoFormatFacts> formats {fmt(4032, 3024, {{4032, 3024}, {8064, 6048}}), fmt(1920, 1440, {{4032, 3024}})};
    const auto chosen = anchorOf(formats);
    require(chosen.index == 1, "K4 takes the smaller format: both make 12 MP under the cap");
    require(photoMaximum(formats[chosen.index], 12'700'000) == CameraStillSize({4032, 3024}), "and its output is capped at 12 MP");
    require(photoMaximum(formats[0], 12'700'000) == CameraStillSize({4032, 3024}), "so even the larger format prepares no 48 MP pipeline");
}

// N1 (recheck4, only fast): no format has a rate within the cap. Nothing is
// clipped away: the camera keeps its video and its photos.
void aCameraWithOnlyFastFormatsKeepsAModeAndItsPhotos()
{
    const std::vector<VideoFormatFacts> formats {fmt(640, 480, {{1920, 1080}}, {{60, 60}}), fmt(1920, 1080, {{4032, 3024}}, {{120, 120}})};
    const auto chosen = anchorOf(formats);
    require(chosen.index == 1, "K1 ties; K2 takes the format that makes 12 MP");
    require(chosen.offer.stills == Sizes({{4032, 3024}}) && !chosen.offer.videoSizes.empty() &&
                chosen.offer.rates == std::vector<WholeRates> {{120, 120}},
            "its slowest rate is offered alone");
}

// N1 (recheck4, lost 30): 1080p at 1-60 and 1920x1440 at 1-30. The 1440 format
// is the anchor; native 1080p stays offered.
void aFormatThatTakesTheTierSizeKeepsItsModesWhenAnotherWins()
{
    const std::vector<VideoFormatFacts> formats {fmt(1920, 1080, {{1920, 1080}}, {{1, 60}}), fmt(1920, 1440, {{4032, 3024}}, {{1, 30}})};
    const auto chosen = anchorOf(formats);
    require(chosen.index == 1 && chosen.offer.stills == Sizes({{4032, 3024}}), "12 MP wins");
    require(chosen.offer.videoSizes.front() == FrameSize({1920, 1080}) && chosen.offer.rates == std::vector<WholeRates> {{1, 30}},
            "1080p at 1-30 stays: the camera keeps a mode");
}

// N2 (recheck4): 1080p at exactly 60 and 1920x1920 at 1-30, both 12 MP. Every
// size offered starts, because the box was built from the anchor's own frame.
void everyOfferedSizeStartsInTheAnchor()
{
    const std::vector<VideoFormatFacts> formats {fmt(1920, 1080, {{4032, 3024}}, {{60, 60}}), fmt(1920, 1920, {{4032, 3024}}, {{1, 30}})};
    const auto chosen = anchorOf(formats);
    require(chosen.index == 1, "the format that has a rate within the cap");
    require(chosen.offer.box == FrameSize({1280, 720}), "a 1080p frame from 1:1 would be 1920x1920, over the bound");
    for (const auto& size : chosen.offer.videoSizes)
        require(deliveredFrameBytes(formats[1], size.width, size.height, rowAlignment) <= smallFrameBound, "every offered size fits the frame bound");
    require(deliveredFrameBytes(formats[1], chosen.offer.box.width, chosen.offer.box.height, rowAlignment) <= smallFrameBound, "so does the box");
}

// R2 (recheck5): a 1802x1534 format delivers 1802x1534 to every size it is asked for
// (its reduced aspect ratio, 901:767, has no smaller even multiple). Its pixels,
// 4,146,402 bytes tightly packed, are under the old bound of 4,147,200; in
// capture buffers whose rows are padded to 256 bytes it is 4,712,448, over the
// 4,423,680 the reserve counts. The offer is bounded by the reserve's figure, so
// the format offers no size and is not an anchor, and no camera that has only it
// is offered.
void aFrameThatFitsTightlyButNotInTheReservedBufferIsNotOffered()
{
    const VideoFormatFacts odd = fmt(1802, 1534, {{4032, 3024}});
    require(deliveredFrameSize(1802, 1534, 1280, 720) == FrameSize({1802, 1534}), "1280x720 is delivered as the whole format");
    require(deliveredFrameBytes(odd, 1280, 720, 0) == 1802ull * 1534 * 3 / 2 && deliveredFrameBytes(odd, 1280, 720, 0) < 4'147'200,
            "tightly packed it is under 1920x1440's bytes");
    require(deliveredFrameBytes(odd, 1280, 720, rowAlignment) == 4'712'448, "in 256-byte rows it is 4,712,448");
    require(deliveredFrameBytes(odd, 1280, 720, rowAlignment) > smallFrameBound, "which is over the reserved buffer");
    const std::vector<VideoFormatFacts> alone {odd};
    const OfferRules rules = smallRules(alone);
    require(offerOf(odd, rules).videoSizes.empty(), "no size of it is offered");
    require(!anchorKeyOf(odd, rules) && !chooseAnchor(alone, rules, false), "so it is no anchor");
    // With the bound of tightly packed rows (the old figure), it was admitted.
    OfferRules tight = rules;
    tight.limits.frameRowAlignment = 0;
    require(!offerOf(odd, tight).videoSizes.empty(), "the same format was offered when rows were counted tightly");
    // Beside a format that fits, the camera runs in that one.
    const std::vector<VideoFormatFacts> two {odd, fmt(1920, 1440, {{4032, 3024}})};
    require(anchorOf(two).index == 1, "the format that fits is the anchor");
}

// The Mac's external camera lists near-integer rates.
void aCameraThatListsNearIntegerRatesIsOfferedWholeRates()
{
    const std::vector<VideoFormatFacts> formats {fmt(1920, 1080, {{1920, 1080}}, {{29.97, 29.97}, {23.976, 23.976}, {20, 20}, {14.99, 14.99}}),
                                                 fmt(1280, 720, {{1280, 720}}, {{29.97, 29.97}})};
    const auto chosen = anchorOf(formats);
    require(chosen.index == 0 && chosen.offer.rates == std::vector<WholeRates> {{15, 15}, {20, 20}, {24, 24}, {30, 30}},
            "30 24 20 15: the Mac's rate goes from 20 to 30");
    const auto setting = rateSettingFor(formats[0], 30);
    require(setting && setting->duration == FrameDuration::spanShortest, "30 is programmed as the span's own shortest duration");
}

void theLargeTierOffersWhatItsLimitsHold()
{
    const std::vector<VideoFormatFacts> formats {fmt(3840, 2160, {{8064, 6048}, {4032, 3024}}, {{1, 60}}), fmt(1920, 1440, {{4032, 3024}}, {{1, 30}})};
    const auto chosen = anchorOf(formats, false, true);
    require(chosen.index == 0 && chosen.offer.stills.front() == CameraStillSize({8064, 6048}), "48 MP in the large tier");
    require(chosen.offer.videoSizes.front() == FrameSize({3840, 2160}), "4K video");
}

void aFormatThatCarriesNoStreamIsNotAnAnchor()
{
    require(!chooseAnchor({}, smallRules({}), false), "no formats, no anchor");
    const std::vector<VideoFormatFacts> noRate {fmt(1920, 1080, {{1920, 1080}}, {{7.5, 7.9}})};
    require(!chooseAnchor(noRate, smallRules(noRate), false), "no whole rate, no anchor");
    const std::vector<VideoFormatFacts> noSize {fmt(8, 8, {{1920, 1080}}, {{1, 30}})};
    require(!chooseAnchor(noSize, smallRules(noSize), false), "no video size in its box, no anchor");
    const std::vector<VideoFormatFacts> neverUsable {fmt(1920, 1080, {{1920, 1080}}, {{7.5, 7.9}}), fmt(1280, 720, {}, {{1, 30}})};
    const auto chosen = chooseAnchor(neverUsable, smallRules(neverUsable), false);
    require(chosen && *chosen == 1, "a format that lists no photo can still be the anchor");
    const std::vector<VideoFormatFacts> single {fmt(1920, 1080, {{1920, 1080}}, {{1, 30}}, true, false)};
    require(!chooseAnchor(single, smallRules(single), true), "no multi-camera format, no multi-camera anchor");
}

void theFrameBoundKeepsAnOversizedFormatOutOfTheBox()
{
    // A 1:1 format delivers a 1080p frame as 1920x1920 (5,529,600 bytes): over the bound.
    const auto square = fmt(1920, 1920, {{4032, 3024}}, {{1, 30}});
    const std::vector<VideoFormatFacts> formats {square};
    const auto offer = offerOf(square, smallRules(formats));
    require(offer.box == FrameSize({1280, 720}), "the box ends at 1280x720");
    for (const auto& size : offer.videoSizes) require(size.width <= 1280 && size.height <= 720, "no larger size is offered");
}

void aTiesBreakByCostThenLowRatesThenFullRangeThenIndex()
{
    const Sizes photos {{4032, 3024}};
    std::vector<VideoFormatFacts> formats {fmt(4032, 3024, photos), fmt(1920, 1440, photos)};
    require(anchorOf(formats).index == 1, "the smaller frame");
    formats = {fmt(1920, 1440, photos, {{5, 30}}), fmt(1920, 1440, photos, {{1, 30}})};
    require(anchorOf(formats).index == 1, "the lower minimum rate");
    formats = {fmt(1920, 1440, photos, {{1, 30}}, false), fmt(1920, 1440, photos, {{1, 30}}, true)};
    require(anchorOf(formats).index == 1, "the full-range format");
    formats = {fmt(1920, 1440, photos), fmt(1920, 1440, photos)};
    require(anchorOf(formats).index == 0, "the lower index");
}

void theKeyOrderIsRateThenStillThenVideo()
{
    const Sizes twelve {{4032, 3024}};
    // A 12 MP format that reaches only 15 fps loses to a 30 fps one (K1 before K2).
    std::vector<VideoFormatFacts> formats {fmt(1920, 1440, twelve, {{1, 15}}), fmt(1920, 1080, {{1920, 1080}}, {{1, 30}})};
    require(anchorOf(formats).index == 1, "the format that reaches 30");
    // Equal rate, a larger still wins over a larger video box (K2 before K3).
    formats = {fmt(1280, 720, twelve), fmt(1920, 1080, {{1920, 1080}})};
    require(anchorOf(formats).index == 0, "12 MP photos over 1080p video");
    // Equal rate and still: the larger video box (K3 before K4).
    formats = {fmt(1280, 720, twelve), fmt(1920, 1080, twelve)};
    require(anchorOf(formats).index == 1, "the larger box");
}

void aCameraKeepsConcurrencyOnlyWhenItsMultiCameraFormatsOfferAsMuch()
{
    const Sizes twelve {{4032, 3024}};
    std::vector<VideoFormatFacts> formats {fmt(1920, 1440, twelve, {{1, 30}}, true, true), fmt(1920, 1080, {{1920, 1080}})};
    require(keepsConcurrency(formats, smallRules(formats)), "the best format is multi-camera");
    formats = {fmt(1920, 1440, twelve, {{1, 30}}, true, false), fmt(1920, 1080, {{1920, 1080}}, {{1, 30}}, true, true)};
    require(!keepsConcurrency(formats, smallRules(formats)), "the multi-camera formats make a smaller photo");
    formats = {fmt(1920, 1440, twelve, {{1, 30}}, true, false), fmt(1920, 1440, twelve, {{1, 30}}, true, true)};
    require(keepsConcurrency(formats, smallRules(formats)), "a multi-camera format that offers as much");
    formats = {fmt(1920, 1440, twelve, {{1, 30}}, true, false)};
    require(!keepsConcurrency(formats, smallRules(formats)), "no multi-camera format at all");
    require(!keepsConcurrency({}, smallRules({})), "no formats");
}

void aCameraJoinsAMultiCameraSessionOnlyWhenEveryAnchorIsAFormatItTakes()
{
    const bool yes[] {true, true};
    const bool one[] {true, false};
    require(mayJoin(false, false, {}), "the first input joins whatever it is");
    require(mayJoin(true, true, yes), "every anchor is a multi-camera format");
    require(!mayJoin(true, false, yes), "this anchor is not");
    require(!mayJoin(true, true, one), "another input's is not");
    require(mayJoin(true, true, std::span<const bool>()), "peers that cannot be named count as none to check");
}

// ---------------------------------------------------------------------------
// The session's transactions: a fake device that records what was added and what
// was removed
// ---------------------------------------------------------------------------

// A device as the platform models it (the rules AVCaptureDevice.h states): the active format with the frame durations
// it holds, which a long manual exposure may make longer at the maximum than the rate asks for, and a manual exposure.
// A maximum duration shorter than a custom exposure shortens the exposure to it; a minimum above the maximum, or a
// maximum below the minimum, is refused; a format brings its own default durations; and adding the input to a session
// resets the durations (the fake session does that).
struct FakeDevice {
    DeviceTiming timing;
    std::vector<std::string> writes;
    bool refuseLock {}, refuseFormat {}, refuseExposure {};
    int locks {}, unlocks {};
    FakeDevice()
    {
        timing.format = 7;
        timing.minimum = timing.maximum = {1, 30};
    }
    [[nodiscard]] static CameraTime fps(std::int32_t rate) { return {1, rate}; }
    void durations(CameraTime minimum, CameraTime maximum)
    {
        timing.minimum = minimum;
        timing.maximum = maximum;
    }
    void customExposure(CameraTime duration, float iso) { timing.exposure = {true, duration, iso}; }
    // The platform shortens a longer custom exposure to a shorter maximum.
    void clamp()
    {
        if (timing.exposure.custom && timing.exposure.duration.seconds() > timing.maximum.seconds()) timing.exposure.duration = timing.maximum;
    }
    // What an activation does to the device: the format, then the minimum, and the maximum unless it is refused
    // between them (`middle` false).
    bool program(std::uint32_t format, CameraTime rate, bool middle)
    {
        timing.format = format;
        timing.minimum = rate;
        if (!middle) return false;
        timing.maximum = rate;
        clamp();
        return true;
    }
    TimingSteps steps()
    {
        TimingSteps s;
        s.lock = [this] {
            ++locks;
            return !refuseLock;
        };
        s.unlock = [this] { ++unlocks; };
        s.read = [this] { return timing; };
        s.setFormat = [this](std::uint32_t format) {
            if (refuseFormat) return false;
            timing.format = format;
            timing.minimum = timing.maximum = {1, 30};
            writes.push_back("format");
            return true;
        };
        s.setMinimum = [this](CameraTime time) {
            if (time.seconds() > timing.maximum.seconds()) return false;
            timing.minimum = time;
            writes.push_back("minimum");
            return true;
        };
        s.setMaximum = [this](CameraTime time) {
            if (time.seconds() < timing.minimum.seconds()) return false;
            timing.maximum = time;
            clamp();
            writes.push_back("maximum");
            return true;
        };
        s.setCustomExposure = [this](CameraTime duration, float iso) {
            if (refuseExposure) return false;
            timing.exposure = {true, duration, iso};
            writes.push_back("exposure");
            return true;
        };
        return s;
    }
};

struct FakeSession {
    bool input {}, video {}, photo {};
    std::vector<std::string> log;
    int begins {}, commits {};
    bool refuseInput {}, refuseActivate {}, refuseVideo {}, refusePhoto {}, overBudget {};
    // The device whose timing a refused change must not lose.
    FakeDevice device;
    DeviceTiming saved;
    bool restored {true};
    // Something the device does by itself while the call runs (an automatic exposure follows the scene).
    std::function<void(FakeDevice&)> meanwhile;

    AdmitSteps steps(const AdmitPlan& plan)
    {
        AdmitSteps s;
        s.begin = [this] { ++begins; log.push_back("begin"); };
        s.commit = [this] { ++commits; log.push_back("commit"); };
        s.addInput = [this] {
            log.push_back("addInput");
            if (refuseInput) return false;
            input = true;
            // Joining the session resets the device's durations to its format's.
            device.timing.minimum = device.timing.maximum = {1, 30};
            return true;
        };
        s.snapshotTiming = [this] {
            log.push_back("snapshot");
            saved = device.timing;
        };
        s.activate = [this] {
            log.push_back("activate");
            if (meanwhile) meanwhile(device);
            // The minimum is set first; a refusal can come between the two.
            return device.program(1, FakeDevice::fps(15), !refuseActivate);
        };
        s.addVideo = [this] {
            log.push_back("addVideo");
            if (refuseVideo) return false;
            video = true;
            return true;
        };
        s.addPhoto = [this] {
            log.push_back("addPhoto");
            if (refusePhoto) return false;
            photo = true;
            return true;
        };
        s.overBudget = [this] { return overBudget; };
        s.removeVideo = [this] {
            log.push_back("removeVideo");
            video = false;
        };
        s.removePhoto = [this] {
            log.push_back("removePhoto");
            photo = false;
        };
        s.removeInput = [this] {
            log.push_back("removeInput");
            input = false;
        };
        s.restoreTiming = [this] {
            log.push_back("restoreTiming");
            restored = putBackTiming(saved, device.steps());
        };
        (void)plan;
        return s;
    }
};

// R3 (recheck4): a still is in flight (the camera has its input and a photo
// output, no video) and a stream is asked for and refused. Only the video
// output the call added is removed.
void aRefusedStreamRemovesOnlyTheOutputItAdded()
{
    FakeSession session;
    session.input = session.photo = true;
    session.refuseVideo = true;
    const AdmitPlan plan {.addInput = false, .activate = true, .addVideo = true, .addPhoto = false};
    require(admit(plan, session.steps(plan)) == Admission::videoRefused, "refused");
    require(session.input && session.photo && !session.video, "the input and the photo output of the still in flight stay");
    require(session.log == std::vector<std::string> {"begin", "snapshot", "activate", "addVideo", "removeVideo", "restoreTiming", "commit"},
            "begin, the durations taken, the rate, the refused video output, its removal, the durations back, commit: no removal of the photo "
            "output or the input");
    require(session.begins == session.commits, "one configuration, closed");
}

void aRefusedStreamBeforeItsInputLeavesTheSessionAsItWas()
{
    FakeSession session;
    session.refuseVideo = true;
    const AdmitPlan plan {.addInput = true, .activate = true, .addVideo = true, .addPhoto = false};
    require(admit(plan, session.steps(plan)) == Admission::videoRefused, "refused");
    require(!session.input && !session.video && !session.photo, "the input this call added left again");
    require(session.log == std::vector<std::string> {"begin", "snapshot", "addInput", "activate", "addVideo", "removeVideo", "removeInput",
                                                      "restoreTiming", "commit"},
            "the timing is taken before the input is added, and put back after it has left");
}

void aRefusedInputOrFormatAddsNothingElse()
{
    FakeSession refused;
    refused.refuseInput = true;
    const AdmitPlan plan {.addInput = true, .activate = true, .addVideo = true, .addPhoto = true};
    require(admit(plan, refused.steps(plan)) == Admission::inputRefused &&
                refused.log == std::vector<std::string> {"begin", "snapshot", "addInput", "restoreTiming", "commit"},
            "a refused input stops the transaction before anything else runs, and leaves the device as it was");
    FakeSession format;
    format.refuseActivate = true;
    require(admit(plan, format.steps(plan)) == Admission::formatRefused && !format.input && !format.video && !format.photo,
            "a refused format takes the input it added out");
    require(format.log == std::vector<std::string> {"begin", "snapshot", "addInput", "activate", "removeInput", "restoreTiming", "commit"},
            "and nothing else");
    FakeSession kept;
    kept.input = true;
    kept.refuseActivate = true;
    const AdmitPlan existing {.addInput = false, .activate = true, .addVideo = true, .addPhoto = false};
    require(admit(existing, kept.steps(existing)) == Admission::formatRefused && kept.input &&
                kept.log == std::vector<std::string> {"begin", "snapshot", "activate", "restoreTiming", "commit"},
            "a camera that was in the session stays in it");
}

void aRefusedPhotoOutputRemovesTheVideoAndInputItAdded()
{
    FakeSession session;
    session.refusePhoto = true;
    const AdmitPlan plan {.addInput = true, .activate = true, .addVideo = true, .addPhoto = true};
    require(admit(plan, session.steps(plan)) == Admission::photoRefused, "refused");
    require(!session.input && !session.video && !session.photo, "everything this call added left");
    require(session.log == std::vector<std::string> {"begin", "snapshot", "addInput", "activate", "addVideo", "addPhoto", "removePhoto", "removeVideo",
                                                      "removeInput", "restoreTiming", "commit"},
            "the newest first, inside the one configuration");
    // A photo output refused for a camera that streams leaves the stream alone.
    FakeSession streaming;
    streaming.input = streaming.video = true;
    streaming.refusePhoto = true;
    const AdmitPlan still {.addInput = false, .activate = false, .addVideo = false, .addPhoto = true};
    require(admit(still, streaming.steps(still)) == Admission::photoRefused && streaming.input && streaming.video,
            "a refused still leaves the stream as it was");
    require(streaming.log == std::vector<std::string> {"begin", "addPhoto", "removePhoto", "commit"},
            "and, as it changes no durations, takes and puts back none");
}

void aSessionOverItsBudgetRemovesWhatTheJoinAdded()
{
    FakeSession session;
    session.overBudget = true;
    const AdmitPlan plan {.addInput = true, .activate = true, .addVideo = true, .addPhoto = false};
    require(admit(plan, session.steps(plan)) == Admission::overBudget, "refused after the commit");
    require(!session.input && !session.video, "the join is undone");
    require(session.begins == 2 && session.commits == 2, "inside a configuration of its own");
    require(session.log == std::vector<std::string> {"begin", "snapshot", "addInput", "activate", "addVideo", "commit", "begin", "removeVideo",
                                                      "removeInput", "restoreTiming", "commit"},
            "the first configuration commits, the second undoes it");
    FakeSession admitted;
    require(admit(plan, admitted.steps(plan)) == Admission::admitted && admitted.input && admitted.video && admitted.begins == 1,
            "a join within budget stays");
}

// R4 (recheck5): a still-only camera is in the session at 1/30 s, a long manual exposure having made the maximum 1/10 s,
// and a 15 fps stream is asked for. Whatever refuses it (the video output, the photo output, a hardware budget, or the
// device partway through its durations), the device holds the very durations it held before, not the 1/15 s the
// request programmed and not the 1/30 s a rate would recompute.
void aRefusedStreamOnACameraInTheSessionPutsItsDurationsBack()
{
    const auto before = [] {
        FakeDevice device;
        device.durations(FakeDevice::fps(30), FakeDevice::fps(10));
        return device.timing;
    }();
    const AdmitPlan plan {.addInput = false, .activate = true, .addVideo = true, .addPhoto = false};
    const auto session = [&] {
        FakeSession fake;
        fake.input = true;
        fake.device.timing = before;
        return fake;
    };
    auto video = session();
    video.photo = true;
    video.refuseVideo = true;
    require(admit(plan, video.steps(plan)) == Admission::videoRefused && video.device.timing == before, "a refused video output");
    auto photo = session();
    photo.refusePhoto = true;
    const AdmitPlan both {.addInput = false, .activate = true, .addVideo = true, .addPhoto = true};
    require(admit(both, photo.steps(both)) == Admission::photoRefused && photo.device.timing == before && photo.input && !photo.video,
            "a refused photo output, the video output it added removed");
    auto budget = session();
    budget.overBudget = true;
    require(admit(plan, budget.steps(plan)) == Admission::overBudget && budget.device.timing == before && budget.input && !budget.video,
            "a session over its hardware budget");
    auto partway = session();
    partway.refuseActivate = true;
    require(admit(plan, partway.steps(plan)) == Admission::formatRefused && partway.device.timing == before &&
                partway.device.timing.minimum != FakeDevice::fps(15),
            "a device that took the minimum duration and refused the maximum");
    // A join that succeeds keeps what it programmed.
    auto admitted = session();
    require(admit(plan, admitted.steps(plan)) == Admission::admitted && admitted.device.timing.minimum == FakeDevice::fps(15) &&
                admitted.device.timing.maximum == FakeDevice::fps(15),
            "a stream that is admitted runs at its rate");
}

// R4 (recheck6): the same camera has a custom exposure of 1/10 s (ISO 400). Shortening the maximum duration to 1/15 s
// shortens the exposure with it, so the durations put back do not put the exposure back: it is part of what a refusal
// restores, including the refusal that comes after the configuration is committed (a hardware budget). The fake
// device clamps as the platform does, so a restore that put back only the pair fails here.
void aRefusedStreamPutsTheManualExposureBackToo()
{
    FakeDevice start;
    start.durations(FakeDevice::fps(30), FakeDevice::fps(10));
    start.customExposure(FakeDevice::fps(10), 400.0f);
    const DeviceTiming before = start.timing;
    const AdmitPlan plan {.addInput = false, .activate = true, .addVideo = true, .addPhoto = false};
    const AdmitPlan both {.addInput = false, .activate = true, .addVideo = true, .addPhoto = true};
    const auto session = [&] {
        FakeSession fake;
        fake.input = true;
        fake.device.timing = before;
        return fake;
    };
    auto admitted = session();
    require(admit(plan, admitted.steps(plan)) == Admission::admitted && admitted.device.timing.exposure.duration == FakeDevice::fps(15),
            "the platform does shorten the exposure to the new maximum (the fake models it)");
    auto video = session();
    video.refuseVideo = true;
    require(admit(plan, video.steps(plan)) == Admission::videoRefused && video.device.timing == before && video.restored,
            "a refused video output: durations and exposure");
    auto photo = session();
    photo.refusePhoto = true;
    require(admit(both, photo.steps(both)) == Admission::photoRefused && photo.device.timing == before, "a refused photo output");
    auto budget = session();
    budget.overBudget = true;
    require(admit(plan, budget.steps(plan)) == Admission::overBudget && budget.device.timing == before,
            "the committed configuration over its budget: the exposure comes back after the commit");
    auto partway = session();
    partway.refuseActivate = true;
    require(admit(plan, partway.steps(plan)) == Admission::formatRefused && partway.device.timing == before,
            "a refusal between the minimum and the maximum");
    // A device that was not in a custom exposure has none to put back, and none is written.
    FakeSession automatic;
    automatic.input = true;
    automatic.device.durations(FakeDevice::fps(30), FakeDevice::fps(10));
    automatic.refuseVideo = true;
    automatic.device.timing.exposure = {false, {1, 50}, 100.0f};
    // An automatic exposure follows the scene while the call runs: what it reads then is not the device's to be put back,
    // and writing it would switch the device to a custom exposure.
    automatic.meanwhile = [](FakeDevice& device) { device.timing.exposure = {false, {1, 60}, 160.0f}; };
    require(admit(plan, automatic.steps(plan)) == Admission::videoRefused && !automatic.device.timing.exposure.custom &&
                automatic.device.timing.exposure.duration == CameraTime {1, 60} &&
                std::find(automatic.device.writes.begin(), automatic.device.writes.end(), "exposure") == automatic.device.writes.end(),
            "an automatic exposure is left alone: it is never switched to a custom one");
    // The ISO is put back as it was held, not as a default.
    FakeSession iso;
    iso.input = true;
    iso.device.durations(FakeDevice::fps(30), FakeDevice::fps(10));
    iso.device.customExposure({1, 8}, 1250.0f);
    iso.device.timing.maximum = {1, 8};
    iso.refuseVideo = true;
    const auto isoBefore = iso.device.timing;
    require(admit(plan, iso.steps(plan)) == Admission::videoRefused && iso.device.timing == isoBefore && iso.device.timing.exposure.iso == 1250.0f,
            "mode, duration and ISO as held");
}

// R4 (recheck6): the device is in another format, at its own durations, before its first join. Adding the input resets
// the durations, so the snapshot is taken before the input; the camera is then put in its anchor, and a refusal leaves
// the device as it was before the join, in its format too, once the input has gone.
void aRefusedFirstJoinPutsTheDeviceBackAsItWasBeforeTheJoin()
{
    FakeDevice start;
    start.durations(FakeDevice::fps(24), FakeDevice::fps(12));
    const DeviceTiming before = start.timing;
    require(before.format != 1, "the device is in another format than the anchor's");
    const AdmitPlan plan {.addInput = true, .activate = true, .addVideo = true, .addPhoto = true};
    const auto first = [&] {
        FakeSession fake;
        fake.device.timing = before;
        return fake;
    };
    auto video = first();
    video.refuseVideo = true;
    require(admit(plan, video.steps(plan)) == Admission::videoRefused && video.device.timing == before && video.restored && !video.input,
            "a refused video output: the format and the durations from before the join, which the input had reset");
    auto photo = first();
    photo.refusePhoto = true;
    require(admit(plan, photo.steps(plan)) == Admission::photoRefused && photo.device.timing == before, "a refused photo output");
    auto budget = first();
    budget.overBudget = true;
    require(admit(plan, budget.steps(plan)) == Admission::overBudget && budget.device.timing == before, "over the budget");
    auto format = first();
    format.refuseActivate = true;
    require(admit(plan, format.steps(plan)) == Admission::formatRefused && format.device.timing == before, "a refused anchor");
    auto input = first();
    input.refuseInput = true;
    require(admit(plan, input.steps(plan)) == Admission::inputRefused && input.device.timing == before, "a refused input");
    // A device that cannot be set back says so: nothing is claimed that was not done.
    auto stuck = first();
    stuck.refuseVideo = true;
    stuck.device.refuseFormat = true;
    require(admit(plan, stuck.steps(plan)) == Admission::videoRefused && !stuck.restored && stuck.device.timing.format == 1,
            "a refused format restore is reported");
}

// R4 (recheck6): putBackTiming writes in the order that never breaks the platform's rule that the minimum does not
// exceed the maximum, writes the exposure last and only when it is needed, and stops at the first refusal.
void putBackTimingOrdersItsWritesAndStopsAtARefusal()
{
    FakeDevice slow;  // the held minimum is above the maximum the device has now: the maximum goes first
    slow.durations(FakeDevice::fps(60), FakeDevice::fps(60));
    DeviceTiming slower = slow.timing;
    slower.minimum = FakeDevice::fps(30);
    slower.maximum = FakeDevice::fps(10);
    require(putBackTiming(slower, slow.steps()) && slow.timing == slower && slow.writes == std::vector<std::string> {"maximum", "minimum"},
            "a minimum above the current maximum waits for the maximum to move");
    FakeDevice fast;  // the held minimum is within the maximum the device has now: the minimum goes first
    fast.durations(FakeDevice::fps(30), FakeDevice::fps(10));
    DeviceTiming faster = fast.timing;
    faster.minimum = FakeDevice::fps(60);
    faster.maximum = FakeDevice::fps(60);
    require(putBackTiming(faster, fast.steps()) && fast.timing == faster && fast.writes == std::vector<std::string> {"minimum", "maximum"},
            "a minimum within the current maximum is written first, so the maximum never goes below it");
    FakeDevice unchanged;
    require(putBackTiming(unchanged.timing, unchanged.steps()) && unchanged.locks == 1 && unchanged.unlocks == 1,
            "locked once and unlocked once");
    FakeDevice locked;
    locked.refuseLock = true;
    DeviceTiming other = locked.timing;
    other.minimum = FakeDevice::fps(15);
    require(!putBackTiming(other, locked.steps()) && locked.writes.empty() && locked.unlocks == 0, "a device that will not lock is not written");
    FakeDevice exposure;
    exposure.durations(FakeDevice::fps(30), FakeDevice::fps(10));
    exposure.customExposure(FakeDevice::fps(10), 100.0f);
    DeviceTiming held = exposure.timing;
    exposure.timing.maximum = FakeDevice::fps(15);
    exposure.clamp();
    exposure.refuseExposure = true;
    require(!putBackTiming(held, exposure.steps()) && exposure.unlocks == 1 && exposure.timing.maximum == held.maximum,
            "a refused exposure is false, after the durations, and the device is unlocked");
    FakeDevice refusedFormat;
    DeviceTiming otherFormat = refusedFormat.timing;
    otherFormat.format = 3;
    refusedFormat.refuseFormat = true;
    require(!putBackTiming(otherFormat, refusedFormat.steps()) && refusedFormat.writes.empty() && refusedFormat.unlocks == 1,
            "nothing after a refused format is written");
}

struct FakeStream {
    FakeDevice device;
    DeviceTiming saved;
    int size {640};
    bool refuseDurations {}, refuseSize {};
    std::vector<std::string> log;
    FakeStream()
    {
        device.durations(FakeDevice::fps(30), FakeDevice::fps(10));
        device.customExposure(FakeDevice::fps(10), 400.0f);
    }
    ResizeSteps steps(bool rate, bool dimension)
    {
        ResizeSteps s;
        s.begin = [this] { log.push_back("begin"); };
        s.commit = [this] { log.push_back("commit"); };
        if (rate)
            s.snapshotTiming = [this] {
                log.push_back("snapshot");
                saved = device.timing;
            };
        if (rate)
            s.setDurations = [this] {
                log.push_back("durations");
                device.timing.minimum = FakeDevice::fps(15);
                if (refuseDurations) return false;
                device.timing.maximum = FakeDevice::fps(15);
                device.clamp();
                return true;
            };
        if (dimension)
            s.setVideoSize = [this] {
                log.push_back("size");
                if (refuseSize) return false;
                size = 1280;
                return true;
            };
        s.restoreTiming = [this] {
            log.push_back("restore");
            (void)putBackTiming(saved, device.steps());
        };
        return s;
    }
};

void aResizeSetsTheDurationsThenTheSizeAndRestoresTheOldTimingOnARefusal()
{
    const FakeStream fresh;
    const DeviceTiming before = fresh.device.timing;  // a long manual exposure made the maximum longer than the rate asks
    FakeStream ok;
    require(resizeStream(ok.steps(true, true)) == Resizing::resized && ok.device.timing.minimum == FakeDevice::fps(15) &&
                ok.device.timing.maximum == FakeDevice::fps(15) && ok.size == 1280,
            "both change");
    require(ok.log == std::vector<std::string> {"begin", "snapshot", "durations", "size", "commit"}, "the durations first");
    FakeStream sizeRefused;
    sizeRefused.refuseSize = true;
    require(resizeStream(sizeRefused.steps(true, true)) == Resizing::sizeRefused && sizeRefused.device.timing == before,
            "a refused size puts the device's own timing back, exposure included, not what a rate would program");
    require(sizeRefused.log == std::vector<std::string> {"begin", "snapshot", "durations", "size", "restore", "commit"}, "in the same configuration");
    FakeStream durationsRefused;
    durationsRefused.refuseDurations = true;
    require(resizeStream(durationsRefused.steps(true, true)) == Resizing::durationsRefused && durationsRefused.size == 640 &&
                durationsRefused.device.timing == before &&
                durationsRefused.log == std::vector<std::string> {"begin", "snapshot", "durations", "restore", "commit"},
            "a refused rate leaves the size alone, and the timing as it was after the minimum had changed");
    FakeStream sizeOnly;
    sizeOnly.refuseSize = true;
    require(resizeStream(sizeOnly.steps(false, true)) == Resizing::sizeRefused &&
                sizeOnly.log == std::vector<std::string> {"begin", "size", "commit"},
            "a resize that does not change the rate has no timing to restore");
    FakeStream rateOnly;
    require(resizeStream(rateOnly.steps(true, false)) == Resizing::resized && rateOnly.size == 640, "a rate alone");
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

// R5 (recheck5): a fake device that records the order of what recovery does.
struct FakeRecovery {
    std::vector<std::string> log;
    std::set<std::size_t> moved;    // the cameras the system moved out of their anchors
    std::set<std::size_t> refused;  // the cameras whose repair the device refuses
    RecoverySteps steps()
    {
        RecoverySteps s;
        s.outOfAnchor = [this](std::size_t camera) { return moved.contains(camera); };
        s.repair = [this](std::size_t camera) {
            log.push_back("repair " + std::to_string(camera));
            if (refused.contains(camera)) return false;
            moved.erase(camera);
            return true;
        };
        s.announce = [this](Recovery outcome) { log.push_back(outcome == Recovery::resumed ? "resumed" : "failed"); };
        return s;
    }
};

void everyCameraIsRepairedBeforeTheSessionIsToldItIsBack()
{
    const std::size_t cameras[] {0, 1, 2};
    FakeRecovery none;
    require(recoverAnchors(cameras, none.steps()) == Recovery::resumed && none.log == std::vector<std::string> {"resumed"},
            "cameras in their anchors need no repair, and the session is back");
    FakeRecovery two;
    two.moved = {0, 2};
    require(recoverAnchors(cameras, two.steps()) == Recovery::resumed &&
                two.log == std::vector<std::string> {"repair 0", "repair 2", "resumed"},
            "every camera out of its anchor is put back, then the session is told");
    FakeRecovery empty;
    require(recoverAnchors({}, empty.steps()) == Recovery::resumed && empty.log == std::vector<std::string> {"resumed"}, "no camera, no repair");
}

void aRefusedRepairIsAFailureAndNothingIsAnnouncedAsBack()
{
    const std::size_t cameras[] {0, 1, 2};
    FakeRecovery first;
    first.moved = {0, 1};
    first.refused = {0};
    require(recoverAnchors(cameras, first.steps()) == Recovery::failed && first.log == std::vector<std::string> {"repair 0", "failed"},
            "the first refusal ends the recovery: failed, with no resumed");
    require(first.moved.contains(1), "no further repair is attempted in a session that is being closed");
    FakeRecovery later;
    later.moved = {0, 2};
    later.refused = {2};
    require(recoverAnchors(cameras, later.steps()) == Recovery::failed &&
                later.log == std::vector<std::string> {"repair 0", "repair 2", "failed"},
            "a refusal after a success is a failure too, and no camera is announced as back");
    for (const auto& entry : {first.log, later.log})
        require(std::find(entry.begin(), entry.end(), "resumed") == entry.end(), "never resumed");
}

// ---------------------------------------------------------------------------
// Pressure
// ---------------------------------------------------------------------------

// R1 (recheck5): the pressure observer's handler and the work it queues for the session's queue hold what they use.
// The session's queue is drained when the session ends, but a notification that arrives during the drain queues its
// work behind it, to run after the session, its anchors and its running cameras are gone. This is the glue's shape in
// the glue's own medium, Objective-C blocks: the owner `session` (RunningCamera::watch), the observer's `handler` and
// the queued work. `begin` takes the watch by value, so the blocks copy it from a local; a handler that named a
// reference parameter (the old `const std::shared_ptr<const Anchor>&`, which was the session's element) would read
// freed memory when the late work runs, which AddressSanitizer reports.
struct PressureRig {
    using Handler = void (^)(std::uint32_t);
    using Work = void (^)();
    std::unique_ptr<std::shared_ptr<PressureWatch>> session;
    std::vector<Work> queue;                 // the session's queue
    std::vector<ProgrammedRate> programmed;  // what reached the device
    Handler handler {};

    explicit PressureRig(const VideoFormatFacts& facts)
        : session(std::make_unique<std::shared_ptr<PressureWatch>>(std::make_shared<PressureWatch>(facts, offeredRatesOf(facts, videoRateCap))))
    {
        (*session)->request(30);
        begin(*session);
    }
    ~PressureRig()
    {
        drain();
        if (handler) Block_release(handler);
    }
    PressureRig(const PressureRig&) = delete;
    PressureRig& operator=(const PressureRig&) = delete;

    void begin(std::shared_ptr<PressureWatch> watch)
    {
        PressureRig* const self = this;
        handler = Block_copy(^(std::uint32_t cap) {
            if (!watch->pressure(cap)) return;
            self->queue.push_back(Block_copy(^{
                if (const auto rate = watch->programmed()) self->programmed.push_back(*rate);
            }));
        });
    }
    void drain()
    {
        auto work = std::move(queue);
        queue.clear();
        for (auto item : work) {
            item();
            Block_release(item);
        }
    }
};

void aPressureNotificationIsAppliedWhileTheStreamRunsAndNeverAfterIt()
{
    const VideoFormatFacts facts = fmt(1920, 1440, {{4032, 3024}}, {{1, 60}});
    {
        PressureRig rig(facts);
        rig.handler(20);
        rig.drain();
        require(rig.programmed.size() == 1 && rig.programmed.back().rate == 20, "serious pressure caps the stream at 20 fps");
        rig.handler(20);
        require(rig.queue.empty(), "the same cap again changes nothing and queues nothing");
        rig.handler(15);
        rig.handler(0);
        rig.drain();
        require(rig.programmed.size() == 3 && rig.programmed[1].rate == 30 && rig.programmed[2].rate == 30,
                "each change is programmed from the cap the work finds when it runs: both found the pressure gone");
        rig.handler(15);
        rig.drain();
        require(rig.programmed.size() == 4 && rig.programmed.back().rate == 15, "and 15 fps under critical pressure");
    }
    {
        // The notification that arrives during the session's final drain: its work runs after the session is gone.
        PressureRig rig(facts);
        rig.handler(15);
        require(rig.queue.size() == 1, "the work is queued behind the session's drain");
        (*rig.session)->end();  // stop() ends the watches inside the queue...
        rig.session->reset();    // ...and the session lets go of its own (the anchors_ element, null from now on)
        rig.drain();             // the late work runs now: through a reference to that element it would find null
        rig.session.reset();     // and the session's storage goes
        require(rig.programmed.empty(), "it programs nothing: the stream is over");
        rig.handler(20);
        require(rig.queue.empty(), "and a notification after the end queues nothing");
    }
    {
        // Ended before a cap change that was already queued, with the session still there.
        PressureRig rig(facts);
        rig.handler(20);
        (*rig.session)->end();
        require(!(*rig.session)->live() && (*rig.session)->cap() == 0 && !(*rig.session)->programmed().has_value(), "an ended watch programs nothing");
        rig.drain();
        require(rig.programmed.empty(), "so the queued work does nothing");
    }
}

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

void aRefusalIsLoggedOncePerCameraAndReasonUntilTheCameraChanges()
{
    RefusalLog log;
    require(log.first(0, Refusal::stillNotMade), "the first refusal is logged");
    for (int again = 0; again < 100; ++again) require(!log.first(0, Refusal::stillNotMade), "the same refusal is not");
    require(log.first(0, Refusal::overBudget), "another reason is");
    require(log.first(1, Refusal::stillNotMade), "another camera is");
    log.reset(0);
    require(log.first(0, Refusal::stillNotMade) && log.first(0, Refusal::overBudget), "a reset camera logs again");
    require(!log.first(1, Refusal::stillNotMade), "and another camera is not reset with it");
    for (const auto reason : {Refusal::noAnchor, Refusal::formatChanged, Refusal::notJoinable, Refusal::inputRefused, Refusal::outputRefused,
                              Refusal::overBudget, Refusal::stillNotMade, Refusal::rawUnavailable, Refusal::raised})
        require(!refusalName(reason).empty(), "every reason has a name");
}

// ---------------------------------------------------------------------------
// Generated tables
// ---------------------------------------------------------------------------

struct Rng {
    std::uint64_t state;
    std::uint64_t next()
    {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : std::uint32_t(next() % n); }
    bool chance(std::uint32_t percent) { return below(100) < percent; }
    template<typename T> const T& pick(const std::vector<T>& values) { return values[below(std::uint32_t(values.size()))]; }
};

std::vector<VideoFormatFacts> generateTable(Rng& rng)
{
    static const std::vector<std::pair<std::uint32_t, std::uint32_t>> bases {
        {160, 120}, {320, 240}, {640, 480}, {1280, 720}, {1440, 1080}, {1600, 900}, {1600, 1200}, {1920, 1080}, {1920, 1440}, {1920, 1920},
        {2016, 1512}, {2560, 1440}, {3840, 2160}, {4032, 3024}, {1088, 1088}, {1536, 1536}, {3000, 2000}, {1921, 1081}, {4224, 2376}};
    static const std::vector<CameraStillSize> photoSizes {{4032, 3024}, {4224, 2376}, {8064, 6048}, {2016, 1512}, {1920, 1080}, {1280, 720},
                                                          {3088, 2316}, {640, 480}, {5712, 4284}, {1920, 1440}};
    static const std::vector<std::uint32_t> rates {10, 15, 24, 25, 30, 60, 120, 240};
    std::vector<VideoFormatFacts> formats;
    const std::uint32_t count = 1 + rng.below(12);
    for (std::uint32_t index = 0; index < count; ++index) {
        VideoFormatFacts format;
        if (rng.chance(25)) {
            const std::uint32_t width = 160 + 2 * rng.below(1936);
            static const std::vector<std::pair<std::uint32_t, std::uint32_t>> aspects {{1, 1}, {16, 9}, {4, 3}, {3, 2}, {5, 4}, {21, 9}};
            const auto aspect = rng.pick(aspects);
            format.width = width;
            format.height = std::max<std::uint32_t>(16, width * aspect.second / aspect.first);
        } else {
            const auto size = rng.pick(bases);
            format.width = size.first;
            format.height = size.second;
        }
        const std::uint32_t kind = rng.below(7);
        const std::uint32_t k = rng.pick(rates);
        switch (kind) {
        case 0: format.frameRates = {{1, double(k)}}; break;
        case 1: format.frameRates = {{double(k), double(k)}}; break;
        case 2: format.frameRates = {{1, 10}, {50, 60}}; break;
        case 3: format.frameRates = {{k * 1000.0 / 1001.0, k * 1000.0 / 1001.0}}; break;
        case 4: format.frameRates = {{k + 3e-5, k + 3e-5}}; break;
        case 5: format.frameRates = {{1, double(k)}, {double(k) + 1, 2.0 * k}}; break;
        default: format.frameRates = rng.chance(10) ? Spans {{7.5, 7.9}} : Spans {{double(k), double(k) * 2}}; break;
        }
        const std::uint32_t photos = rng.below(4);
        for (std::uint32_t photo = 0; photo < photos; ++photo) format.photoSizes.push_back(rng.pick(photoSizes));
        format.fullRange = rng.chance(60);
        format.multiCamera = rng.chance(50);
        formats.push_back(std::move(format));
    }
    return formats;
}

// The rules, restated from the model as a person would read it, to compare with
// the implementation. A whole rate is a number the span holds, or is within
// 0.15 % of.
bool refWholeRate(const CameraRateSpan& span, std::uint32_t n)
{
    const double tolerance = 0.0015 * n;
    return (n >= span.minimum && n <= span.maximum) || std::fabs(n - span.minimum) <= tolerance || std::fabs(n - span.maximum) <= tolerance;
}

std::vector<std::uint32_t> refRates(const VideoFormatFacts& format)
{
    std::vector<std::uint32_t> rates;
    for (std::uint32_t n = 1; n <= 600; ++n)
        if (std::any_of(format.frameRates.begin(), format.frameRates.end(), [&](const CameraRateSpan& span) { return refWholeRate(span, n); }))
            rates.push_back(n);
    return rates;
}

Sizes refStills(const VideoFormatFacts& format, std::uint64_t cap)
{
    Sizes within;
    for (const auto& size : format.photoSizes)
        if (size.width != 0 && size.height != 0 && (cap == 0 || std::uint64_t(size.width) * size.height <= cap)) within.push_back(size);
    if (within.empty()) return {};
    const auto largest = *std::max_element(within.begin(), within.end(), [](const CameraStillSize& a, const CameraStillSize& b) {
        const auto areaA = std::uint64_t(a.width) * a.height, areaB = std::uint64_t(b.width) * b.height;
        return areaA != areaB ? areaA < areaB : a.width < b.width;
    });
    Sizes stills;
    for (const auto& size : within)
        if (size.width <= largest.width && size.height <= largest.height && std::find(stills.begin(), stills.end(), size) == stills.end())
            stills.push_back(size);
    return stills;
}

// What the platform delivers for a stream, from first principles: the smallest even multiple of the format's
// reduced aspect ratio (found by counting up) that covers the request, else the format's own size.
FrameSize refDelivered(const VideoFormatFacts& format, std::uint32_t width, std::uint32_t height)
{
    const std::uint32_t common = std::gcd(format.width, format.height);
    const std::uint32_t ratioWidth = format.width / common, ratioHeight = format.height / common;
    for (std::uint32_t multiple = 1; multiple < common; ++multiple) {
        const std::uint32_t w = multiple * ratioWidth, h = multiple * ratioHeight;
        if (w % 2 != 0 || h % 2 != 0) continue;
        if (w >= width && h >= height) return {w, h};
    }
    return {format.width, format.height};
}

// The buffer the reserve counts for a frame, from first principles: every row padded up to a multiple of 256
// bytes, one luma plane and a half-height chroma plane.
std::uint64_t refBufferBytes(FrameSize frame)
{
    std::uint64_t pitch = 0;
    while (pitch < frame.width) pitch += rowAlignment;
    return pitch * frame.height + pitch * ((frame.height + 1) / 2);
}

using RefKey = std::tuple<bool, std::uint32_t, std::uint64_t, std::uint32_t, std::uint64_t, std::int64_t, std::int64_t, bool>;

// The key of §1, computed from the facts by the brute-force rules above.
std::optional<RefKey> refKey(const VideoFormatFacts& format, const OfferRules& rules)
{
    const auto rates = refRates(format);
    if (rates.empty()) return std::nullopt;
    std::vector<FrameSize> kept;
    std::uint32_t boxWidth = 0, boxHeight = 0;
    auto candidates = rules.candidateSizes;
    std::sort(candidates.begin(), candidates.end(), [](const FrameSize& a, const FrameSize& b) {
        const auto areaA = std::uint64_t(a.width) * a.height, areaB = std::uint64_t(b.width) * b.height;
        return areaA != areaB ? areaA > areaB : a.width > b.width;
    });
    for (const auto& size : candidates) {
        if (size.width < 16 || size.height < 16 || size.width % 2 != 0 || size.height % 2 != 0) continue;
        if (size.width > rules.maximumVideoWidth || size.height > rules.maximumVideoHeight) continue;
        if (size.width > format.width || size.height > format.height) continue;
        if (std::find(kept.begin(), kept.end(), size) != kept.end()) continue;
        const auto w = std::max(boxWidth, size.width), h = std::max(boxHeight, size.height);
        if (refBufferBytes(refDelivered(format, w, h)) > rules.limits.maximumFrameBytes) continue;
        kept.push_back(size);
        boxWidth = w;
        boxHeight = h;
    }
    if (kept.empty()) return std::nullopt;
    const auto cap = rules.rateCap;
    std::uint32_t best = 0;
    for (const auto n : rates)
        if (n <= cap) best = std::max(best, n);
    const auto stills = refStills(format, rules.limits.maximumStillPixels);
    std::uint64_t stillArea = 0;
    std::uint32_t stillWidth = 0;
    for (const auto& size : stills) {
        const auto area = std::uint64_t(size.width) * size.height;
        if (area > stillArea || (area == stillArea && size.width > stillWidth)) {
            stillArea = area;
            stillWidth = size.width;
        }
    }
    return RefKey {best != 0, best, stillArea, stillWidth, std::uint64_t(boxWidth) * boxHeight, -std::int64_t(std::uint64_t(format.width) * format.height),
                   -std::int64_t(rates.front()), format.fullRange};
}

void everyGeneratedTableHoldsTheInvariants()
{
    constexpr std::uint64_t seed = 20261113;
    constexpr int tables = 20000;
    Rng rng {seed};
    std::uint64_t anchors = 0, withoutAnchor = 0, multiAnchors = 0, requestsChecked = 0;
    for (int table = 0; table < tables; ++table) {
        const std::string where = " (seed " + std::to_string(seed) + ", table " + std::to_string(table) + ")";
        const auto formats = generateTable(rng);
        for (const bool large : {false, true}) {
            const OfferRules rules = large ? largeRules(formats) : smallRules(formats);
            const std::uint64_t bound = rules.limits.maximumFrameBytes;
            const std::uint64_t cap = rules.limits.maximumStillPixels;
            for (const bool multiOnly : {false, true}) {
                const auto index = chooseAnchor(formats, rules, multiOnly);
                // P6: the anchor is the best candidate by the key of the model, the lowest index among equals.
                std::optional<std::size_t> expected;
                std::optional<RefKey> expectedKey;
                for (std::size_t i = 0; i < formats.size(); ++i) {
                    if (multiOnly && !formats[i].multiCamera) continue;
                    const auto key = refKey(formats[i], rules);
                    if (key && (!expectedKey || *key > *expectedKey)) {
                        expected = i;
                        expectedKey = key;
                    }
                }
                require(index == expected, "P6: the anchor is the best-ranked candidate" + where);
                // P5: a second call, and the table in another order, give the same key.
                require(chooseAnchor(formats, rules, multiOnly) == index, "P5: determinism" + where);
                if (formats.size() > 1) {
                    auto shuffled = formats;
                    for (std::size_t i = shuffled.size() - 1; i > 0; --i) std::swap(shuffled[i], shuffled[rng.below(std::uint32_t(i + 1))]);
                    const auto other = chooseAnchor(shuffled, rules, multiOnly);
                    require(other.has_value() == index.has_value(), "P5: the same candidates in another order" + where);
                    if (index) require(refKey(shuffled[*other], rules) == refKey(formats[*index], rules), "P5: an anchor of an equal key" + where);
                }
                if (!index) {
                    // P4: no candidate offers no camera.
                    ++withoutAnchor;
                    continue;
                }
                ++anchors;
                multiAnchors += multiOnly;
                const auto& anchor = formats[*index];
                require(!multiOnly || anchor.multiCamera, "P7: a multi-camera anchor is a multi-camera format" + where);
                const auto offer = offerOf(anchor, rules);
                // P4: an offered camera has a video size and a rate.
                require(!offer.videoSizes.empty() && !offer.rates.empty(), "P4: an anchor has a video mode and a rate" + where);
                // P2: every offered still is made, within the cap.
                for (const auto& still : offer.stills) {
                    require(formatMakesPhoto(anchor, still, cap), "P2: every offered still is made" + where);
                    require(cap == 0 || std::uint64_t(still.width) * still.height <= cap, "P2: within the cap" + where);
                }
                require(offer.stills == refStills(anchor, cap) || [&] {
                    auto a = offer.stills, b = refStills(anchor, cap);
                    std::sort(a.begin(), a.end(), [](auto x, auto y) { return std::tie(x.width, x.height) < std::tie(y.width, y.height); });
                    std::sort(b.begin(), b.end(), [](auto x, auto y) { return std::tie(x.width, x.height) < std::tie(y.width, y.height); });
                    return a == b;
                }(), "P2: the stills are those the model lists" + where);
                // P3: the delivered frame at the box is within the bound, and grows with the request.
                require(bound == 0 || refBufferBytes(refDelivered(anchor, offer.box.width, offer.box.height)) <= bound,
                        "P3: the box's capture buffer is within the reserved one" + where);
                require(deliveredFrameBytes(anchor, offer.box.width, offer.box.height, rowAlignment) ==
                            refBufferBytes(refDelivered(anchor, offer.box.width, offer.box.height)),
                        "P3: the model's byte count is the buffer's" + where);
                // P1: every admitted run (a request inside the box, at an offered rate) runs in the anchor.
                std::vector<std::uint32_t> offered;
                for (const auto& range : offer.rates)
                    for (const auto rate : {range.minimum, range.maximum, range.minimum + (range.maximum - range.minimum) / 2}) offered.push_back(rate);
                for (const auto rate : offered) {
                    const auto setting = rateSettingFor(anchor, rate);
                    require(setting.has_value(), "P1: every offered rate can be programmed" + where);
                    if (setting) {
                        const auto& span = anchor.frameRates[setting->span];
                        const double duration = programmedFrameDuration(span, setting->duration, rate);
                        // P8: the duration lies in the span and is within 0.15 % of the nominal one.
                        require(duration >= 1.0 / span.maximum - 1e-12 && duration <= 1.0 / span.minimum + 1e-12, "P8: the duration is the span's to give" + where);
                        require(std::fabs(duration * rate - 1.0) <= 0.0015 + 1e-9, "P8: within 0.15 % of 1 / rate" + where);
                    }
                }
                for (int draw = 0; draw < 4; ++draw) {
                    // A request of one to three streams, each inside some offered size.
                    std::uint32_t w = 0, h = 0;
                    for (std::uint32_t stream = 0, n = 1 + rng.below(3); stream < n; ++stream) {
                        const auto& size = offer.videoSizes[rng.below(std::uint32_t(offer.videoSizes.size()))];
                        const std::uint32_t sw = std::max<std::uint32_t>(16, (size.width - rng.below(size.width / 2 + 1)) & ~1u);
                        const std::uint32_t sh = std::max<std::uint32_t>(16, (size.height - rng.below(size.height / 2 + 1)) & ~1u);
                        w = std::max(w, rng.chance(50) ? size.width : std::min(sw, size.width));
                        h = std::max(h, rng.chance(50) ? size.height : std::min(sh, size.height));
                    }
                    ++requestsChecked;
                    require(w <= anchor.width && h <= anchor.height, "P1: the anchor covers the request" + where);
                    require(bound == 0 || refBufferBytes(refDelivered(anchor, w, h)) <= bound,
                            "P1/P3: the request's capture buffer is within the reserved one" + where);
                    require(deliveredFrameSize(anchor.width, anchor.height, w, h) == refDelivered(anchor, w, h), "P1: the delivered frame" + where);
                }
                // P3: the delivered frame grows with the request in each dimension.
                for (int draw = 0; draw < 6; ++draw) {
                    const std::uint32_t w = 16 + 2 * rng.below(anchor.width / 2), h = 16 + 2 * rng.below(anchor.height / 2);
                    const auto base = deliveredFrameBytes(anchor, w, h, rowAlignment);
                    require(deliveredFrameBytes(anchor, std::min(anchor.width, w + 2 * (1 + rng.below(40))), h, rowAlignment) >= base, "P3: grows with the width" + where);
                    require(deliveredFrameBytes(anchor, w, std::min(anchor.height, h + 2 * (1 + rng.below(40))), rowAlignment) >= base, "P3: grows with the height" + where);
                }
                // The rates, against the brute-force whole rates.
                const auto whole = refRates(anchor);
                std::vector<std::uint32_t> inOffer;
                for (const auto& range : offer.rates)
                    for (std::uint32_t rate = range.minimum; rate <= range.maximum; ++rate) inOffer.push_back(rate);
                std::vector<std::uint32_t> expectedOffer;
                for (const auto rate : whole)
                    if (rate <= rules.rateCap) expectedOffer.push_back(rate);
                if (expectedOffer.empty()) expectedOffer.push_back(whole.front());
                require(inOffer == expectedOffer, "the offered rates are the whole rates up to the cap, or the slowest alone" + where);
            }
        }
    }
    require(anchors > 10000 && withoutAnchor > 100 && multiAnchors > 1000 && requestsChecked > 40000, "the tables exercise both outcomes and every path");
}

} // namespace

int main()
{
    aSpansWholeRatesSnapToTheNearWholeNumber();
    aRateAHairOutsideASpanIsProgrammedFromTheSpansOwnDuration();
    theOfferedRatesAreWholeRatesUpToTheCap();
    aPressureCapChoosesTheLargestOfferedRateBelowIt();
    aFormatsPhotoMaximumIsTheLargestSizeWithinTheCap();
    theStillsAFormatMakesAreWithinTheMaximumInBothDimensions();
    theSizesAnyFormatMakesAreTheUnionLargestFirst();
    theOutputSizesFollowTheAnchor();
    theTabletRunsInTheFormatThatMakesTheLargestStillAtTheSmallestCost();
    aFormatThatHasNoRateWithinTheCapLosesToOneThatDoes();
    everyAdmittedStreamSetFitsInsideTheAnchor();
    aFormatAtExactlyTheCapStillOffersItsPhotos();
    aCameraNeverRunsInAFormatLargerThanItsAnchor();
    aCameraWithOnlyFastFormatsKeepsAModeAndItsPhotos();
    aFormatThatTakesTheTierSizeKeepsItsModesWhenAnotherWins();
    everyOfferedSizeStartsInTheAnchor();
    aFrameThatFitsTightlyButNotInTheReservedBufferIsNotOffered();
    aCameraThatListsNearIntegerRatesIsOfferedWholeRates();
    theLargeTierOffersWhatItsLimitsHold();
    aFormatThatCarriesNoStreamIsNotAnAnchor();
    theFrameBoundKeepsAnOversizedFormatOutOfTheBox();
    aTiesBreakByCostThenLowRatesThenFullRangeThenIndex();
    theKeyOrderIsRateThenStillThenVideo();
    aCameraKeepsConcurrencyOnlyWhenItsMultiCameraFormatsOfferAsMuch();
    aCameraJoinsAMultiCameraSessionOnlyWhenEveryAnchorIsAFormatItTakes();
    aRefusedStreamRemovesOnlyTheOutputItAdded();
    aRefusedStreamBeforeItsInputLeavesTheSessionAsItWas();
    aRefusedInputOrFormatAddsNothingElse();
    aRefusedPhotoOutputRemovesTheVideoAndInputItAdded();
    aSessionOverItsBudgetRemovesWhatTheJoinAdded();
    aRefusedStreamOnACameraInTheSessionPutsItsDurationsBack();
    aRefusedStreamPutsTheManualExposureBackToo();
    aRefusedFirstJoinPutsTheDeviceBackAsItWasBeforeTheJoin();
    putBackTimingOrdersItsWritesAndStopsAtARefusal();
    aResizeSetsTheDurationsThenTheSizeAndRestoresTheOldTimingOnARefusal();
    everyCameraIsRepairedBeforeTheSessionIsToldItIsBack();
    aRefusedRepairIsAFailureAndNothingIsAnnouncedAsBack();
    aPressureNotificationIsAppliedWhileTheStreamRunsAndNeverAfterIt();
    aRefusalIsLoggedOncePerCameraAndReasonUntilTheCameraChanges();
    everyGeneratedTableHoldsTheInvariants();
    return 0;
}
