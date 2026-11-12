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
        if (calibrated)
            for (const auto& info : infos)
                if (info.rawStills) {
                    const auto size = info.rawStillSize;
                    store.save(camera->hostModel(), CalibrationStore::lensKey(info), colorOf(size.width, size.height));
                }
        ArrangementOptions options;
        options.tier = tier;
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

} // namespace

int main(int argc, char** argv)
{
    require(argc == 2, "the layout table is the argument");
    testTheImagesTable(argv[1]);
    testNormalization();
    testMissingCombinations();
    testHostsEarnTheirLayouts();
    testConcurrentRuleIsTheHals();
    std::puts("camera-layout: passed");
    return 0;
}
