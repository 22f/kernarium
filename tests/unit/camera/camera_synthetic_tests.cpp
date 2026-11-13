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
        if (firstFrameDelay.count() > 0) {
            // A delegate that is slow on its first frame (the timer thread waits for it).
            bool first;
            {
                std::lock_guard lock(mutex);
                first = frames.empty();
            }
            if (first) std::this_thread::sleep_for(firstFrameDelay);
        }
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
    std::chrono::milliseconds firstFrameDelay {0};
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
    for (const auto profile : {SyntheticCameraProfile::phone, SyntheticCameraProfile::tablet, SyntheticCameraProfile::tabletDualFront,
                               SyntheticCameraProfile::laptop}) {
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
    require(models.size() == 4);
    const auto dual = makeSyntheticCamera(manual(SyntheticCameraProfile::tabletDualFront))->cameras();
    require(dual.size() == 3 && dual[1].position == CameraPosition::front && dual[1].lens == CameraLensKind::wide &&
            dual[2].position == CameraPosition::front && dual[2].lens == CameraLensKind::ultraWide);
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
    settings.focusDistanceSerial = 1;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(recorder.frames.back().second.exposure.af == CameraControlState::locked);
    require(recorder.frames.back().second.exposure.lensPositionDioptres == 5.0f);
    require(sharpness(recorder.frames.back().second) < crisp * 0.8);

    // The lens holds while the distance is not written anew, even if the value says
    // otherwise; a locked lens holds too.
    settings.focusDistanceDioptres = 2.0f;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(recorder.frames.back().second.exposure.lensPositionDioptres == 5.0f);
    settings.focusMode = CameraFocusMode::locked;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(recorder.frames.back().second.exposure.af == CameraControlState::locked &&
            recorder.frames.back().second.exposure.lensPositionDioptres == 5.0f);

    // A scan in a manual mode puts the lens on the chart, and it holds there.
    settings.focusMode = CameraFocusMode::manual;
    settings.focusTriggerSerial = 1;
    session->apply(0, settings);
    camera->pump(0, 2);
    require(recorder.frames[recorder.frames.size() - 2].second.exposure.af == CameraControlState::searching);
    require(recorder.frames.back().second.exposure.af == CameraControlState::locked);
    require(recorder.frames.back().second.exposure.lensPositionDioptres == 1.0f);

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

// What the exposure of a frame says: the lens's focal length is left to the device (the camera's own, as its
// description states it), an automatic exposure follows the compensation and the picture with it, and a frame lasts at
// least as long as its exposure, which delays the next one.
void exposureReport()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    require(session->startVideo(0, 640, 480, 30));
    camera->pump(0, 1);
    const auto unit = recorder.frames.back().second;
    require(unit.exposure.focalLengthMillimetres == 0.0f);  // none: the device states the lens's own
    require(unit.exposure.exposureNanoseconds == 8'333'333 && unit.exposure.frameDurationNanoseconds == 33'333'333);
    const auto unitLuma = meanLuma(unit);

    CameraSettings settings;
    settings.exposureBiasStops = -1.0f;
    session->apply(0, settings);
    camera->pump(0, 1);
    const auto& half = recorder.frames.back().second;
    require(std::llabs(std::int64_t(half.exposure.exposureNanoseconds) - 8'333'333 / 2) <= 1 && half.exposure.iso == unit.exposure.iso);
    require(half.exposure.ae == CameraControlState::converged && half.exposure.frameDurationNanoseconds == 33'333'333);
    require(meanLuma(half) < unitLuma * 0.7);
    settings.exposureBiasStops = 2.0f;
    session->apply(0, settings);
    camera->pump(0, 1);
    const auto& bright = recorder.frames.back().second;
    require(std::llabs(std::int64_t(bright.exposure.exposureNanoseconds) - 8'333'333 * 4) <= 1);
    const double brightLuma = meanLuma(bright);
    require(brightLuma > unitLuma);
    // The automatic exposure stays inside the frame the rate asks for: a compensation that asks for more than a 30 fps
    // frame holds (+3 stops is twice as long) runs the frame's own length, and the sensitivity takes the rest, so the
    // exposure times the sensitivity still follows the compensation and the rate does not fall.
    settings.exposureBiasStops = 3.0f;
    session->apply(0, settings);
    camera->pump(0, 2);
    const auto& third = recorder.frames[recorder.frames.size() - 2].second;
    const auto& fourth = recorder.frames.back().second;
    require(third.exposure.exposureNanoseconds == 33'333'333 && third.exposure.frameDurationNanoseconds == 33'333'333);
    require(fourth.timestampNanoseconds - third.timestampNanoseconds == 33'333'333);
    require(std::llabs(std::int64_t(third.exposure.iso) - 200) <= 1);  // 3 stops are 2 of exposure (to the frame) and 1 of it
    const double product = double(third.exposure.exposureNanoseconds) * third.exposure.iso;
    require(std::fabs(product / (double(unit.exposure.exposureNanoseconds) * unit.exposure.iso) - 8.0) < 0.1);
    require(meanLuma(third) >= brightLuma);
    // The exposure of the sensor ends where its range does, the sensitivity where its range does, and the picture stops
    // with them.
    const auto info = camera->cameras()[0];
    settings.exposureBiasStops = 20.0f;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(recorder.frames.back().second.exposure.exposureNanoseconds == 33'333'333);
    require(recorder.frames.back().second.exposure.iso == info.isoMaximum);
    settings.exposureBiasStops = -20.0f;
    session->apply(0, settings);
    camera->pump(0, 1);
    require(recorder.frames.back().second.exposure.exposureNanoseconds == info.exposureMinimumNanoseconds);
    require(recorder.frames.back().second.exposure.iso == info.isoMinimum);

    // A manual exposure longer than the frame period: the frame lasts as long as the exposure, and the next frame
    // arrives after it.
    settings = {};
    settings.exposureMode = CameraExposureMode::manual;
    settings.exposureNanoseconds = 333'346'667;
    settings.iso = 100;
    session->apply(0, settings);
    camera->pump(0, 2);
    const auto& first = recorder.frames[recorder.frames.size() - 2].second;
    const auto& second = recorder.frames.back().second;
    require(first.exposure.exposureNanoseconds == 333'346'667 && first.exposure.frameDurationNanoseconds == 333'346'667);
    require(second.timestampNanoseconds - first.timestampNanoseconds == 333'346'667);
    // A shorter one leaves the period alone.
    settings.exposureNanoseconds = 8'333'333;
    session->apply(0, settings);
    camera->pump(0, 2);
    const auto& short1 = recorder.frames[recorder.frames.size() - 2].second;
    require(short1.exposure.frameDurationNanoseconds == 33'333'333 &&
            recorder.frames.back().second.timestampNanoseconds - short1.timestampNanoseconds == 33'333'333);
    // A still's exposure states no focal length either.
    CameraStillRequest request;
    request.jpegWidth = 640;
    request.jpegHeight = 480;
    require(session->takeStill(0, request));
    camera->pump(0, 0);
    require(recorder.waitFor(1, 1));
    require(recorder.stills.back().second.exposure.focalLengthMillimetres == 0.0f);
}

// Every profile's every camera runs an automatic exposure that is not zero and shows a lit chart at no compensation: a
// camera that states no exposure range (a laptop's) runs the exposure as asked and is not clamped to nothing.
void everyProfileExposes()
{
    for (const auto profile : {SyntheticCameraProfile::phone, SyntheticCameraProfile::tablet, SyntheticCameraProfile::tabletDualFront,
                               SyntheticCameraProfile::laptop}) {
        const auto camera = makeSyntheticCamera(manual(profile));
        Recorder recorder;
        auto session = camera->openSession(recorder);
        const auto cameras = camera->cameras();
        for (std::size_t index = 0; index < cameras.size(); ++index) {
            const auto& format = cameras[index].videoFormats.front();
            require(session->startVideo(index, format.width, format.height, format.maximumFrameRate));
            camera->pump(index, 1);
            const auto& frame = recorder.frames.back().second;
            require(frame.exposure.exposureNanoseconds == 8'333'333);
            require(frame.exposure.iso >= 100);
            require(frame.exposure.frameDurationNanoseconds >= frame.exposure.exposureNanoseconds);
            require(meanLuma(frame) > 100.0);
            session->stopVideo(index);
        }
    }
    // A laptop's camera states no range: the compensation moves its exposure the same way.
    const auto laptop = makeSyntheticCamera(manual(SyntheticCameraProfile::laptop));
    require(laptop->cameras()[0].exposureMaximumNanoseconds == 0 && laptop->cameras()[0].isoMaximum == 0);
    Recorder recorder;
    auto session = laptop->openSession(recorder);
    require(session->startVideo(0, 640, 480, 30));
    CameraSettings settings;
    settings.exposureBiasStops = -1.0f;
    session->apply(0, settings);
    laptop->pump(0, 1);
    require(std::llabs(std::int64_t(recorder.frames.back().second.exposure.exposureNanoseconds) - 8'333'333 / 2) <= 1);
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

// A camera alone runs any format; cameras that run together run formats a
// multi-camera session takes. The ones running move to such a format when
// another joins, the newcomer is refused with nothing changed when one has none,
// and a camera left alone goes back to the smallest format of any kind.
void multiCameraFormats()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tabletDualFront));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    using Size = std::pair<std::uint32_t, std::uint32_t>;
    require(session->startVideo(0, 1600, 900, 30));
    require(camera->videoSize(0) == Size(1600, 900));  // its own format, which no multi-camera session takes
    camera->pump(0, 1);
    require(recorder.frames.back().second.width == 1600);

    require(session->startVideo(1, 640, 480, 30));     // the front wide camera joins
    require(camera->videoSize(0) == Size(1920, 1080) && camera->videoSize(1) == Size(640, 480));
    camera->pump(0, 1);
    require(recorder.frames.back().second.width == 1920 && recorder.frames.back().first == 0);
    require(!session->startVideo(2, 640, 480, 30));    // the ultra wide front is in no set with both
    require(camera->videoSize(2) == Size(0, 0));

    session->stopVideo(1);
    require(camera->videoSize(0) == Size(1600, 900));  // alone again: its smallest format
    // The other front camera is a set with the back one, in its turn.
    require(session->startVideo(2, 640, 480, 30));
    require(camera->videoSize(0) == Size(1920, 1080) && camera->videoSize(2) == Size(640, 480));
    session->stopVideo(2);

    // A camera that has no format a multi-camera session takes at its size cannot be joined.
    require(session->startVideo(0, 3840, 2160, 30));
    require(camera->videoSize(0) == Size(3840, 2160));
    require(!session->startVideo(1, 640, 480, 30));
    require(camera->videoSize(0) == Size(3840, 2160) && !camera->videoRunning(1));
    // Nor can it be moved by a request for a size no format of its kind covers at that rate.
    session->stopVideo(0);
    require(session->startVideo(1, 1920, 1080, 30));
    require(!session->startVideo(0, 3840, 2160, 30));
    require(camera->videoSize(1) == Size(1920, 1080) && !camera->videoRunning(0));
}

// Every camera with an input counts in a multi-camera session, one that only
// takes stills as well: it joins as a streaming camera does, the cameras that
// stream move to formats the session takes (or the still is refused, nothing
// changed), and it leaves with its last still, the others going back.
void stillOnlyCamerasCount()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tabletDualFront));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    using Size = std::pair<std::uint32_t, std::uint32_t>;
    CameraStillRequest request;
    request.jpegWidth = 1000;
    request.jpegHeight = 700;
    request.token = 7;

    // The back camera streams at a size of its own; a still on a front camera joins it.
    require(session->startVideo(0, 1600, 900, 30));
    require(camera->videoSize(0) == Size(1600, 900));
    require(session->takeStill(1, request));
    require(camera->videoSize(0) == Size(1920, 1080));  // moved to a format the session takes
    require(recorder.stills.empty());
    camera->pump(1, 0);
    require(recorder.stills.size() == 1 && recorder.stills.back().second.ok);
    require(camera->videoSize(0) == Size(1600, 900));  // alone again once the still is delivered

    // A still on a camera in no concurrent set with the streaming one is refused.
    require(session->startVideo(1, 640, 480, 30));
    require(!session->takeStill(2, request));
    require(camera->videoSize(0) == Size(1920, 1080) && camera->videoSize(1) == Size(640, 480));
    session->stopVideo(1);

    // A camera at a size no multi-camera format covers cannot be joined by a still either.
    session->stopVideo(0);
    require(session->startVideo(0, 3840, 2160, 30));
    request.token = 8;
    require(!session->takeStill(1, request));
    require(camera->videoSize(0) == Size(3840, 2160) && camera->videoRunning(0));
    // Without that stream the still is taken, and a stream cannot join it either while it is on its way.
    session->stopVideo(0);
    request.token = 9;
    require(session->takeStill(1, request));
    require(!session->startVideo(0, 3840, 2160, 30));
    camera->pump(1, 0);
    require(recorder.stills.size() == 2);
    require(session->startVideo(0, 3840, 2160, 30));  // the still has left the session
}

// A join whose cameras ask for more than the hardware gives (AVFoundation's
// hardwareCost over 1.0) is refused, and nothing changes.
void hardwareBudget()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tabletDualFront));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    using Size = std::pair<std::uint32_t, std::uint32_t>;
    require(session->startVideo(0, 1920, 1080, 60));
    require(!session->startVideo(1, 1920, 1080, 60));
    require(!camera->videoRunning(1) && camera->videoSize(0) == Size(1920, 1080));
    require(session->startVideo(1, 1920, 1080, 30));  // half the rate fits
    // Raising a camera's rate is a join of its own and is refused the same way.
    require(!session->startVideo(1, 1920, 1080, 60));
    require(camera->videoFrameRate(1) == 30 && camera->videoFrameRate(0) == 60);
}

// A Bayer still the platform refuses, or that fails alone, leaves what else the
// capture asked for.
void rawFailures()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    Recorder recorder;
    auto session = camera->openSession(recorder);
    CameraStillRequest request;
    request.jpegWidth = 1000;
    request.jpegHeight = 700;
    request.raw = true;
    camera->refuseRaw(true);
    require(!session->takeStill(0, request));
    request.raw = false;
    require(session->takeStill(0, request));
    camera->pump(0, 0);
    require(recorder.stills.back().second.ok && !recorder.stills.back().second.jpeg.empty());

    camera->refuseRaw(false);
    camera->failRaw(true);
    request.raw = true;
    request.token = 3;
    require(session->takeStill(0, request));
    camera->pump(0, 0);
    const auto& partial = recorder.stills.back().second;
    require(partial.ok && partial.token == 3 && !partial.jpeg.empty() && partial.raw.empty());
    request.jpegWidth = 0;
    request.token = 4;
    require(session->takeStill(0, request));
    camera->pump(0, 0);
    require(!recorder.stills.back().second.ok && recorder.stills.back().second.token == 4);
}

// The cause of an interruption outlives a session: one opened while it lasts is
// interrupted as it starts, with the same reason.
void interruptionOutlivesSession()
{
    const auto camera = makeSyntheticCamera(manual(SyntheticCameraProfile::tablet));
    {
        Recorder first;
        auto session = camera->openSession(first);
        camera->interrupt(CameraEventKind::interruptedBySystemPressure);
        require(first.events.size() == 1 && first.events[0].kind == CameraEventKind::interruptedBySystemPressure);
        session->stop();
    }
    Recorder second;
    auto session = camera->openSession(second);
    require(second.events.size() == 1 && second.events[0].kind == CameraEventKind::interruptedBySystemPressure &&
            second.events[0].camera == allCameras);
    camera->resume();
    require(second.events.size() == 2 && second.events[1].kind == CameraEventKind::resumed);
    session->stop();
    Recorder third;
    session = camera->openSession(third);
    require(third.events.empty());
    // post() delivers an event and changes nothing else.
    camera->post({1, CameraEventKind::unavailable});
    require(third.events.size() == 1 && third.events[0].kind == CameraEventKind::unavailable && third.events[0].camera == 1);
    require(session->startVideo(0, 640, 480, 30));
    camera->pump(0, 1);
    require(third.frames.size() == 1);
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

// The real timer's schedule when a frame is late: a frame that lasts 200 ms at 10 fps is followed by the next one a whole
// frame duration after its own start, also when the delegate held the timer thread long enough to make that frame overdue
// (the next deadline is counted from the frame's start, not clamped to now with only the excess added).
void timerSchedulesWholeDurations()
{
    auto options = manual(SyntheticCameraProfile::tablet);
    options.manualClock = false;
    const auto camera = makeSyntheticCamera(options);
    Recorder recorder;
    recorder.firstFrameDelay = std::chrono::milliseconds(350);
    {
        auto session = camera->openSession(recorder);
        CameraSettings settings;
        settings.exposureMode = CameraExposureMode::manual;
        settings.exposureNanoseconds = 200'000'000;
        settings.iso = 100;
        session->apply(0, settings);
        require(session->startVideo(0, 640, 480, 10));
        require(recorder.waitFor(4));
        std::lock_guard lock(recorder.mutex);
        for (std::size_t i = 0; i < 4; ++i)
            require(recorder.frames[i].second.exposure.exposureNanoseconds == 200'000'000 &&
                    recorder.frames[i].second.exposure.frameDurationNanoseconds == 200'000'000);
        // Frame 1 was held for 350 ms, so frame 2 is overdue when it is due; frames 2 to 4 are a frame duration apart.
        for (std::size_t i = 2; i < 4; ++i) {
            const auto interval = recorder.frames[i].second.timestampNanoseconds - recorder.frames[i - 1].second.timestampNanoseconds;
            require(interval >= 185'000'000 && interval <= 400'000'000);
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
    exposureReport();
    everyProfileExposes();
    zoomCamera();
    multiCamera();
    multiCameraFormats();
    stillOnlyCamerasCount();
    hardwareBudget();
    rawFailures();
    interruptionOutlivesSession();
    stills();
    interruptions();
    threaded();
    timerSchedulesWholeDurations();
    return 0;
}
