// SPDX-License-Identifier: GPL-2.0-or-later
// The anchor model of photo_dimensions.hpp: the one format a camera runs in,
// what it offers, the rates it takes, and the session's transactions. Every
// reviewer counter-example of the earlier choosers is a named test built from
// the reviewer's table, and the invariants hold over generated tables (the seed
// is in a failure's message).
#include "photo_dimensions.hpp"
#include "require.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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

constexpr std::uint64_t smallFrameBound = 1920ull * 1440 * 3 / 2;
constexpr std::uint64_t largeFrameBound = 3840ull * 2880 * 3 / 2;

// The small tier: 1080p video, 12 MP stills (12.7 million pixels), and the
// frame of 1920x1440 at most.
OfferRules smallRules(const std::vector<VideoFormatFacts>& formats)
{
    OfferRules rules;
    rules.limits = {12'700'000, smallFrameBound};
    rules.maximumVideoWidth = 1920;
    rules.maximumVideoHeight = 1080;
    rules.candidateSizes = candidatesFor(formats);
    return rules;
}

OfferRules largeRules(const std::vector<VideoFormatFacts>& formats)
{
    OfferRules rules;
    rules.limits = {50'000'000, largeFrameBound};
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
        require(deliveredFrameBytes(formats[1], size.width, size.height) <= smallFrameBound, "every offered size fits the frame bound");
    require(deliveredFrameBytes(formats[1], chosen.offer.box.width, chosen.offer.box.height) <= smallFrameBound, "so does the box");
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

struct FakeSession {
    bool input {}, video {}, photo {};
    std::vector<std::string> log;
    int begins {}, commits {};
    bool refuseInput {}, refuseActivate {}, refuseVideo {}, refusePhoto {}, overBudget {};
    // The format and the durations the device holds, which a refused change must not lose.
    int format {7};

    AdmitSteps steps(const AdmitPlan& plan)
    {
        AdmitSteps s;
        s.begin = [this] { ++begins; log.push_back("begin"); };
        s.commit = [this] { ++commits; log.push_back("commit"); };
        s.addInput = [this] {
            log.push_back("addInput");
            if (refuseInput) return false;
            input = true;
            return true;
        };
        s.activate = [this] {
            log.push_back("activate");
            if (refuseActivate) return false;
            format = 1;
            return true;
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
    require(session.log == std::vector<std::string> {"begin", "activate", "addVideo", "removeVideo", "commit"},
            "begin, the rate, the refused video output, its removal, commit: no removal of the photo output or the input");
    require(session.begins == session.commits, "one configuration, closed");
}

void aRefusedStreamBeforeItsInputLeavesTheSessionAsItWas()
{
    FakeSession session;
    session.refuseVideo = true;
    const AdmitPlan plan {.addInput = true, .activate = true, .addVideo = true, .addPhoto = false};
    require(admit(plan, session.steps(plan)) == Admission::videoRefused, "refused");
    require(!session.input && !session.video && !session.photo, "the input this call added left again");
    require(session.log == std::vector<std::string> {"begin", "addInput", "activate", "addVideo", "removeVideo", "removeInput", "commit"},
            "the newest first");
}

void aRefusedInputOrFormatAddsNothingElse()
{
    FakeSession refused;
    refused.refuseInput = true;
    const AdmitPlan plan {.addInput = true, .activate = true, .addVideo = true, .addPhoto = true};
    require(admit(plan, refused.steps(plan)) == Admission::inputRefused && refused.log == std::vector<std::string> {"begin", "addInput", "commit"},
            "a refused input stops the transaction before anything else runs");
    FakeSession format;
    format.refuseActivate = true;
    require(admit(plan, format.steps(plan)) == Admission::formatRefused && !format.input && !format.video && !format.photo,
            "a refused format takes the input it added out");
    require(format.log == std::vector<std::string> {"begin", "addInput", "activate", "removeInput", "commit"}, "and nothing else");
    FakeSession kept;
    kept.input = true;
    kept.refuseActivate = true;
    const AdmitPlan existing {.addInput = false, .activate = true, .addVideo = true, .addPhoto = false};
    require(admit(existing, kept.steps(existing)) == Admission::formatRefused && kept.input && kept.log.size() == 3,
            "a camera that was in the session stays in it");
}

void aRefusedPhotoOutputRemovesTheVideoAndInputItAdded()
{
    FakeSession session;
    session.refusePhoto = true;
    const AdmitPlan plan {.addInput = true, .activate = true, .addVideo = true, .addPhoto = true};
    require(admit(plan, session.steps(plan)) == Admission::photoRefused, "refused");
    require(!session.input && !session.video && !session.photo, "everything this call added left");
    require(session.log == std::vector<std::string> {"begin", "addInput", "activate", "addVideo", "addPhoto", "removePhoto", "removeVideo", "removeInput",
                                                      "commit"},
            "the newest first, inside the one configuration");
    // A photo output refused for a camera that streams leaves the stream alone.
    FakeSession streaming;
    streaming.input = streaming.video = true;
    streaming.refusePhoto = true;
    const AdmitPlan still {.addInput = false, .activate = false, .addVideo = false, .addPhoto = true};
    require(admit(still, streaming.steps(still)) == Admission::photoRefused && streaming.input && streaming.video,
            "a refused still leaves the stream as it was");
}

void aSessionOverItsBudgetRemovesWhatTheJoinAdded()
{
    FakeSession session;
    session.overBudget = true;
    const AdmitPlan plan {.addInput = true, .activate = true, .addVideo = true, .addPhoto = false};
    require(admit(plan, session.steps(plan)) == Admission::overBudget, "refused after the commit");
    require(!session.input && !session.video, "the join is undone");
    require(session.begins == 2 && session.commits == 2, "inside a configuration of its own");
    require(session.log == std::vector<std::string> {"begin", "addInput", "activate", "addVideo", "commit", "begin", "removeVideo", "removeInput", "commit"},
            "the first configuration commits, the second undoes it");
    FakeSession admitted;
    require(admit(plan, admitted.steps(plan)) == Admission::admitted && admitted.input && admitted.video && admitted.begins == 1,
            "a join within budget stays");
}

struct FakeStream {
    int durations {30}, size {640};
    bool refuseDurations {}, refuseSize {};
    std::vector<std::string> log;
    ResizeSteps steps(bool rate, bool dimension)
    {
        ResizeSteps s;
        s.begin = [this] { log.push_back("begin"); };
        s.commit = [this] { log.push_back("commit"); };
        if (rate)
            s.setDurations = [this] {
                log.push_back("durations");
                if (refuseDurations) return false;
                durations = 15;
                return true;
            };
        if (dimension)
            s.setVideoSize = [this] {
                log.push_back("size");
                if (refuseSize) return false;
                size = 1280;
                return true;
            };
        s.restoreDurations = [this] {
            log.push_back("restore");
            durations = 30;
        };
        return s;
    }
};

void aResizeSetsTheDurationsThenTheSizeAndRestoresTheOldDurationsOnARefusal()
{
    FakeStream ok;
    require(resizeStream(ok.steps(true, true)) == Resizing::resized && ok.durations == 15 && ok.size == 1280, "both change");
    require(ok.log == std::vector<std::string> {"begin", "durations", "size", "commit"}, "the durations first");
    FakeStream sizeRefused;
    sizeRefused.refuseSize = true;
    require(resizeStream(sizeRefused.steps(true, true)) == Resizing::sizeRefused && sizeRefused.durations == 30,
            "a refused size programs the old durations again");
    require(sizeRefused.log == std::vector<std::string> {"begin", "durations", "size", "restore", "commit"}, "in the same configuration");
    FakeStream durationsRefused;
    durationsRefused.refuseDurations = true;
    require(resizeStream(durationsRefused.steps(true, true)) == Resizing::durationsRefused && durationsRefused.size == 640 &&
                durationsRefused.log == std::vector<std::string> {"begin", "durations", "restore", "commit"},
            "a refused rate leaves the size alone");
    FakeStream sizeOnly;
    sizeOnly.refuseSize = true;
    require(resizeStream(sizeOnly.steps(false, true)) == Resizing::sizeRefused &&
                sizeOnly.log == std::vector<std::string> {"begin", "size", "commit"},
            "a resize that does not change the rate has no durations to restore");
    FakeStream rateOnly;
    require(resizeStream(rateOnly.steps(true, false)) == Resizing::resized && rateOnly.size == 640, "a rate alone");
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
        if (deliveredFrameBytes(format, w, h) > rules.limits.maximumFrameBytes) continue;
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
                require(bound == 0 || deliveredFrameBytes(anchor, offer.box.width, offer.box.height) <= bound, "P3: the box delivers within the bound" + where);
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
                    require(bound == 0 || deliveredFrameBytes(anchor, w, h) <= bound, "P1/P3: the request delivers within the bound" + where);
                }
                // P3: the delivered frame grows with the request in each dimension.
                for (int draw = 0; draw < 6; ++draw) {
                    const std::uint32_t w = 16 + 2 * rng.below(anchor.width / 2), h = 16 + 2 * rng.below(anchor.height / 2);
                    const auto base = deliveredFrameBytes(anchor, w, h);
                    require(deliveredFrameBytes(anchor, std::min(anchor.width, w + 2 * (1 + rng.below(40))), h) >= base, "P3: grows with the width" + where);
                    require(deliveredFrameBytes(anchor, w, std::min(anchor.height, h + 2 * (1 + rng.below(40)))) >= base, "P3: grows with the height" + where);
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
    aResizeSetsTheDurationsThenTheSizeAndRestoresTheOldDurationsOnARefusal();
    aRefusalIsLoggedOncePerCameraAndReasonUntilTheCameraChanges();
    everyGeneratedTableHoldsTheInvariants();
    return 0;
}
