// SPDX-License-Identifier: GPL-2.0-or-later
#include "require.hpp"
#include "synthetic_camera.hpp"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace uml::platform;
using uml::testing::require;

namespace {

class Recorder final : public CameraSessionDelegate {
public:
    void cameraFrame(std::size_t camera, CameraFrame frame) override
    {
        std::lock_guard lock(mutex);
        frames.emplace_back(camera, std::move(frame));
        changed.notify_all();
    }
    void cameraStill(std::size_t camera, CameraStillResult result) override
    {
        std::lock_guard lock(mutex);
        stills.emplace_back(camera, std::move(result));
        changed.notify_all();
    }
    void cameraEvent(CameraEvent event) override
    {
        std::lock_guard lock(mutex);
        events.push_back(event);
        changed.notify_all();
    }
    bool waitFor(std::size_t framesWanted, std::size_t stillsWanted = 0)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(10),
                                [&] { return frames.size() >= framesWanted && stills.size() >= stillsWanted; });
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<std::size_t, CameraFrame>> frames;
    std::vector<std::pair<std::size_t, CameraStillResult>> stills;
    std::vector<CameraEvent> events;
};

double meanLuma(const CameraFrame& frame)
{
    // Below the marker rows.
    double sum = 0;
    std::size_t count = 0;
    for (std::uint32_t y = 16; y < frame.height; ++y)
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            sum += frame.luma[std::size_t(y) * frame.lumaStride + x];
            ++count;
        }
    return sum / double(count);
}

double sharpness(const CameraFrame& frame)
{
    double sum = 0;
    for (std::uint32_t y = 16; y < frame.height; ++y)
        for (std::uint32_t x = 1; x < frame.width; ++x)
            sum += std::pow(double(frame.luma[std::size_t(y) * frame.lumaStride + x]) -
                                double(frame.luma[std::size_t(y) * frame.lumaStride + x - 1]), 2.0);
    return sum;
}

double meanChroma(const CameraFrame& frame, unsigned component)
{
    double sum = 0;
    std::size_t count = 0;
    for (std::uint32_t y = 8; y < frame.height / 2; ++y)
        for (std::uint32_t x = 0; x < frame.width; x += 2) {
            sum += frame.chroma[std::size_t(y) * frame.chromaStride + x + component];
            ++count;
        }
    return sum / double(count);
}

SyntheticCameraOptions manual(SyntheticCameraProfile profile)
{
    SyntheticCameraOptions options;
    options.profile = profile;
    options.manualClock = true;
    return options;
}

void profiles()
{
    std::set<std::string> models;
    for (const auto profile : {SyntheticCameraProfile::phone, SyntheticCameraProfile::tablet, SyntheticCameraProfile::laptop}) {
        const auto camera = makeSyntheticCamera(manual(profile));
        const auto cameras = camera->cameras();
        require(!cameras.empty());
        models.insert(camera->hostModel());
        std::set<std::string> identifiers;
        for (const auto& info : cameras) {
            require(identifiers.insert(info.identifier).second);
            require(!info.videoFormats.empty() && !info.stillSizes.empty());
            require(info.pixelArrayWidth > 0 && info.sensorOrientation % 90 == 0);
        }
        for (const auto& set : camera->concurrentSets()) {
            require(set.size() > 1);
            for (const auto member : set) require(member < cameras.size());
        }
        // A zoom camera names lenses that exist.
        for (const auto& info : cameras)
            for (const auto& name : info.constituents) {
                bool found = false;
                for (const auto& other : cameras) found |= other.identifier == name;
                require(found);
            }
    }
    require(models.size() == 3);
    const auto phone = makeSyntheticCamera(manual(SyntheticCameraProfile::phone))->cameras();
    require(phone.size() == 5 && phone[0].lens == CameraLensKind::zoom && !phone[0].manualExposure);
    require(phone[1].manualExposure && phone[1].rawStills && phone[1].stillSizes.front().width == 8064);
    require(makeSyntheticCamera(manual(SyntheticCameraProfile::laptop))->cameras().size() == 1);
}

void unauthorized()
{
    auto options = manual(SyntheticCameraProfile::tablet);
    options.authorized = false;
    const auto camera = makeSyntheticCamera(options);
    Recorder recorder;
    require(camera->openSession(recorder) == nullptr);
    require(camera->sessionsOpened() == 0 && camera->sessionsOpen() == 0);
    require(camera->cameras().size() == 2);  // discovery needs no authorization
}

void video()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    require(session != nullptr && camera->sessionsOpened() == 1 && camera->sessionsOpen() == 1);
    require(!session->startVideo(0, 640, 480, 500));     // no format runs that fast
    require(!session->startVideo(0, 8000, 6000, 30));    // no format is that large
    require(!session->startVideo(9, 640, 480, 30));      // no such camera
    require(session->startVideo(0, 600, 400, 30));       // the smallest covering format
    require(camera->videoRunning(0) && !camera->videoRunning(1) && camera->videoFrameRate(0) == 30);
    const auto start = camera->now();
    camera->pump(0, 3);
    require(recorder.frames.size() == 3);
    std::uint32_t expected = 1;
    for (const auto& [index, frame] : recorder.frames) {
        require(index == 0 && frame.width == 640 && frame.height == 480);
        require(frame.lumaStride >= frame.width && frame.luma.size() >= frame.lumaStride * frame.height);
        const auto marker = decodeChartMarker(frame.luma.data(), frame.lumaStride, frame.width, frame.height);
        require(marker.valid && marker.tag == chartTag(0) && marker.counter == expected);
        require(frame.exposure.frameDurationNanoseconds == 33'333'333);
        require(frame.exposure.ae == CameraControlState::converged && frame.exposure.iso >= 25);
        require(frame.timestampNanoseconds == start + (expected - 1) * 33'333'333ull);
        ++expected;
    }
    // Another camera's chart carries its own tag.
    require(session->startVideo(1, 640, 480, 30));  // front and back are one set
    camera->pump(1, 1);
    const auto& other = recorder.frames.back();
    require(other.first == 1 && decodeChartMarker(other.second.luma.data(), other.second.lumaStride, 640, 480).tag == chartTag(1));
    session->stopVideo(0);
    require(!camera->videoRunning(0));
    camera->pump(0, 2);
    require(recorder.frames.size() == 4);
    session->stop();
    require(camera->sessionsOpen() == 0 && camera->sessionsOpened() == 1);
    require(!session->startVideo(0, 640, 480, 30));  // a stopped session starts nothing
}

void controls()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    require(session->startVideo(0, 640, 480, 30));
    camera->pump(0, 1);
    const auto automatic = meanLuma(recorder.frames.back().second);
    const auto crisp = sharpness(recorder.frames.back().second);
    const auto autoCb = meanChroma(recorder.frames.back().second, 0);

    CameraSettings settings;
    settings.exposureMode = CameraExposureMode::manual;
    settings.exposureNanoseconds = 8'333'333 / 2;
    settings.iso = 100;
    session->apply(0, settings);
    camera->pump(0, 1);
    const auto& dim = recorder.frames.back().second;
    require(dim.exposure.exposureNanoseconds == 8'333'333 / 2 && dim.exposure.iso == 100 && dim.exposure.ae == CameraControlState::inactive);
    require(meanLuma(dim) < automatic * 0.7);
    settings.exposureNanoseconds = 8'333'333;
    settings.iso = 200;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(meanLuma(recorder.frames.back().second) > automatic * 1.3);
    require(camera->appliedSettings(0) == settings);

    // A manual focus away from the chart's distance blurs the picture.
    settings = {};
    settings.focusMode = CameraFocusMode::manual;
    settings.focusDistanceDioptres = 5.0f;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(recorder.frames.back().second.exposure.af == CameraControlState::locked);
    require(recorder.frames.back().second.exposure.lensPositionDioptres == 5.0f);
    require(sharpness(recorder.frames.back().second) < crisp * 0.8);

    // White balance moves the chroma.
    settings = {};
    settings.whiteBalanceMode = CameraWhiteBalanceMode::manual;
    settings.whiteBalanceGains = {1.0f, 1.0f, 3.0f};
    session->apply(0, settings);
    camera->pump(0, 1);
    require(meanChroma(recorder.frames.back().second, 0) > autoCb + 5);
    require(recorder.frames.back().second.exposure.awb == CameraControlState::inactive);

    // A trigger makes one scan.
    settings = {};
    settings.focusTriggerSerial = 1;
    session->apply(0, settings);
    camera->pump(0, 2);
    require(recorder.frames[recorder.frames.size() - 2].second.exposure.af == CameraControlState::searching);
    require(recorder.frames.back().second.exposure.af == CameraControlState::converged);

    // Digital zoom enlarges the middle: the marker is gone from view.
    settings = {};
    settings.zoomRatio = 2.0f;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(recorder.frames.back().second.exposure.zoomRatio == 2.0f);
}

void zoomCamera()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::phone));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    require(session->startVideo(0, 640, 480, 30));
    // A zoom camera takes automatic exposure only, and reports the lens in use.
    CameraSettings settings;
    settings.exposureMode = CameraExposureMode::manual;
    settings.exposureNanoseconds = 1'000'000;
    settings.iso = 800;
    settings.zoomRatio = 0.5f;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(recorder.frames.back().second.exposure.activeCamera == 2);  // ultra wide
    require(recorder.frames.back().second.exposure.ae == CameraControlState::converged);
    settings.zoomRatio = 2.0f;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(recorder.frames.back().second.exposure.activeCamera == 1);  // wide
    settings.zoomRatio = 5.0f;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(recorder.frames.back().second.exposure.activeCamera == 3);  // telephoto
}

void multiCamera()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::phone));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    require(session->startVideo(4, 1280, 720, 30));    // the front camera
    require(session->startVideo(1, 1280, 720, 30));    // wide: in a set with the front
    require(!session->startVideo(2, 1280, 720, 30));   // ultra wide would not share a set with both
    require(!session->startVideo(3, 640, 480, 30));    // nor would the telephoto
    session->stopVideo(1);
    require(session->startVideo(2, 1280, 720, 30));    // front and ultra wide are a set
    // A format a multi-camera session does not take is refused while two run.
    require(!session->startVideo(2, 3840, 2160, 30));
    session->stopVideo(4);
    require(session->startVideo(2, 3840, 2160, 30));   // alone, it may use it
}

void stills()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    CameraStillRequest request;
    request.token = 7;
    request.jpegWidth = 1000;
    request.jpegHeight = 700;
    require(session->takeStill(0, request));
    require(recorder.stills.empty());  // arrives on the camera's next tick
    camera->pump(0, 0);
    require(recorder.stills.size() == 1);
    const auto& jpeg = recorder.stills[0].second;
    require(jpeg.ok && jpeg.token == 7 && jpeg.raw.empty());
    require(jpeg.jpeg.size() > 1000 && jpeg.jpeg[0] == 0xff && jpeg.jpeg[1] == 0xd8);
    // An Exif segment follows the start of image or the JFIF segment.
    bool exif = false;
    for (std::size_t at = 2; at + 10 < jpeg.jpeg.size() && at < 200; ++at)
        exif |= jpeg.jpeg[at] == 0xff && jpeg.jpeg[at + 1] == 0xe1 && jpeg.jpeg[at + 4] == 'E' && jpeg.jpeg[at + 5] == 'x';
    require(exif);

    // The Bayer still, with the colour data of the sensor.
    request = {};
    request.token = 8;
    request.raw = true;
    require(session->takeStill(0, request));
    camera->pump(0, 0);
    const auto& raw = recorder.stills.back().second;
    require(raw.ok && raw.token == 8 && raw.jpeg.empty());
    const auto info = camera->cameras()[0];
    require(raw.raw.size() == std::size_t(info.rawStillSize.width) * info.rawStillSize.height);
    require(raw.color.bayer == BayerOrder::rggb && raw.color.whiteLevel > raw.color.blackLevel);
    require(raw.color.rawWidth == info.rawStillSize.width && raw.color.colorMatrix1[0] != 0);
    for (const auto v : raw.raw) require(v >= raw.color.blackLevel && v <= raw.color.whiteLevel);
    // Red sites are brighter than blue ones in a chart with equal weights of both.
    double red = 0, blue = 0;
    for (std::uint32_t y = 100; y + 1 < info.rawStillSize.height; y += 2)
        for (std::uint32_t x = 0; x + 1 < info.rawStillSize.width; x += 2) {
            red += raw.raw[std::size_t(y) * info.rawStillSize.width + x];
            blue += raw.raw[std::size_t(y + 1) * info.rawStillSize.width + x + 1];
        }
    require(red > blue);
    // JPEG and Bayer in one capture.
    request.jpegWidth = 1000;
    request.jpegHeight = 700;
    request.token = 9;
    require(session->takeStill(0, request));
    camera->pump(0, 0);
    require(recorder.stills.back().second.ok && !recorder.stills.back().second.jpeg.empty() && !recorder.stills.back().second.raw.empty());

    // What the camera cannot take is refused at the request.
    request = {};
    request.jpegWidth = 9000;
    request.jpegHeight = 9000;
    require(!session->takeStill(0, request));
    request = {};
    require(!session->takeStill(0, request));
    request.raw = true;
    require(!session->takeStill(1, request));  // the front camera has no Bayer still
}

void interruptions()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    require(session->startVideo(0, 640, 480, 30));
    camera->pump(0, 1);
    require(recorder.frames.size() == 1);
    camera->interrupt(CameraEventKind::interruptedByMultitasking);
    require(recorder.events.size() == 1 && recorder.events[0].kind == CameraEventKind::interruptedByMultitasking &&
            recorder.events[0].camera == allCameras);
    camera->pump(0, 3);
    require(recorder.frames.size() == 1);  // nothing flows while interrupted
    CameraStillRequest request;
    request.jpegWidth = 1000;
    request.jpegHeight = 700;
    require(session->takeStill(0, request));
    camera->pump(0, 0);
    require(!recorder.stills.back().second.ok);
    camera->resume();
    require(recorder.events.size() == 2 && recorder.events[1].kind == CameraEventKind::resumed);
    camera->pump(0, 1);
    require(recorder.frames.size() == 2);
}

void threaded()
{
    auto options = manual(SyntheticCameraProfile::tablet);
    options.manualClock = false;
    options.latencyMilliseconds = 5;
    const auto camera = makeSyntheticCamera(options);
    Recorder recorder;
    {
        auto session = camera->openSession(recorder);
        require(session->startVideo(0, 640, 480, 60));
        require(recorder.waitFor(4));
        CameraStillRequest request;
        request.jpegWidth = 1000;
        request.jpegHeight = 700;
        require(session->takeStill(0, request));
        require(recorder.waitFor(4, 1));
        std::lock_guard lock(recorder.mutex);
        require(recorder.stills[0].second.ok);
        // The exposure began a few milliseconds before delivery.
        for (const auto& [index, frame] : recorder.frames) {
            (void)index;
            require(frame.timestampNanoseconds + 4'000'000 <= camera->now());
        }
    }
    require(camera->sessionsOpen() == 0);
}

} // namespace

int main()
{
    profiles();
    unauthorized();
    video();
    controls();
    zoomCamera();
    multiCamera();
    stills();
    interruptions();
    threaded();
    return 0;
}
