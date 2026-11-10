// SPDX-License-Identifier: GPL-2.0-or-later
#include "synthetic_camera.hpp"

#include <time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace uml::platform {
namespace {

constexpr std::uint32_t markerCells = 64;
constexpr std::uint32_t markerHeight = 8;
constexpr std::uint8_t markerOne = 235;
constexpr std::uint8_t markerZero = 16;
// An exposure of 1/120 s at ISO 100 is the chart's unit brightness.
constexpr double referenceExposure = 1.0 / 120.0 * 100.0;
constexpr std::uint32_t whiteLevel = 16383;
constexpr std::uint32_t blackLevel = 1024;

std::uint64_t realNow() noexcept
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

// ----------------------------------------------------------------------------
// Profiles
// ----------------------------------------------------------------------------

CameraVideoFormat format(std::uint32_t width, std::uint32_t height, std::uint32_t maximumRate, bool multi = true)
{
    return {width, height, 1, maximumRate, multi};
}

struct Profile {
    std::string model;
    std::vector<CameraInfo> cameras;
    std::vector<std::vector<std::size_t>> sets;
};

CameraInfo lens(const char* identifier, CameraPosition position, CameraLensKind kind)
{
    CameraInfo info;
    info.identifier = identifier;
    info.position = position;
    info.lens = kind;
    info.autofocus = position == CameraPosition::back;
    info.zoomMinimum = 1.0f;
    info.zoomMaximum = 8.0f;
    info.horizontalFieldOfView = kind == CameraLensKind::ultraWide ? 106.0f : kind == CameraLensKind::telephoto ? 28.0f : 63.0f;
    info.aperture = kind == CameraLensKind::ultraWide ? 2.2f : kind == CameraLensKind::telephoto ? 2.8f : 1.8f;
    info.minimumFocusDistanceMillimetres = position == CameraPosition::back ? 120.0f : 0.0f;
    info.isoMinimum = 25;
    info.isoMaximum = 3200;
    info.exposureMinimumNanoseconds = 20'000;
    info.exposureMaximumNanoseconds = 1'000'000'000;
    info.maximumWhiteBalanceGain = 4.0f;
    info.sensorOrientation = position == CameraPosition::front ? 270 : 90;
    info.manualExposure = info.manualFocus = info.manualWhiteBalance = true;
    info.manualFocus = info.autofocus;
    info.videoFormats = {format(640, 480, 60), format(1280, 720, 120), format(1920, 1080, 60), format(3840, 2160, 60, false)};
    info.stillSizes = {{4032, 3024}, {2016, 1512}, {1008, 756}};
    info.pixelArrayWidth = 4032;
    info.pixelArrayHeight = 3024;
    return info;
}

Profile profileOf(SyntheticCameraProfile which)
{
    Profile profile;
    switch (which) {
    case SyntheticCameraProfile::phone: {
        profile.model = "SyntheticPhone1,1";
        CameraInfo zoom = lens("synthetic-back-zoom", CameraPosition::back, CameraLensKind::zoom);
        zoom.zoomMinimum = 0.5f;
        zoom.zoomMaximum = 15.0f;
        zoom.switchOvers = {1.0f, 3.0f};
        zoom.constituents = {"synthetic-back-ultrawide", "synthetic-back-wide", "synthetic-back-tele"};
        zoom.manualExposure = zoom.manualFocus = zoom.manualWhiteBalance = false;
        zoom.flash = zoom.torch = true;
        CameraInfo wide = lens("synthetic-back-wide", CameraPosition::back, CameraLensKind::wide);
        wide.flash = wide.torch = true;
        wide.rawStills = true;
        wide.rawStillSize = {2016, 1512};
        wide.stillSizes = {{8064, 6048}, {4032, 3024}, {2016, 1512}, {1008, 756}};
        wide.pixelArrayWidth = 8064;
        wide.pixelArrayHeight = 6048;
        CameraInfo ultra = lens("synthetic-back-ultrawide", CameraPosition::back, CameraLensKind::ultraWide);
        ultra.zoomMinimum = 0.5f;
        ultra.zoomMaximum = 4.0f;
        ultra.rawStills = true;
        ultra.rawStillSize = {2016, 1512};
        CameraInfo tele = lens("synthetic-back-tele", CameraPosition::back, CameraLensKind::telephoto);
        tele.rawStills = true;
        tele.rawStillSize = {2016, 1512};
        CameraInfo front = lens("synthetic-front", CameraPosition::front, CameraLensKind::wide);
        front.zoomMaximum = 4.0f;
        front.flash = front.torch = false;
        front.manualExposure = front.manualWhiteBalance = true;
        front.stillSizes = {{4032, 3024}, {2016, 1512}, {1008, 756}};
        profile.cameras = {zoom, wide, ultra, tele, front};
        profile.sets = {{0, 4}, {1, 4}, {2, 4}, {3, 4}};
        break;
    }
    case SyntheticCameraProfile::tablet: {
        profile.model = "SyntheticTablet1,1";
        CameraInfo back = lens("synthetic-back", CameraPosition::back, CameraLensKind::wide);
        back.flash = back.torch = true;
        back.rawStills = true;
        back.rawStillSize = {1008, 756};
        CameraInfo front = lens("synthetic-front", CameraPosition::front, CameraLensKind::wide);
        front.videoFormats = {format(640, 480, 60), format(1280, 720, 60), format(1920, 1080, 60)};
        front.stillSizes = {{2016, 1512}, {1008, 756}};
        front.pixelArrayWidth = 2016;
        front.pixelArrayHeight = 1512;
        front.zoomMaximum = 4.0f;
        profile.cameras = {back, front};
        profile.sets = {{0, 1}};
        break;
    }
    case SyntheticCameraProfile::tabletDualFront: {
        profile.model = "SyntheticTablet2,1";
        CameraInfo back = lens("synthetic-back", CameraPosition::back, CameraLensKind::wide);
        back.flash = back.torch = true;
        back.rawStills = true;
        back.rawStillSize = {1008, 756};
        // The smallest size that covers 1600 x 900, and a single camera's alone.
        back.videoFormats = {format(640, 480, 60), format(1280, 720, 120), format(1600, 900, 60, false),
                             format(1920, 1080, 60), format(3840, 2160, 60, false)};
        CameraInfo front = lens("synthetic-front", CameraPosition::front, CameraLensKind::wide);
        front.videoFormats = {format(640, 480, 60), format(1280, 720, 60), format(1920, 1080, 60)};
        front.stillSizes = {{2016, 1512}, {1008, 756}};
        front.pixelArrayWidth = 2016;
        front.pixelArrayHeight = 1512;
        front.zoomMaximum = 4.0f;
        CameraInfo ultra = lens("synthetic-front-ultrawide", CameraPosition::front, CameraLensKind::ultraWide);
        ultra.videoFormats = {format(640, 480, 60), format(1280, 720, 60), format(1920, 1080, 60)};
        ultra.stillSizes = {{2016, 1512}, {1008, 756}};
        ultra.pixelArrayWidth = 2016;
        ultra.pixelArrayHeight = 1512;
        ultra.zoomMinimum = 0.5f;
        ultra.zoomMaximum = 2.0f;
        profile.cameras = {back, front, ultra};
        profile.sets = {{0, 1}, {0, 2}};
        break;
    }
    case SyntheticCameraProfile::laptop: {
        profile.model = "SyntheticLaptop1,1";
        CameraInfo front = lens("synthetic-front", CameraPosition::front, CameraLensKind::wide);
        front.autofocus = false;
        front.manualExposure = front.manualFocus = front.manualWhiteBalance = false;
        front.sensorOrientation = 0;
        front.zoomMaximum = 1.0f;
        front.videoFormats = {format(640, 480, 30, false), format(1280, 720, 30, false), format(1920, 1080, 30, false)};
        front.stillSizes = {{1920, 1080}};
        front.pixelArrayWidth = 1920;
        front.pixelArrayHeight = 1080;
        front.isoMinimum = front.isoMaximum = 0;
        front.exposureMinimumNanoseconds = front.exposureMaximumNanoseconds = 0;
        front.maximumWhiteBalanceGain = 0;
        profile.cameras = {front};
        break;
    }
    }
    return profile;
}

// ----------------------------------------------------------------------------
// The chart
// ----------------------------------------------------------------------------

struct Patch {
    std::uint8_t y, cb, cr;
};

// Six by four patches in the order of a colour checker's rows.
constexpr std::array<Patch, 24> patches = {{
    {80, 118, 140}, {150, 120, 138}, {100, 150, 118}, {90, 110, 125}, {120, 145, 130}, {170, 135, 90},
    {140, 80, 175}, {70, 160, 130}, {110, 120, 190}, {60, 150, 150}, {180, 100, 100}, {160, 90, 150},
    {50, 190, 110}, {100, 90, 100}, {90, 100, 190}, {200, 60, 140}, {110, 170, 170}, {120, 140, 60},
    {240, 128, 128}, {200, 128, 128}, {160, 128, 128}, {120, 128, 128}, {80, 128, 128}, {40, 128, 128},
}};

struct Picture {
    std::uint32_t width {};
    std::uint32_t height {};
    std::vector<std::uint8_t> luma;    // width x height
    std::vector<std::uint8_t> chroma;  // width x height / 2, interleaved
};

Picture baseChart(std::uint32_t width, std::uint32_t height)
{
    Picture picture;
    picture.width = width;
    picture.height = height;
    picture.luma.resize(std::size_t(width) * height);
    picture.chroma.resize(std::size_t(width) * (height / 2));
    const std::uint32_t gridTop = markerHeight * 2;
    const std::uint32_t gridHeight = height * 3 / 4 - gridTop;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t value = std::uint8_t(16 + 219 * std::uint64_t(x) / std::max<std::uint32_t>(width - 1, 1));
            std::uint8_t cb = 128, cr = 128;
            if (y >= gridTop && y < gridTop + gridHeight && x >= width / 8 && x < width * 7 / 8) {
                const auto column = (x - width / 8) * 6 / (width * 3 / 4);
                const auto row = (y - gridTop) * 4 / gridHeight;
                const auto& patch = patches[std::min<std::size_t>(row, 3) * 6 + std::min<std::size_t>(column, 5)];
                value = patch.y;
                cb = patch.cb;
                cr = patch.cr;
            } else if (y >= height * 3 / 4) {
                // A slanted edge, for sharpness.
                const bool right = x * 2 + (y - height * 3 / 4) > width;
                value = right ? 220 : 40;
            }
            picture.luma[std::size_t(y) * width + x] = value;
            if ((x & 1) == 0 && (y & 1) == 0) {
                const auto at = (std::size_t(y / 2) * width) + x;
                picture.chroma[at] = cb;
                picture.chroma[at + 1] = cr;
            }
        }
    }
    return picture;
}

struct Scene {
    double gain {1.0};
    int blur {};
    int tintCb {};
    int tintCr {};
    float zoom {1.0f};
    std::uint32_t counter {};
    std::uint16_t tag {};
};

std::uint8_t clampByte(int value) noexcept
{
    return std::uint8_t(std::clamp(value, 0, 255));
}

void drawMarker(std::vector<std::uint8_t>& luma, std::vector<std::uint8_t>& chroma, std::uint32_t width,
                std::uint16_t tag, std::uint32_t counter)
{
    const std::uint64_t first48 = (std::uint64_t(tag) << 32) | counter;
    const std::uint16_t check = std::uint16_t((first48 >> 32) ^ (first48 >> 16) ^ first48);
    const std::uint64_t bits = (first48 << 16) | check;
    const auto cell = markerCell(width);
    for (std::uint32_t index = 0; index < markerCells; ++index) {
        const bool one = (bits >> (markerCells - 1 - index)) & 1;
        for (std::uint32_t y = 0; y < markerHeight; ++y)
            std::memset(&luma[std::size_t(y) * width + std::size_t(index) * cell], one ? markerOne : markerZero, cell);
        for (std::uint32_t y = 0; y < markerHeight / 2; ++y)
            for (std::uint32_t x = 0; x < cell; x += 2) {
                chroma[std::size_t(y) * width + std::size_t(index) * cell + x] = 128;
                chroma[std::size_t(y) * width + std::size_t(index) * cell + x + 1] = 128;
            }
    }
}

Picture render(const Picture& base, const Scene& scene)
{
    const auto width = base.width;
    const auto height = base.height;
    Picture out;
    out.width = width;
    out.height = height;
    out.luma.resize(base.luma.size());
    out.chroma.resize(base.chroma.size());
    if (scene.zoom > 1.001f) {
        // Digital zoom: the centre of the chart, enlarged without smoothing.
        for (std::uint32_t y = 0; y < height; ++y) {
            const auto sy = std::min<std::uint32_t>(
                height - 1, std::uint32_t(height / 2.0 + (y - height / 2.0) / scene.zoom));
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto sx = std::min<std::uint32_t>(
                    width - 1, std::uint32_t(width / 2.0 + (x - width / 2.0) / scene.zoom));
                out.luma[std::size_t(y) * width + x] = base.luma[std::size_t(sy) * width + sx];
                if (((x | y) & 1) == 0) {
                    const auto from = (std::size_t(sy / 2) * width) + (sx & ~1u);
                    const auto to = (std::size_t(y / 2) * width) + x;
                    out.chroma[to] = base.chroma[from];
                    out.chroma[to + 1] = base.chroma[from + 1];
                }
            }
        }
    } else {
        out.luma = base.luma;
        out.chroma = base.chroma;
    }
    if (scene.blur > 0) {
        const int radius = scene.blur;
        std::vector<std::uint8_t> row(width);
        for (std::uint32_t y = markerHeight; y < height; ++y) {
            auto* line = &out.luma[std::size_t(y) * width];
            for (std::uint32_t x = 0; x < width; ++x) {
                int sum = 0, count = 0;
                for (int d = -radius; d <= radius; ++d) {
                    const int at = int(x) + d;
                    if (at < 0 || at >= int(width)) continue;
                    sum += line[at];
                    ++count;
                }
                row[x] = std::uint8_t(sum / count);
            }
            std::memcpy(line, row.data(), width);
        }
    }
    if (scene.gain != 1.0) {
        std::array<std::uint8_t, 256> table {};
        for (int v = 0; v < 256; ++v) table[std::size_t(v)] = clampByte(int(std::lround(v * scene.gain)));
        for (auto& v : out.luma) v = table[v];
    }
    if (scene.tintCb != 0 || scene.tintCr != 0)
        for (std::size_t at = 0; at + 1 < out.chroma.size(); at += 2) {
            out.chroma[at] = clampByte(out.chroma[at] + scene.tintCb);
            out.chroma[at + 1] = clampByte(out.chroma[at + 1] + scene.tintCr);
        }
    drawMarker(out.luma, out.chroma, width, scene.tag, scene.counter);
    return out;
}

// ----------------------------------------------------------------------------
// Bayer stills
// ----------------------------------------------------------------------------

std::vector<std::uint16_t> bayerStill(const Picture& chart, BayerOrder order, std::uint32_t seed)
{
    std::vector<std::uint16_t> raw(std::size_t(chart.width) * chart.height);
    // Which colour each of the four sites of a 2 x 2 cell has: 0 red, 1 green, 2 blue.
    static constexpr std::uint8_t sites[4][4] = {{0, 1, 1, 2}, {0, 0, 0, 0}, {1, 0, 2, 1}, {1, 2, 0, 1}};
    const std::uint8_t* pattern = order == BayerOrder::rggb ? sites[0] : order == BayerOrder::grbg ? sites[2]
        : order == BayerOrder::gbrg ? sites[3] : (static_cast<void>(0), nullptr);
    static constexpr std::uint8_t bggr[4] = {2, 1, 1, 0};
    if (order == BayerOrder::bggr) pattern = bggr;
    if (pattern == nullptr) return {};
    constexpr double weights[3] = {0.9, 1.0, 0.8};
    std::uint32_t state = seed ? seed : 1u;
    for (std::uint32_t y = 0; y < chart.height; ++y)
        for (std::uint32_t x = 0; x < chart.width; ++x) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            const int noise = int(state % 17) - 8;
            const double level = chart.luma[std::size_t(y) * chart.width + x] / 255.0 * weights[pattern[(y & 1) * 2 + (x & 1)]];
            raw[std::size_t(y) * chart.width + x] =
                std::uint16_t(std::clamp<int>(int(blackLevel + level * (whiteLevel - blackLevel)) + noise, int(blackLevel),
                                              int(whiteLevel)));
        }
    return raw;
}

CameraColorData syntheticColor(const CameraInfo& info, BayerOrder order)
{
    CameraColorData data;
    data.bayer = order;
    data.blackLevel = blackLevel;
    data.whiteLevel = whiteLevel;
    data.rawWidth = info.rawStillSize.width;
    data.rawHeight = info.rawStillSize.height;
    data.illuminant1 = 17;  // standard light A
    data.illuminant2 = 21;  // D65
    data.colorMatrix1 = {1.2, -0.3, -0.1, -0.4, 1.3, 0.1, -0.1, 0.2, 0.7};
    data.colorMatrix2 = {1.1, -0.2, -0.1, -0.3, 1.2, 0.1, -0.1, 0.2, 0.8};
    data.forwardMatrix1 = {0.7, 0.2, 0.1, 0.3, 0.7, 0.0, 0.0, 0.1, 0.8};
    data.forwardMatrix2 = {0.6, 0.3, 0.1, 0.3, 0.7, 0.0, 0.0, 0.0, 0.9};
    data.focalLengthMillimetres = info.lens == CameraLensKind::ultraWide ? 2.7 : info.lens == CameraLensKind::telephoto ? 9.0 : 6.9;
    data.intrinsics = {3000.0, 3000.0, info.pixelArrayWidth / 2.0, info.pixelArrayHeight / 2.0};
    return data;
}

// ----------------------------------------------------------------------------
// The camera and its sessions
// ----------------------------------------------------------------------------

class State;

struct RunningVideo {
    // The format it runs at.
    std::uint32_t width {};
    std::uint32_t height {};
    // What it was asked for, which the format covers.
    std::uint32_t requestedWidth {};
    std::uint32_t requestedHeight {};
    std::uint32_t frameRate {};
    std::uint32_t counter {};
    std::chrono::steady_clock::time_point next;
};

struct PendingStill {
    CameraStillRequest request;
};

struct CameraState {
    CameraSettings settings;
    std::optional<RunningVideo> video;
    std::deque<PendingStill> stills;
    std::uint32_t stillCounter {};
    std::uint32_t seenFocusSerial {};
    bool scanning {};
    std::map<std::pair<std::uint32_t, std::uint32_t>, Picture> bases;
};

class Core {
public:
    Core(const SyntheticCameraOptions& options, Profile profile) : options_(options), profile_(std::move(profile)) {}

    const SyntheticCameraOptions options_;
    const Profile profile_;
    std::atomic<unsigned> opened {0};
    std::atomic<unsigned> open {0};
    std::atomic<std::uint64_t> clock {1'000'000'000};
    std::atomic<bool> rawRefused {false};
    std::atomic<bool> rawFails {false};
    std::mutex mutex;  // guards the sessions list and the interrupted flag
    std::set<class Session*> sessions;
    bool interrupted {};
    CameraEventKind interruptKind {CameraEventKind::interrupted};
};

class Session final : public CameraSession {
public:
    Session(Core& core, CameraSessionDelegate& delegate) : core_(core), delegate_(delegate), cameras_(core.profile_.cameras.size())
    {
        {
            std::lock_guard lock(core_.mutex);
            core_.sessions.insert(this);
        }
        ++core_.opened;
        ++core_.open;
        if (!core_.options_.manualClock) worker_ = std::thread([this] { run(); });
    }

    ~Session() override { stop(); }

    bool startVideo(std::size_t camera, std::uint32_t width, std::uint32_t height, std::uint32_t frameRate) override
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || camera >= cameras_.size() || width == 0 || height == 0 || frameRate == 0) return false;
        std::set<std::size_t> running {camera};
        for (std::size_t index = 0; index < cameras_.size(); ++index)
            if (cameras_[index].video) running.insert(index);
        // Cameras that run together lie in one concurrent set and use formats
        // a multi-camera session takes; the ones already running move to such a
        // format, or the new one is refused and nothing changes.
        const bool several = running.size() > 1;
        if (several && !joinable(running)) return false;
        std::map<std::size_t, const CameraVideoFormat*> picks;
        for (const auto index : running) {
            const bool self = index == camera;
            const auto askedWidth = self ? width : cameras_[index].video->requestedWidth;
            const auto askedHeight = self ? height : cameras_[index].video->requestedHeight;
            const auto askedRate = self ? frameRate : cameras_[index].video->frameRate;
            const auto* pick = chooseFormat(core_.profile_.cameras[index], askedWidth, askedHeight, askedRate, several);
            if (pick == nullptr) return false;
            picks[index] = pick;
        }
        for (const auto& [index, pick] : picks) {
            auto& state = cameras_[index];
            const bool self = index == camera;
            if (!self && state.video->width == pick->width && state.video->height == pick->height) continue;
            RunningVideo video;
            video.width = pick->width;
            video.height = pick->height;
            video.requestedWidth = self ? width : state.video->requestedWidth;
            video.requestedHeight = self ? height : state.video->requestedHeight;
            video.frameRate = self ? frameRate : state.video->frameRate;
            video.next = std::chrono::steady_clock::now();
            if (state.video) video.counter = state.video->counter;
            state.video = video;
        }
        wake_.notify_all();
        return true;
    }

    void stopVideo(std::size_t camera) override
    {
        std::lock_guard lock(mutex_);
        if (camera >= cameras_.size() || !cameras_[camera].video) return;
        cameras_[camera].video.reset();
        // A camera left alone goes back to the smallest format of any kind.
        std::vector<std::size_t> running;
        for (std::size_t index = 0; index < cameras_.size(); ++index)
            if (cameras_[index].video) running.push_back(index);
        if (running.size() != 1) return;
        auto& state = cameras_[running.front()];
        const auto* pick = chooseFormat(core_.profile_.cameras[running.front()], state.video->requestedWidth,
                                        state.video->requestedHeight, state.video->frameRate, false);
        if (pick == nullptr || (pick->width == state.video->width && pick->height == state.video->height)) return;
        state.video->width = pick->width;
        state.video->height = pick->height;
    }

    void apply(std::size_t camera, const CameraSettings& settings) override
    {
        std::lock_guard lock(mutex_);
        if (!stopped_ && camera < cameras_.size()) cameras_[camera].settings = settings;
    }

    bool takeStill(std::size_t camera, const CameraStillRequest& request) override
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || camera >= cameras_.size()) return false;
        const auto& info = core_.profile_.cameras[camera];
        if (request.raw && (!info.rawStills || info.rawStillSize.width == 0 || core_.rawRefused)) return false;
        if (request.jpegWidth != 0 && !stillSize(info, request.jpegWidth, request.jpegHeight)) return false;
        if (request.jpegWidth == 0 && !request.raw) return false;
        cameras_[camera].stills.push_back({request});
        wake_.notify_all();
        return true;
    }

    void stop() override
    {
        {
            std::lock_guard lock(mutex_);
            if (stopped_) return;
            stopped_ = true;
            wake_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
        {
            std::lock_guard lock(core_.mutex);
            core_.sessions.erase(this);
        }
        --core_.open;
    }

    // Manual clock: a camera's pending stills, then `frames` frames.
    void pump(std::size_t camera, unsigned frames)
    {
        if (camera >= cameras_.size()) return;
        for (;;) {
            std::optional<CameraStillRequest> still;
            {
                std::lock_guard lock(mutex_);
                if (stopped_ || cameras_[camera].stills.empty()) break;
                still = cameras_[camera].stills.front().request;
                cameras_[camera].stills.pop_front();
            }
            produceStill(camera, *still);
        }
        for (unsigned frame = 0; frame < frames; ++frame) {
            std::uint32_t rate;
            {
                std::lock_guard lock(mutex_);
                if (stopped_ || !cameras_[camera].video) return;
                rate = cameras_[camera].video->frameRate;
            }
            if (!interrupted()) produceFrame(camera);
            core_.clock += 1'000'000'000ull / rate;
        }
    }

    CameraSettings settings(std::size_t camera) const
    {
        std::lock_guard lock(mutex_);
        return camera < cameras_.size() ? cameras_[camera].settings : CameraSettings {};
    }
    bool video(std::size_t camera) const
    {
        std::lock_guard lock(mutex_);
        return camera < cameras_.size() && cameras_[camera].video.has_value();
    }
    std::pair<std::uint32_t, std::uint32_t> videoSize(std::size_t camera) const
    {
        std::lock_guard lock(mutex_);
        if (camera >= cameras_.size() || !cameras_[camera].video) return {0, 0};
        return {cameras_[camera].video->width, cameras_[camera].video->height};
    }
    std::uint32_t rate(std::size_t camera) const
    {
        std::lock_guard lock(mutex_);
        return camera < cameras_.size() && cameras_[camera].video ? cameras_[camera].video->frameRate : 0;
    }
    void deliver(CameraEvent event) { delegate_.cameraEvent(event); }

private:
    // Whether the cameras that would run all lie in one concurrent set.
    bool joinable(const std::set<std::size_t>& running) const
    {
        return std::any_of(core_.profile_.sets.begin(), core_.profile_.sets.end(), [&](const auto& set) {
            return std::all_of(running.begin(), running.end(),
                               [&](auto member) { return std::find(set.begin(), set.end(), member) != set.end(); });
        });
    }

    // The smallest format that covers the size at the rate.
    static const CameraVideoFormat* chooseFormat(const CameraInfo& info, std::uint32_t width, std::uint32_t height,
                                                 std::uint32_t frameRate, bool multiCameraOnly)
    {
        const CameraVideoFormat* best = nullptr;
        for (const auto& candidate : info.videoFormats) {
            if (candidate.width < width || candidate.height < height) continue;
            if (frameRate < candidate.minimumFrameRate || frameRate > candidate.maximumFrameRate) continue;
            if (multiCameraOnly && !candidate.multiCamera) continue;
            if (!best || std::uint64_t(candidate.width) * candidate.height < std::uint64_t(best->width) * best->height)
                best = &candidate;
        }
        return best;
    }

    static const CameraStillSize* stillSize(const CameraInfo& info, std::uint32_t width, std::uint32_t height)
    {
        const CameraStillSize* best = nullptr;
        for (const auto& size : info.stillSizes) {
            if (size.width < width || size.height < height) continue;
            if (!best || std::uint64_t(size.width) * size.height < std::uint64_t(best->width) * best->height) best = &size;
        }
        return best;
    }

    // What the settings make of an exposure.
    struct Outcome {
        CameraExposure exposure;
        Scene scene;
    };
    Outcome outcome(std::size_t camera, std::uint32_t frameRate)
    {
        const auto& info = core_.profile_.cameras[camera];
        auto& state = cameras_[camera];
        const auto& settings = state.settings;
        Outcome result;
        auto& exposure = result.exposure;
        exposure.activeCamera = std::uint32_t(camera);
        exposure.aperture = info.aperture;
        exposure.frameDurationNanoseconds = frameRate ? 1'000'000'000ull / frameRate : 0;
        exposure.focalLengthMillimetres = 6.9f;
        exposure.noiseProfile = {1e-4f, 1e-6f, 1e-4f, 1e-6f, 1e-4f, 1e-6f, 1e-4f, 1e-6f};
        exposure.neutralPoint = {0.5f, 1.0f, 0.6f};
        // Exposure.
        if (settings.exposureMode == CameraExposureMode::manual && info.manualExposure) {
            exposure.exposureNanoseconds = std::clamp(settings.exposureNanoseconds, info.exposureMinimumNanoseconds,
                                                      info.exposureMaximumNanoseconds);
            exposure.iso = std::clamp(settings.iso, info.isoMinimum, info.isoMaximum);
            exposure.ae = CameraControlState::inactive;
            result.scene.gain = exposure.exposureNanoseconds / 1e9 * exposure.iso / referenceExposure;
        } else {
            exposure.exposureNanoseconds = 8'333'333;
            exposure.iso = std::max<std::uint32_t>(100, info.isoMinimum);
            exposure.ae = settings.exposureMode == CameraExposureMode::locked ? CameraControlState::locked
                                                                              : CameraControlState::converged;
            result.scene.gain = std::pow(2.0, settings.exposureBiasStops);
        }
        // Focus.
        if (info.autofocus) {
            if (settings.focusMode == CameraFocusMode::manual && info.manualFocus) {
                exposure.lensPositionDioptres = settings.focusDistanceDioptres;
                exposure.af = CameraControlState::locked;
            } else {
                exposure.lensPositionDioptres = 1.0f;
                exposure.af = settings.focusMode == CameraFocusMode::locked ? CameraControlState::locked
                                                                             : CameraControlState::converged;
                if (settings.focusTriggerSerial != state.seenFocusSerial) {
                    state.seenFocusSerial = settings.focusTriggerSerial;
                    exposure.af = CameraControlState::searching;
                }
            }
            result.scene.blur = std::min(8, int(std::lround(std::fabs(exposure.lensPositionDioptres - 1.0f) * 2.0f)));
        }
        // White balance.
        if (settings.whiteBalanceMode == CameraWhiteBalanceMode::manual && info.manualWhiteBalance) {
            exposure.whiteBalanceGains = settings.whiteBalanceGains;
            exposure.awb = CameraControlState::inactive;
        } else {
            exposure.whiteBalanceGains = {1.8f, 1.0f, 1.6f};
            exposure.awb = settings.whiteBalanceMode == CameraWhiteBalanceMode::locked ? CameraControlState::locked
                                                                                       : CameraControlState::converged;
        }
        exposure.colorTemperature = std::uint32_t(std::clamp(5500.0f + (exposure.whiteBalanceGains[2] - exposure.whiteBalanceGains[0]) * 1000.0f,
                                                             2000.0f, 10000.0f));
        result.scene.tintCb = int(std::lround((exposure.whiteBalanceGains[2] / exposure.whiteBalanceGains[1] - 1.6f) * 20.0f));
        result.scene.tintCr = int(std::lround((exposure.whiteBalanceGains[0] / exposure.whiteBalanceGains[1] - 1.8f) * 20.0f));
        // Zoom, and the lens a zoom camera has switched to.
        exposure.zoomRatio = std::clamp(settings.zoomRatio, info.zoomMinimum, info.zoomMaximum);
        result.scene.zoom = std::max(exposure.zoomRatio, 1.0f);
        if (info.lens == CameraLensKind::zoom && !info.constituents.empty()) {
            std::size_t lensIndex = 0;
            for (const auto switchOver : info.switchOvers)
                if (exposure.zoomRatio >= switchOver) ++lensIndex;
            lensIndex = std::min(lensIndex, info.constituents.size() - 1);
            for (std::size_t other = 0; other < core_.profile_.cameras.size(); ++other)
                if (core_.profile_.cameras[other].identifier == info.constituents[lensIndex])
                    exposure.activeCamera = std::uint32_t(other);
        }
        result.scene.tag = chartTag(camera);
        return result;
    }

    const Picture& base(CameraState& state, std::uint32_t width, std::uint32_t height)
    {
        auto found = state.bases.find({width, height});
        if (found == state.bases.end()) {
            if (state.bases.size() > 4) state.bases.clear();
            found = state.bases.emplace(std::make_pair(width, height), baseChart(width, height)).first;
        }
        return found->second;
    }

    void produceFrame(std::size_t camera)
    {
        CameraFrame frame;
        {
            std::lock_guard lock(mutex_);
            auto& state = cameras_[camera];
            if (stopped_ || !state.video) return;
            auto outcome = this->outcome(camera, state.video->frameRate);
            outcome.scene.counter = ++state.video->counter;
            auto picture = std::make_shared<Picture>(render(base(state, state.video->width, state.video->height), outcome.scene));
            frame.width = picture->width;
            frame.height = picture->height;
            frame.luma = picture->luma;
            frame.lumaStride = picture->width;
            frame.chroma = picture->chroma;
            frame.chromaStride = picture->width;
            frame.owner = picture;
            frame.exposure = outcome.exposure;
        }
        const auto latency = std::uint64_t(core_.options_.latencyMilliseconds) * 1'000'000;
        const auto now = core_.options_.manualClock ? core_.clock.load() : realNow();
        frame.timestampNanoseconds = now > latency ? now - latency : 0;
        delegate_.cameraFrame(camera, std::move(frame));
    }

    bool interrupted() const
    {
        std::lock_guard lock(core_.mutex);
        return core_.interrupted;
    }

    void produceStill(std::size_t camera, const CameraStillRequest& request)
    {
        const auto& info = core_.profile_.cameras[camera];
        CameraStillResult result;
        result.token = request.token;
        if (!interrupted()) {
            std::lock_guard lock(mutex_);
            auto& state = cameras_[camera];
            auto outcome = this->outcome(camera, 0);
            outcome.scene.counter = ++state.stillCounter;
            outcome.exposure.flashFired = request.flash == CameraFlashMode::on && info.flash;
            result.exposure = outcome.exposure;
            if (request.jpegWidth != 0) {
                const auto* size = stillSize(info, request.jpegWidth, request.jpegHeight);
                const auto picture = render(base(state, size->width, size->height), outcome.scene);
                result.jpeg = encodeSyntheticJpeg(picture.luma.data(), picture.width, picture.chroma.data(),
                                                  picture.width, picture.width, picture.height, request.jpegQuality);
            }
            if (request.raw && !core_.rawFails) {
                const auto picture = render(base(state, info.rawStillSize.width, info.rawStillSize.height), outcome.scene);
                result.raw = bayerStill(picture, BayerOrder::rggb, 0x5eed0000u + state.stillCounter);
                result.color = syntheticColor(info, BayerOrder::rggb);
            }
            result.ok = (request.jpegWidth != 0 && !result.jpeg.empty()) || (request.raw && !result.raw.empty());
        }
        delegate_.cameraStill(camera, std::move(result));
    }

    void run()
    {
        std::unique_lock lock(mutex_);
        while (!stopped_) {
            const auto now = std::chrono::steady_clock::now();
            std::optional<std::chrono::steady_clock::time_point> wake;
            std::vector<std::size_t> due;
            std::vector<std::pair<std::size_t, CameraStillRequest>> stills;
            for (std::size_t camera = 0; camera < cameras_.size(); ++camera) {
                auto& state = cameras_[camera];
                while (!state.stills.empty()) {
                    stills.emplace_back(camera, state.stills.front().request);
                    state.stills.pop_front();
                }
                if (!state.video) continue;
                if (state.video->next <= now) {
                    due.push_back(camera);
                    state.video->next += std::chrono::nanoseconds(1'000'000'000ull / state.video->frameRate);
                    if (state.video->next < now) state.video->next = now;
                }
                if (!wake || state.video->next < *wake) wake = state.video->next;
            }
            if (!due.empty() || !stills.empty()) {
                lock.unlock();
                for (auto& [camera, request] : stills) produceStill(camera, request);
                for (const auto camera : due) {
                    if (!interrupted()) produceFrame(camera);
                }
                lock.lock();
                continue;
            }
            if (wake) wake_.wait_until(lock, *wake);
            else wake_.wait(lock);
        }
    }

    Core& core_;
    CameraSessionDelegate& delegate_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<CameraState> cameras_;
    std::thread worker_;
    bool stopped_ {};
};

}  // namespace
}  // namespace uml::platform

namespace uml::platform {
namespace {

class SyntheticCameraImpl final : public SyntheticCamera {
public:
    explicit SyntheticCameraImpl(const SyntheticCameraOptions& options)
        : core_(options, profileOf(options.profile))
    {
    }

    std::vector<CameraInfo> cameras() const override { return core_.profile_.cameras; }
    std::vector<std::vector<std::size_t>> concurrentSets() const override { return core_.profile_.sets; }
    std::string hostModel() const override { return core_.profile_.model; }
    std::uint64_t now() const noexcept override { return core_.options_.manualClock ? core_.clock.load() : realNow(); }
    std::unique_ptr<Nv12Transformer> makeFrameTransformer() const override { return makePortableNv12Transformer(); }

    std::unique_ptr<CameraSession> openSession(CameraSessionDelegate& delegate) override
    {
        if (!core_.options_.authorized) return nullptr;
        auto session = std::make_unique<Session>(core_, delegate);
        // The cause of an interruption outlives a session: one opened while it
        // lasts is interrupted as it starts, as the host's is.
        CameraEventKind kind {};
        bool interrupted;
        {
            std::lock_guard lock(core_.mutex);
            interrupted = core_.interrupted;
            kind = core_.interruptKind;
        }
        if (interrupted) session->deliver({allCameras, kind});
        return session;
    }

    unsigned sessionsOpened() const noexcept override { return core_.opened; }
    unsigned sessionsOpen() const noexcept override { return core_.open; }

    CameraSettings appliedSettings(std::size_t camera) const override
    {
        return withSessions([&](Session& session) { return session.settings(camera); }, CameraSettings {});
    }
    bool videoRunning(std::size_t camera) const override
    {
        return withSessions([&](Session& session) { return session.video(camera); }, false);
    }
    std::uint32_t videoFrameRate(std::size_t camera) const override
    {
        return withSessions([&](Session& session) { return session.rate(camera); }, std::uint32_t(0));
    }

    void pump(std::size_t camera, unsigned frames) override
    {
        std::vector<Session*> sessions;
        {
            std::lock_guard lock(core_.mutex);
            sessions.assign(core_.sessions.begin(), core_.sessions.end());
        }
        for (auto* session : sessions) session->pump(camera, frames);
    }

    void advanceClock(std::uint64_t nanoseconds) override { core_.clock += nanoseconds; }

    void interrupt(CameraEventKind kind) override
    {
        std::vector<Session*> sessions;
        {
            std::lock_guard lock(core_.mutex);
            core_.interrupted = true;
            core_.interruptKind = kind;
            sessions.assign(core_.sessions.begin(), core_.sessions.end());
        }
        for (auto* session : sessions) session->deliver({allCameras, kind});
    }

    std::pair<std::uint32_t, std::uint32_t> videoSize(std::size_t camera) const override
    {
        return withSessions([&](Session& session) { return session.videoSize(camera); },
                            std::pair<std::uint32_t, std::uint32_t> {0, 0});
    }

    void post(CameraEvent event) override
    {
        std::vector<Session*> sessions;
        {
            std::lock_guard lock(core_.mutex);
            sessions.assign(core_.sessions.begin(), core_.sessions.end());
        }
        for (auto* session : sessions) session->deliver(event);
    }

    void refuseRaw(bool refuse) override { core_.rawRefused = refuse; }
    void failRaw(bool fail) override { core_.rawFails = fail; }

    void resume() override
    {
        std::vector<Session*> sessions;
        {
            std::lock_guard lock(core_.mutex);
            core_.interrupted = false;
            sessions.assign(core_.sessions.begin(), core_.sessions.end());
        }
        for (auto* session : sessions) session->deliver({allCameras, CameraEventKind::resumed});
    }

private:
    template<typename Function, typename Result>
    Result withSessions(Function function, Result fallback) const
    {
        std::lock_guard lock(core_.mutex);
        for (auto* session : core_.sessions) return function(*session);
        return fallback;
    }

    mutable Core core_;
};

}  // namespace

std::unique_ptr<SyntheticCamera> makeSyntheticCamera(const SyntheticCameraOptions& options)
{
    return std::make_unique<SyntheticCameraImpl>(options);
}

std::uint32_t markerCell(std::uint32_t width) noexcept
{
    return std::max<std::uint32_t>(2, (width / markerCells) & ~1u);
}

std::uint16_t chartTag(std::size_t camera) noexcept
{
    return std::uint16_t(0x5a00u + (camera & 0xffu));
}

ChartMarker decodeChartMarker(const std::uint8_t* luma, std::size_t stride, std::uint32_t width,
                              std::uint32_t height) noexcept
{
    ChartMarker marker;
    const auto cell = markerCell(width);
    if (luma == nullptr || height < markerHeight || width < cell * markerCells) return marker;
    std::uint64_t bits = 0;
    for (std::uint32_t index = 0; index < markerCells; ++index) {
        // The cell's centre.
        const auto sample = luma[std::size_t(markerHeight / 2) * stride + std::size_t(index) * cell + cell / 2];
        bits = (bits << 1) | (sample >= 128 ? 1u : 0u);
    }
    const std::uint16_t check = std::uint16_t(bits);
    const std::uint64_t first48 = bits >> 16;
    marker.tag = std::uint16_t(first48 >> 32);
    marker.counter = std::uint32_t(first48);
    marker.valid = check == std::uint16_t((first48 >> 32) ^ (first48 >> 16) ^ first48);
    return marker;
}

}  // namespace uml::platform
