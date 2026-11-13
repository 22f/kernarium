// SPDX-License-Identifier: GPL-2.0-or-later
// The camera layouts a host composes for the Android image: the closed set and its fallback order against the
// image's own table (layouts.tsv, the generator's output, which the test takes as its argument), what a host's
// cameras earn, and the concurrent feature against the HAL's own rule.

#include "calibration_store.hpp"
#include "camera_description.hpp"
#include "camera_layout.hpp"
#include "camera_plan.hpp"
#include "require.hpp"
#include "synthetic_camera.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace uml::camera;
using uml::platform::BayerOrder;
using uml::platform::CameraColorData;
using uml::platform::CameraInfo;
using uml::platform::CameraPosition;
using uml::platform::SyntheticCameraOptions;
using uml::platform::SyntheticCameraProfile;
using uml::testing::require;

namespace {

struct Row {
    std::string name;
    std::string cameras;
    std::string tier;
    bool flash {};
    bool manual {};
    bool raw {};
    bool concurrent {};
    bool front2160 {};
    std::string variant;
    std::vector<std::string> fallback;
};

std::vector<std::string> split(const std::string& text, char separator)
{
    std::vector<std::string> parts;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, separator)) parts.push_back(part);
    return parts;
}

std::vector<Row> readTable(const char* path)
{
    std::ifstream file(path);
    require(file.good(), "the layout table opens");
    std::vector<Row> rows;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto columns = split(line, '\t');
        require(columns.size() == 10, "ten columns");
        Row row;
        row.name = columns[0];
        row.cameras = columns[1];
        row.tier = columns[2];
        row.flash = columns[3] == "1";
        row.manual = columns[4] == "1";
        row.raw = columns[5] == "1";
        row.concurrent = columns[6] == "1";
        row.front2160 = columns[7] == "1";
        row.variant = columns[8];
        if (columns[9] != "-") row.fallback = split(columns[9], ',');
        rows.push_back(std::move(row));
    }
    return rows;
}

LayoutDimensions dimensionsOf(const Row& row)
{
    LayoutDimensions dimensions;
    dimensions.cameras = row.cameras == "none" ? LayoutCameras::none
        : row.cameras == "front"               ? LayoutCameras::front
                                               : LayoutCameras::backAndFront;
    dimensions.tier = row.tier == "large" ? CameraTier::large : CameraTier::small;
    dimensions.flash = row.flash;
    dimensions.manual = row.manual;
    dimensions.raw = row.raw;
    dimensions.concurrent = row.concurrent;
    dimensions.front2160 = row.front2160;
    return dimensions;
}

// The set, its order and every fallback chain are the image's table.
void testTheImagesTable(const char* path)
{
    const auto rows = readTable(path);
    const auto layouts = allLayouts();
    const auto names = allLayoutNames();
    require(rows.size() == 39 && layouts.size() == 39 && names.size() == 39, "thirty-nine layouts");
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        require(names[index] == row.name, "the same name in the same place");
        require(layouts[index] == dimensionsOf(row), "and the same dimensions");
        require(layoutName(dimensionsOf(row)) == row.name, "a row's dimensions compose its name");
        auto chain = std::vector<std::string> {row.name};
        chain.insert(chain.end(), row.fallback.begin(), row.fallback.end());
        require(layoutFallbackChain(dimensionsOf(row)) == chain, "and fall back as the table says");
    }
    const std::set<std::string> distinct(names.begin(), names.end());
    require(distinct.size() == names.size(), "names are distinct");
}

void testNormalization()
{
    // A tier, flash, RAW, concurrency or 2160p means nothing without back cameras.
    LayoutDimensions front {LayoutCameras::front, CameraTier::large, true, false, true, true, true};
    require(layoutName(front) == "front", "a front camera alone says nothing of the back");
    front.manual = true;
    require(layoutName(front) == "front-manual", "manual control is the one thing it may say");
    require(layoutName({LayoutCameras::none, CameraTier::large, true, true, true, true, true}) == "none", "no cameras is none");
    // RAW needs manual control and 2160p the large tier.
    require(layoutName({LayoutCameras::backAndFront, CameraTier::small, false, false, true, false, false}) == "bf-small", "no RAW without manual");
    require(layoutName({LayoutCameras::backAndFront, CameraTier::small, false, true, true, false, true}) == "bf-small-manual-raw",
            "no 2160p in the small tier");
    require(layoutName({LayoutCameras::backAndFront, CameraTier::large, true, true, true, true, true}) ==
                "bf-large-flash-manual-raw-concurrent-front2160",
            "the name with every dimension");
}

bool isIn(const std::set<std::string>& available, std::string_view name) { return available.count(std::string(name)) != 0; }

// A host whose combination an image lacks takes the nearest layout the image has.
void testMissingCombinations()
{
    const LayoutDimensions phone {LayoutCameras::backAndFront, CameraTier::large, true, true, true, true, true};
    const auto choose = [&](const std::set<std::string>& available) {
        return chooseLayout(phone, [&](std::string_view name) { return isIn(available, name); });
    };
    const auto all = allLayoutNames();
    require(choose(std::set<std::string>(all.begin(), all.end())) == "bf-large-flash-manual-raw-concurrent-front2160", "an image with all of them");
    require(chooseLayout(phone, {}) == "bf-large-flash-manual-raw-concurrent-front2160", "no restriction accepts the first");

    // An image without the 2160p layouts: concurrency goes first, then RAW, then 2160p, and the image has the rest.
    std::set<std::string> no2160;
    for (const auto& name : all)
        if (name.find("front2160") == std::string::npos) no2160.insert(name);
    require(choose(no2160) == "bf-large-flash-manual", "concurrency and RAW go before 2160p does");

    // An image without concurrent streaming or RAW.
    std::set<std::string> plain;
    for (const auto& name : all)
        if (name.find("concurrent") == std::string::npos && name.find("raw") == std::string::npos) plain.insert(name);
    require(choose(plain) == "bf-large-flash-manual-front2160", "without concurrency and RAW both go");

    // An image of the small tier only: the flash is dropped before the tier, and the tier before manual control.
    std::set<std::string> small;
    for (const auto& name : all)
        if (name.find("bf-large") == std::string::npos) small.insert(name);
    require(choose(small) == "bf-small-manual", "the large tier gives way before manual control and the flash goes first");
    small.erase("bf-small-manual");
    require(choose(small) == "bf-small", "then manual control");
    small.erase("bf-small");
    require(choose(small) == "front", "then the back cameras");
    require(choose({"none"}) == "none", "then every camera");
    require(choose({}) == "none", "an image with nothing takes none");
}

CameraColorData colorOf(std::uint32_t width, std::uint32_t height)
{
    CameraColorData data;
    data.bayer = BayerOrder::grbg;
    data.blackLevel = 64;
    data.whiteLevel = 1023;
    data.rawWidth = width;
    data.rawHeight = height;
    data.illuminant1 = 17;
    data.illuminant2 = 21;
    data.colorMatrix1 = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    data.focalLengthMillimetres = 6.9;
    data.intrinsics = {3000, 3000, 1008, 756};
    return data;
}

struct Host {
    std::unique_ptr<uml::platform::SyntheticCamera> camera;
    std::vector<CameraInfo> infos;
    explicit Host(SyntheticCameraProfile profile)
    {
        SyntheticCameraOptions options;
        options.profile = profile;
        camera = uml::platform::makeSyntheticCamera(options);
        infos = camera->cameras();
    }
    // The host's cameras at a tier, with every lens that can take a Bayer still calibrated or none.
    [[nodiscard]] std::vector<CameraDescription> arrange(CameraTier tier, bool calibrated) const
    {
        CalibrationStore store;
        ArrangementOptions options;
        options.tier = tier;
        if (calibrated) {
            // The colour data was learned in the camera's anchor, which the arrangement names.
            const auto bare = arrangeCameras(infos, camera->concurrentSets(), camera->hostModel(), store, options);
            for (const auto& arranged : bare)
                if (arranged.info.rawStills) {
                    const auto size = arranged.info.rawStillSize;
                    auto color = colorOf(size.width, size.height);
                    if (arranged.anchor) {
                        color.anchorWidth = arranged.info.captureFormats[*arranged.anchor].width;
                        color.anchorHeight = arranged.info.captureFormats[*arranged.anchor].height;
                    }
                    store.save(camera->hostModel(), CalibrationStore::lensKey(arranged.info), color);
                }
        }
        return arrangeCameras(infos, camera->concurrentSets(), camera->hostModel(), store, options);
    }
};

// What the synthetic hosts earn. These are the layouts the Android runner expects of the same profiles
// (tests/integration/android/run_android_camera.py).
// The layout a host composes, named; a mismatch says which.
std::string composed(const std::vector<CameraDescription>& cameras, CameraTier tier, const char* expected)
{
    auto name = layoutName(layoutDimensionsOf(cameras, tier));
    if (name != expected) std::fprintf(stderr, "composed %s, expected %s\n", name.c_str(), expected);
    return name;
}

void testHostsEarnTheirLayouts()
{
    const Host phone(SyntheticCameraProfile::phone);
    require(composed(phone.arrange(CameraTier::large, true), CameraTier::large, "bf-large-flash-manual-raw-concurrent-front2160") ==
                "bf-large-flash-manual-raw-concurrent-front2160",
            "a phone with calibrated lenses");
    require(composed(phone.arrange(CameraTier::large, false), CameraTier::large, "bf-large-flash-manual-concurrent-front2160") ==
                "bf-large-flash-manual-concurrent-front2160",
            "and before the first photo it has no RAW");
    require(composed(phone.arrange(CameraTier::small, true), CameraTier::small, "bf-small-flash-manual-raw") ==
                "bf-small-flash-manual-raw",
            "the small tier caps video at 1080p: no 2160p, and no 1920x1440 for the pair that concurrency needs");

    const Host tablet(SyntheticCameraProfile::tablet);
    require(composed(tablet.arrange(CameraTier::small, true), CameraTier::small, "bf-small-flash-manual-raw") ==
                "bf-small-flash-manual-raw",
            "a tablet whose front camera cannot make the guaranteed concurrent sizes");

    const Host laptop(SyntheticCameraProfile::laptop);
    require(layoutName(layoutDimensionsOf(laptop.arrange(CameraTier::large, true), CameraTier::large)) == "front",
            "a laptop has a front camera, with no manual control");

    require(layoutName(layoutDimensionsOf({}, CameraTier::large)) == "none", "a host without cameras");

    // A back camera with no front one has no layout of its own.
    auto backOnly = tablet.arrange(CameraTier::small, false);
    backOnly.erase(std::remove_if(backOnly.begin(), backOnly.end(),
                                  [](const CameraDescription& camera) { return camera.info.position == CameraPosition::front; }),
                   backOnly.end());
    require(layoutName(layoutDimensionsOf(backOnly, CameraTier::small)) == "none", "a back camera alone composes none");

    // Cameras that only run one at a time do not earn concurrency.
    auto apart = phone.arrange(CameraTier::large, true);
    apart[0].wire.concurrent_mask = 0;
    require(!layoutDimensionsOf(apart, CameraTier::large).concurrent, "concurrency needs both to name each other");
}

// The layout of a platform camera's host is the one the device's own arrangement earns, restricted to what the
// image has.
void testComposeLayoutOfAHost()
{
    CalibrationStore store;
    ArrangementOptions options;
    options.tier = CameraTier::large;

    SyntheticCameraOptions phoneOptions;
    phoneOptions.profile = SyntheticCameraProfile::phone;
    const auto phone = uml::platform::makeSyntheticCamera(phoneOptions);
    require(composeLayout(*phone, store, options) == "bf-large-flash-manual-concurrent-front2160", "discovery composes the name");
    require(phone->sessionsOpened() == 0, "and opens no session");
    const auto smallOnly = [](std::string_view name) { return name.find("bf-large") == std::string_view::npos; };
    require(composeLayout(*phone, store, options, smallOnly) == "bf-small-manual", "from the layouts an image has (the flash goes before the tier)");
    options.tier = CameraTier::small;
    require(composeLayout(*phone, store, options) == "bf-small-flash-manual", "the tier is the launch's");

    SyntheticCameraOptions laptopOptions;
    laptopOptions.profile = SyntheticCameraProfile::laptop;
    require(composeLayout(*uml::platform::makeSyntheticCamera(laptopOptions), store, options) == "front", "a laptop");
    require(layoutArgumentPrefix == "androidboot.product.vendor.sku=", "the argument Android imports");
}

// The guaranteed concurrent sizes: this file's rule is the HAL's.
andarium::camera::Formats halFormats(const CameraDescription& camera)
{
    andarium::camera::Formats formats;
    for (const auto& mode : camera.video)
        formats.video.push_back({mode.width, mode.height, mode.minimumFrameRate, mode.maximumFrameRate});
    for (const auto& size : camera.jpegSizes) formats.jpeg.push_back({size.width, size.height});
    return formats;
}

bool halMeets(const CameraDescription& camera)
{
    return andarium::camera::meets_concurrent_streams(andarium::camera::derive_sizes(halFormats(camera)));
}

CameraDescription describe(std::vector<std::pair<std::uint32_t, std::uint32_t>> video,
                           std::vector<std::pair<std::uint32_t, std::uint32_t>> stills)
{
    CameraDescription camera;
    for (const auto& [width, height] : video) camera.video.push_back({width, height, 1, 30});
    for (const auto& [width, height] : stills) camera.jpegSizes.push_back({width, height});
    return camera;
}

void testConcurrentRuleIsTheHals()
{
    for (const auto profile : {SyntheticCameraProfile::phone, SyntheticCameraProfile::tablet, SyntheticCameraProfile::tabletDualFront,
                               SyntheticCameraProfile::laptop}) {
        const Host host(profile);
        for (const auto tier : {CameraTier::small, CameraTier::large})
            for (const auto& camera : host.arrange(tier, false))
                require(meetsConcurrentStreams(camera) == halMeets(camera), "a camera of a profile meets the sizes as the HAL says");
    }
    const std::vector<CameraDescription> cases = {
        describe({}, {}),
        describe({{1280, 720}}, {}),
        describe({{640, 480}}, {{640, 480}}),
        describe({{1920, 1080}}, {{4032, 3024}}),
        describe({{1920, 1080}, {1280, 720}}, {{1920, 1080}}),
        describe({{1920, 1440}}, {{4032, 3024}}),
        describe({{1920, 1440}, {1280, 960}}, {{4032, 3024}}),
        describe({{2016, 1512}, {1920, 1080}, {1280, 720}}, {{2016, 1512}}),
        describe({{3840, 2160}, {1920, 1080}}, {{4032, 3024}, {8064, 6048}}),
        describe({{3840, 2160}}, {{3840, 2160}}),
        describe({{1280, 960}}, {{4032, 3024}}),
        describe({{2736, 2052}}, {{2736, 2052}}),
        describe({{1920, 1080}}, {{1920, 1080}}),
        describe({{1280, 720}}, {{1280, 720}}),
        describe({{1281, 721}}, {{1281, 721}}),
    };
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const bool ours = meetsConcurrentStreams(cases[index]);
        const bool hals = halMeets(cases[index]);
        if (ours != hals) std::fprintf(stderr, "case %zu: ours %d, the HAL's %d\n", index, ours, hals);
        require(ours == hals, "the rule is the HAL's on a made camera");
    }
    // A camera that covers 1920x1440 with a video mode meets them; one limited to 1080p does not.
    require(meetsConcurrentStreams(describe({{1920, 1440}, {1280, 720}}, {{4032, 3024}})), "a video mode of 1920x1440 covers the pair");
    require(!meetsConcurrentStreams(describe({{1920, 1080}}, {{4032, 3024}})), "1080p cannot make 1920x1440");
}

// The reserve follows the composed layout: what a live session of a layout can hold outside the guest's RAM, from
// the cameras that stream together and the RAW still it has. No layout holds more than the tier's ceiling, and the
// ceiling is a layout's own figure: the one with both cameras running together and RAW.
void testTheReserveFollowsTheLayout()
{
    const auto tier = CameraTier::small;
    const auto ceiling = cameraHostMemoryReserveMiB(tier);
    require(ceiling == 154, "the tier's ceiling");
    require(cameraHostMemoryReserveMiB(tier, CameraUse {}) == ceiling, "the default use is the ceiling");

    // The four figures a small-tier layout can have: one camera or two, with or without RAW.
    require(cameraHostMemoryReserveMiB(tier, {1, false}) == 56, "one stream, no RAW: 55.9 MiB, rounded up");
    require(cameraHostMemoryReserveMiB(tier, {1, true}) == 129, "one stream and a RAW still");
    require(cameraHostMemoryReserveMiB(tier, {2, false}) == 82, "two streams, no RAW");
    require(cameraHostMemoryReserveMiB(tier, {2, true}) == 154, "two streams and a RAW still");
    require(cameraHostMemory(tier, {1, false}).rawStill == 0 && cameraHostMemory(tier, {1, true}).rawStill == 12'700'000ull * 6,
            "the RAW term is the RAW still's alone");
    require(cameraHostMemory(tier, {1, false}).capturePools * 2 == cameraHostMemory(tier, {2, false}).capturePools,
            "the capture pools are those of the cameras that stream");

    std::set<std::uint64_t> figures;
    bool reachesTheCeiling = false;
    for (const auto& dimensions : allLayouts()) {
        const auto name = layoutName(dimensions);
        const auto reserve = liveCameraReserveMiB(CameraMode::live, name, tier);
        require(liveCameraReserveMiB(CameraMode::blocked, name, tier) == 0, "a blocked session holds none");
        if (!layoutHasCameras(name)) {
            require(reserve == 0, "no cameras, no device, no reserve");
            continue;
        }
        const auto named = layoutDimensionsNamed(name);
        require(named && layoutName(*named) == name, "every layout of the set is found by its name");
        const bool together = dimensions.cameras == LayoutCameras::backAndFront && dimensions.concurrent;
        require(cameraUseOf(dimensions) == CameraUse {together ? 2u : 1u, dimensions.raw}, name);
        require((name.find("concurrent") != std::string::npos) == dimensions.concurrent, name);
        require((name.find("raw") != std::string::npos) == dimensions.raw, name);
        require(reserve == cameraHostMemoryReserveMiB(tier, cameraUseOf(dimensions)), name);
        require(reserve <= ceiling, "no layout holds more than the tier's ceiling");
        require(reserve == (together ? (dimensions.raw ? 154u : 82u) : (dimensions.raw ? 129u : 56u)), name);
        figures.insert(reserve);
        reachesTheCeiling = reachesTheCeiling || reserve == ceiling;
    }
    require(figures == std::set<std::uint64_t> {56, 82, 129, 154}, "the four figures are the layouts' all");
    require(reachesTheCeiling, "the ceiling is a layout's own figure");

    // A layout that is not stated, or is not of the image's set, holds the ceiling: nothing known to lower it.
    require(liveCameraReserveMiB(CameraMode::live, "", tier) == ceiling);
    require(liveCameraReserveMiB(CameraMode::live, "bf-small-flash-manual-raw-concurrent-nothing", tier) == ceiling);
    require(!layoutDimensionsNamed("").has_value() && !layoutDimensionsNamed("bf-small-nothing").has_value());
    require(liveCameraReserveMiB(CameraMode::live, "none", tier) == 0 && liveCameraReserveMiB(CameraMode::blocked, "", tier) == 0);

    // The hosts of the synthetic profiles earn the layouts above: the iPad's pair (the front camera does not run with
    // the back one, no RAW before it is learned) holds the least, a phone that learned RAW and runs both together the most.
    require(liveCameraReserveMiB(CameraMode::live, "bf-small-flash-manual", tier) == 56, "a tablet's pair before RAW is learned");
    require(liveCameraReserveMiB(CameraMode::live, "bf-small-flash-manual-raw", tier) == 129, "with RAW learned");
    require(liveCameraReserveMiB(CameraMode::live, "bf-small-flash-manual-concurrent", tier) == 82, "a pair that runs together");
    require(liveCameraReserveMiB(CameraMode::live, "bf-small-flash-manual-raw-concurrent", tier) == 154, "with RAW learned");
    require(liveCameraReserveMiB(CameraMode::live, "front", tier) == 56 && liveCameraReserveMiB(CameraMode::live, "front-manual", tier) == 56,
            "a host with a front camera alone (a laptop) holds one stream");
}

} // namespace

int main(int argc, char** argv)
{
    require(argc == 2, "the layout table is the argument");
    testTheImagesTable(argv[1]);
    testNormalization();
    testMissingCombinations();
    testHostsEarnTheirLayouts();
    testComposeLayoutOfAHost();
    testConcurrentRuleIsTheHals();
    testTheReserveFollowsTheLayout();
    std::puts("camera-layout: passed");
    return 0;
}
