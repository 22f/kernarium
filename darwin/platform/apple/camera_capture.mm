// SPDX-License-Identifier: GPL-2.0-or-later
//
// The platform Camera over AVFoundation. Discovery reads devices and formats
// and needs no authorization. A session is one AVCaptureMultiCamSession on iOS
// (an AVCaptureSession on macOS, which has no multi-camera session) with one
// input, one video data output and, once a still is asked for, one photo
// output per running camera.

#include "accelerated_nv12.hpp"
#include "camera.hpp"
#include "dng_color.hpp"
#include "photo_metadata.hpp"
#include "photo_dimensions.hpp"
#include "still_exposure.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <TargetConditionals.h>
#include <sys/sysctl.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uml::platform {
namespace {

std::uint64_t hostNow() noexcept
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

std::string stringOf(NSString* value)
{
    return value ? std::string(value.UTF8String) : std::string();
}

[[maybe_unused]] CameraControlState controlState(bool adjusting, bool locked)
{
    if (adjusting) return CameraControlState::searching;
    return locked ? CameraControlState::locked : CameraControlState::converged;
}

#if TARGET_OS_IOS
// The conversion between a lens position and dioptres. The lens position runs
// from 0, the nearest focus, to 1, infinity, without a calibration, so the
// dioptres are an approximation that the nearest focus distance anchors.
float dioptresOf(float lensPosition, float minimumFocusMillimetres)
{
    if (minimumFocusMillimetres <= 0) return 0.0f;
    return (1.0f - std::clamp(lensPosition, 0.0f, 1.0f)) * 1000.0f / minimumFocusMillimetres;
}

float lensPositionOf(float dioptres, float minimumFocusMillimetres)
{
    if (minimumFocusMillimetres <= 0) return 1.0f;
    return std::clamp(1.0f - dioptres * minimumFocusMillimetres / 1000.0f, 0.0f, 1.0f);
}
#endif

struct Entry {
    AVCaptureDevice* device {};
    CameraInfo info;
    // AVFoundation's zoom factor times this is the ratio Android sees.
    float zoomMultiplier {1.0f};
};

[[maybe_unused]] BayerOrder bayerOrderOf(OSType format)
{
    switch (format) {
    case kCVPixelFormatType_14Bayer_RGGB: return BayerOrder::rggb;
    case kCVPixelFormatType_14Bayer_GRBG: return BayerOrder::grbg;
    case kCVPixelFormatType_14Bayer_GBRG: return BayerOrder::gbrg;
    case kCVPixelFormatType_14Bayer_BGGR: return BayerOrder::bggr;
    default: return BayerOrder::none;
    }
}

std::vector<Entry> discover()
{
    NSMutableArray<AVCaptureDeviceType>* types =
        [NSMutableArray arrayWithObjects:AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal, nil];
#if TARGET_OS_IOS
    [types addObjectsFromArray:@[
        AVCaptureDeviceTypeBuiltInUltraWideCamera, AVCaptureDeviceTypeBuiltInTelephotoCamera,
        AVCaptureDeviceTypeBuiltInDualCamera, AVCaptureDeviceTypeBuiltInDualWideCamera,
        AVCaptureDeviceTypeBuiltInTripleCamera
    ]];
    // The foldable's two ultra wide cameras exist from iOS 27.1; on 27.0 the
    // constants are null, so they are named only where they exist. The
    // application's Camera row asks the same types for a front camera
    // (Andarium's SystemCameraSource.hasFrontCamera).
    if (@available(iOS 27.1, *)) {
        [types addObjectsFromArray:@[
            AVCaptureDeviceTypeBuiltInOuterUltraWideCamera, AVCaptureDeviceTypeBuiltInInnerUltraWideCamera
        ]];
    }
#endif
    AVCaptureDeviceDiscoverySession* session =
        [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                                                               mediaType:AVMediaTypeVideo
                                                                position:AVCaptureDevicePositionUnspecified];
    std::vector<Entry> entries;
    for (AVCaptureDevice* device in session.devices) {
        Entry entry;
        entry.device = device;
        auto& info = entry.info;
        info.identifier = stringOf(device.uniqueID);
        switch (device.position) {
        case AVCaptureDevicePositionBack: info.position = CameraPosition::back; break;
        case AVCaptureDevicePositionFront: info.position = CameraPosition::front; break;
        default: info.position = CameraPosition::external; break;
        }
        const AVCaptureDeviceType type = device.deviceType;
        info.lens = CameraLensKind::external;
        if ([type isEqualToString:AVCaptureDeviceTypeBuiltInWideAngleCamera]) info.lens = CameraLensKind::wide;
#if TARGET_OS_IOS
        bool ultraWide = [type isEqualToString:AVCaptureDeviceTypeBuiltInUltraWideCamera];
        if (@available(iOS 27.1, *))
            ultraWide = ultraWide || [type isEqualToString:AVCaptureDeviceTypeBuiltInOuterUltraWideCamera] ||
                [type isEqualToString:AVCaptureDeviceTypeBuiltInInnerUltraWideCamera];
        if (ultraWide) info.lens = CameraLensKind::ultraWide;
        else if ([type isEqualToString:AVCaptureDeviceTypeBuiltInTelephotoCamera]) info.lens = CameraLensKind::telephoto;
        else if (device.virtualDevice) info.lens = CameraLensKind::zoom;
        if (info.position == CameraPosition::external) info.lens = CameraLensKind::external;
#endif
        info.autofocus = [device isFocusModeSupported:AVCaptureFocusModeContinuousAutoFocus];
        // A front camera's flash is the screen's, which no guest camera unit models.
        info.flash = device.hasFlash && info.position != CameraPosition::front;
        info.torch = device.hasTorch;
        const bool zoomCamera = info.lens == CameraLensKind::zoom;

        // Formats: distinct size and frame-rate range pairs of the 4:2:0 formats.
        std::uint32_t pixelWidth = 0, pixelHeight = 0;
        float fieldOfView = 0;
#if TARGET_OS_IOS
        std::uint32_t isoMinimum = 0xffffffffu, isoMaximum = 0;
        double exposureMinimum = 1e9, exposureMaximum = 0;
#endif
        CameraStillSize largestPhoto;
        for (AVCaptureDeviceFormat* format in device.formats) {
            const FourCharCode subtype = CMFormatDescriptionGetMediaSubType(format.formatDescription);
            if (subtype != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange &&
                subtype != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
                continue;
            const CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
            if (dimensions.width <= 0 || dimensions.height <= 0) continue;
            bool multiCamera = false;
#if TARGET_OS_IOS
            multiCamera = format.multiCamSupported;
            fieldOfView = std::max(fieldOfView, format.videoFieldOfView);
            isoMinimum = std::min<std::uint32_t>(isoMinimum, std::uint32_t(std::ceil(format.minISO)));
            isoMaximum = std::max<std::uint32_t>(isoMaximum, std::uint32_t(std::floor(format.maxISO)));
            exposureMinimum = std::min(exposureMinimum, CMTimeGetSeconds(format.minExposureDuration));
            exposureMaximum = std::max(exposureMaximum, CMTimeGetSeconds(format.maxExposureDuration));
#endif
            for (NSValue* value in format.supportedMaxPhotoDimensions) {
                CMVideoDimensions photo;
                [value getValue:&photo];
                if (photo.width <= 0 || photo.height <= 0) continue;
                const CameraStillSize size {std::uint32_t(photo.width), std::uint32_t(photo.height)};
                if (std::find(info.stillSizes.begin(), info.stillSizes.end(), size) == info.stillSizes.end())
                    info.stillSizes.push_back(size);
                if (std::uint64_t(size.width) * size.height > std::uint64_t(largestPhoto.width) * largestPhoto.height)
                    largestPhoto = size;
            }
            if (std::uint64_t(dimensions.width) * dimensions.height > std::uint64_t(pixelWidth) * pixelHeight) {
                pixelWidth = std::uint32_t(dimensions.width);
                pixelHeight = std::uint32_t(dimensions.height);
            }
            for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges) {
                CameraVideoFormat candidate;
                candidate.width = std::uint32_t(dimensions.width);
                candidate.height = std::uint32_t(dimensions.height);
                candidate.minimumFrameRate = std::max<std::uint32_t>(1, std::uint32_t(std::ceil(range.minFrameRate)));
                candidate.maximumFrameRate = std::uint32_t(std::floor(range.maxFrameRate));
                candidate.multiCamera = multiCamera;
                if (candidate.maximumFrameRate < candidate.minimumFrameRate) continue;
                if (std::find(info.videoFormats.begin(), info.videoFormats.end(), candidate) == info.videoFormats.end())
                    info.videoFormats.push_back(candidate);
            }
        }
        if (info.videoFormats.empty()) continue;
        std::sort(info.stillSizes.begin(), info.stillSizes.end(), [](const auto& a, const auto& b) {
            return std::uint64_t(a.width) * a.height > std::uint64_t(b.width) * b.height;
        });
        info.pixelArrayWidth = std::max(pixelWidth, largestPhoto.width);
        info.pixelArrayHeight = std::max(pixelHeight, largestPhoto.height);
        info.horizontalFieldOfView = fieldOfView;

#if TARGET_OS_IOS
        info.isoMinimum = isoMinimum == 0xffffffffu ? 0 : isoMinimum;
        info.isoMaximum = isoMaximum;
        info.exposureMinimumNanoseconds = exposureMinimum >= 1e9 ? 0 : std::uint64_t(exposureMinimum * 1e9);
        info.exposureMaximumNanoseconds = std::uint64_t(exposureMaximum * 1e9);
        info.aperture = device.lensAperture;
        info.minimumFocusDistanceMillimetres = device.minimumFocusDistance > 0 ? float(device.minimumFocusDistance) : 0.0f;
        info.maximumWhiteBalanceGain = device.maxWhiteBalanceGain;
        info.sensorOrientation = info.position == CameraPosition::front ? 270 : 90;
        // A virtual device refuses manual exposure, focus and white balance.
        if (!zoomCamera) {
            info.manualExposure = [device isExposureModeSupported:AVCaptureExposureModeCustom];
            info.manualFocus = info.autofocus && device.lockingFocusWithCustomLensPositionSupported;
            info.manualWhiteBalance = device.lockingWhiteBalanceWithCustomDeviceGainsSupported;
        }
        entry.zoomMultiplier = device.displayVideoZoomFactorMultiplier > 0 ? float(device.displayVideoZoomFactorMultiplier)
                                                                          : 1.0f;
        info.zoomMinimum = float(device.minAvailableVideoZoomFactor) * entry.zoomMultiplier;
        info.zoomMaximum = float(device.maxAvailableVideoZoomFactor) * entry.zoomMultiplier;
        for (NSNumber* factor in device.virtualDeviceSwitchOverVideoZoomFactors)
            info.switchOvers.push_back(factor.floatValue * entry.zoomMultiplier);
        for (AVCaptureDevice* lens in device.constituentDevices) info.constituents.push_back(stringOf(lens.uniqueID));
        // A Bayer still is possible on a single back lens; whether the host
        // delivers one is learned by taking it.
        info.rawStills = !zoomCamera && info.position == CameraPosition::back;
        if (info.rawStills) {
            const std::uint64_t area = std::uint64_t(largestPhoto.width) * largestPhoto.height;
            // 48 MP sensors give 12 MP Bayer.
            const bool binned = area > 24'000'000;
            info.rawStillSize = {binned ? largestPhoto.width / 2 : largestPhoto.width,
                                 binned ? largestPhoto.height / 2 : largestPhoto.height};
        }
#else
        (void)zoomCamera;
#endif
        entries.push_back(std::move(entry));
    }
    // Back cameras first (the zoom camera, then the lenses), then front, then
    // the rest, each group in a fixed lens order.
    const auto rank = [](const CameraInfo& info) {
        const int position = info.position == CameraPosition::back ? 0 : info.position == CameraPosition::front ? 1 : 2;
        return position * 10 + int(info.lens);
    };
    std::stable_sort(entries.begin(), entries.end(),
                     [&](const Entry& a, const Entry& b) { return rank(a.info) < rank(b.info); });
    return entries;
}

class AppleSession;

// One camera's output delegate. It forwards to the session through a state
// that outlives it, so a callback that races the session's end finds it gone.
struct SessionState {
    std::mutex mutex;
    AppleSession* session {};
};

} // namespace
} // namespace uml::platform

namespace {
using uml::platform::SessionState;
}

// ---------------------------------------------------------------------------
// Objective-C delegates
// ---------------------------------------------------------------------------

@interface UMLCameraOutputDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
- (instancetype)initWithCamera:(size_t)camera state:(std::shared_ptr<SessionState>)state;
@end

@interface UMLPhotoDelegate : NSObject <AVCapturePhotoCaptureDelegate>
- (instancetype)initWithCamera:(size_t)camera
                         token:(uint64_t)token
                      wantJpeg:(BOOL)wantJpeg
                      jpegSize:(uml::platform::CameraStillSize)jpegSize
                       wantRaw:(BOOL)wantRaw
                         state:(std::shared_ptr<SessionState>)state;
@end

#if TARGET_OS_IOS
// Reports the level of a device's system pressure (heat, mostly) as it changes.
@interface UMLPressureObserver : NSObject
- (instancetype)initWithDevice:(AVCaptureDevice*)device handler:(void (^)(AVCaptureSystemPressureLevel))handler;
- (void)invalidate;
@end
#endif

namespace uml::platform {
namespace {

// The photo sizes a device format lists (supportedMaxPhotoDimensions).
std::vector<CameraStillSize> listedPhotoSizes(AVCaptureDeviceFormat* format)
{
    std::vector<CameraStillSize> listed;
    for (NSValue* value in format.supportedMaxPhotoDimensions) {
        CMVideoDimensions candidate;
        [value getValue:&candidate];
        if (candidate.width > 0 && candidate.height > 0)
            listed.push_back({std::uint32_t(candidate.width), std::uint32_t(candidate.height)});
    }
    return listed;
}

// What the system's pressure leaves of the rate a camera was asked for. The
// pressure observer reads and writes it on any thread.
struct PressureThrottle {
    std::atomic<std::uint32_t> requestedRate {0};
    // The most frames a second the pressure allows; zero is no limit.
    std::atomic<std::uint32_t> cap {0};
};

struct RunningCamera {
    AVCaptureDeviceInput* input {};
    AVCaptureVideoDataOutput* videoOutput {};
    AVCapturePhotoOutput* photoOutput {};
    UMLCameraOutputDelegate* delegate {};
    AVCaptureConnection* connection {};
    AVCaptureDeviceFormat* format {};
#if TARGET_OS_IOS
    UMLPressureObserver* pressure {};
#endif
    std::shared_ptr<PressureThrottle> throttle {std::make_shared<PressureThrottle>()};
    bool video {};
    CameraSettings settings;
    // What the camera was asked for, which its format covers.
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t frameRate {};
    std::uint32_t focusSerial {};
    std::uint32_t focusDistanceSerial {};
    // Settings have been applied to the device at least once, so that a format
    // change (which reverts some of them) can put them back.
    bool applied {};
};

// The rate a device runs at while the pressure caps it: the one asked for,
// lowered to the cap, and no lower than the format allows.
std::uint32_t throttledRate(const PressureThrottle& throttle, AVCaptureDeviceFormat* format)
{
    std::uint32_t rate = throttle.requestedRate;
    const std::uint32_t cap = throttle.cap;
    if (cap != 0 && cap < rate) rate = cap;
    double floorRate = 1.0;
    for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges) floorRate = std::max(floorRate, range.minFrameRate);
    return std::max<std::uint32_t>(rate, std::uint32_t(std::ceil(floorRate)));
}

// Sets a device's format and frame rate; false when the device refuses either.
bool setFormat(AVCaptureDevice* device, AVCaptureDeviceFormat* format, std::uint32_t frameRate)
{
    NSError* error = nil;
    if (![device lockForConfiguration:&error]) return false;
    bool ok = true;
    @try {
        // A format the session does not take raises instead of failing.
        device.activeFormat = format;
        const CMTime duration = CMTimeMake(1, std::int32_t(frameRate));
        device.activeVideoMinFrameDuration = duration;
        device.activeVideoMaxFrameDuration = duration;
    } @catch (NSException*) {
        ok = false;
    }
    [device unlockForConfiguration];
    return ok;
}

// Sets a device's format alone (a camera that only takes stills has no rate).
bool setActiveFormat(AVCaptureDevice* device, AVCaptureDeviceFormat* format)
{
    NSError* error = nil;
    if (![device lockForConfiguration:&error]) return false;
    bool ok = true;
    @try {
        device.activeFormat = format;
    } @catch (NSException*) {
        ok = false;
    }
    [device unlockForConfiguration];
    return ok;
}

// Sets the frame rate of a device's current format and nothing else: setting
// the format again would revert the device's focus and exposure points.
[[maybe_unused]] bool setFrameRate(AVCaptureDevice* device, std::uint32_t frameRate)
{
    NSError* error = nil;
    if (![device lockForConfiguration:&error]) return false;
    bool ok = true;
    @try {
        const CMTime duration = CMTimeMake(1, std::int32_t(frameRate));
        device.activeVideoMinFrameDuration = duration;
        device.activeVideoMaxFrameDuration = duration;
    } @catch (NSException*) {
        ok = false;
    }
    [device unlockForConfiguration];
    return ok;
}

class AppleCamera;

class AppleSession final : public CameraSession {
public:
    AppleSession(const std::vector<Entry>& entries, CameraSessionDelegate& delegate)
        : entries_(entries), delegate_(delegate), state_(std::make_shared<SessionState>())
    {
        state_->session = this;
#if TARGET_OS_IOS
        if (AVCaptureMultiCamSession.multiCamSupported) {
            session_ = [[AVCaptureMultiCamSession alloc] init];
            multiCamera_ = true;
        } else {
            session_ = [[AVCaptureSession alloc] init];
        }
#else
        session_ = [[AVCaptureSession alloc] init];
#endif
        queue_ = dispatch_queue_create("uml.camera.capture", DISPATCH_QUEUE_SERIAL);
        outputQueue_ = dispatch_queue_create("uml.camera.frames", DISPATCH_QUEUE_SERIAL);
        // The map never changes shape afterwards: the session's queue and the
        // thread that calls it reach a camera's state without searching.
        for (std::size_t index = 0; index < entries_.size(); ++index) cameras_[index];
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        auto state = state_;
        const auto post = [state](CameraEvent event) {
            std::lock_guard lock(state->mutex);
            if (state->session) state->session->deliver(event);
        };
        interrupted_ = [center addObserverForName:AVCaptureSessionWasInterruptedNotification
                                           object:session_
                                            queue:nil
                                       usingBlock:^(NSNotification* note) {
                                           CameraEventKind kind = CameraEventKind::interrupted;
#if TARGET_OS_IOS
                                           NSNumber* reason = note.userInfo[AVCaptureSessionInterruptionReasonKey];
                                           if (reason.integerValue ==
                                               AVCaptureSessionInterruptionReasonVideoDeviceNotAvailableWithMultipleForegroundApps)
                                               kind = CameraEventKind::interruptedByMultitasking;
                                           else if (reason.integerValue ==
                                                    AVCaptureSessionInterruptionReasonVideoDeviceNotAvailableDueToSystemPressure)
                                               kind = CameraEventKind::interruptedBySystemPressure;
#else
                                           (void)note;
#endif
                                           post({allCameras, kind});
                                       }];
        resumed_ = [center addObserverForName:AVCaptureSessionInterruptionEndedNotification
                                       object:session_
                                        queue:nil
                                   usingBlock:^(NSNotification*) {
                                       post({allCameras, CameraEventKind::resumed});
                                   }];
        // A restart after a runtime error ends with the session running again,
        // which is no interruption ending: the capture tells its client itself.
        // The blocks hold locals only: a block that read an instance variable
        // would hold this object, which may be gone when the notification comes.
        auto restarting = restarting_;
        AVCaptureSession* observed = session_;
        dispatch_queue_t queue = queue_;
        failed_ = [center addObserverForName:AVCaptureSessionRuntimeErrorNotification
                                      object:session_
                                       queue:nil
                                  usingBlock:^(NSNotification*) {
                                      // A media-services reset is the recoverable case: one
                                      // restart for an episode of errors, which ends when the
                                      // session runs again. A start that fails once more (a
                                      // hardware cost over budget, say) loses the capture; the
                                      // client closes the session and tries later.
                                      if (restarting->exchange(true)) {
                                          post({allCameras, CameraEventKind::failed});
                                          return;
                                      }
                                      post({allCameras, CameraEventKind::interrupted});
                                      dispatch_async(queue, ^{
                                          @try {
                                              if (!observed.running) [observed startRunning];
                                          } @catch (NSException*) {
                                              post({allCameras, CameraEventKind::failed});
                                          }
                                      });
                                  }];
        started_ = [center addObserverForName:AVCaptureSessionDidStartRunningNotification
                                       object:session_
                                        queue:nil
                                   usingBlock:^(NSNotification*) {
                                       if (restarting->exchange(false)) post({allCameras, CameraEventKind::resumed});
                                   }];
    }

    ~AppleSession() override { stop(); }

    // Runs `work` on the session's queue and waits for it. A lambda keeps the
    // references it captures as references, which a block would copy.
    void onQueue(const std::function<void()>& work) { dispatch_sync(queue_, ^{ work(); }); }

    bool startVideo(std::size_t camera, std::uint32_t width, std::uint32_t height, std::uint32_t frameRate) override
    {
        if (stopped_ || camera >= entries_.size() || width == 0 || height == 0 || frameRate == 0) return false;
        bool ok = false;
        onQueue([&] { ok = startVideoOnQueue(camera, width, height, frameRate); });
        return ok;
    }

    void stopVideo(std::size_t camera) override
    {
        if (stopped_ || camera >= entries_.size()) return;
        onQueue([&] {
            auto& running = cameras_[camera];
            if (!running.video) return;
            // A camera that no longer streams leaves the session, and so does the
            // photo output made for it unless a still is still on its way; the
            // camera's indicator and its share of a multi-camera budget go with it.
            removeFromSession(running, true, !stillsInFlight(camera));
            endPressureWatch(running);
            returnAlone();
        });
    }

    void apply(std::size_t camera, const CameraSettings& settings) override
    {
        if (stopped_ || camera >= entries_.size()) return;
        onQueue([&] { applyOnQueue(camera, settings, false); });
    }

    // Puts a camera's settings on its device: all of them again when `force`
    // (a format change reverts some), or what changed since the last time.
    void applyOnQueue(std::size_t camera, const CameraSettings& settings, bool force)
    {
        auto& running = cameras_[camera];
        if (!force && running.settings == settings && running.focusSerial == settings.focusTriggerSerial) return;
        const Entry& entry = entries_[camera];
        AVCaptureDevice* device = entry.device;
        NSError* error = nil;
        if (![device lockForConfiguration:&error]) return;
#if TARGET_OS_IOS
        @try {
            const CameraInfo& info = entry.info;
            // Exposure.
            if (settings.exposureMode == CameraExposureMode::manual && info.manualExposure) {
                const CMTime duration = CMTimeMake(std::int64_t(settings.exposureNanoseconds), 1'000'000'000);
                const float iso = std::clamp<float>(float(settings.iso), device.activeFormat.minISO, device.activeFormat.maxISO);
                [device setExposureModeCustomWithDuration:duration ISO:iso completionHandler:nil];
            } else if (settings.exposureMode == CameraExposureMode::locked) {
                if ([device isExposureModeSupported:AVCaptureExposureModeLocked])
                    device.exposureMode = AVCaptureExposureModeLocked;
            } else if ([device isExposureModeSupported:AVCaptureExposureModeContinuousAutoExposure]) {
                device.exposureMode = AVCaptureExposureModeContinuousAutoExposure;
            }
            if (settings.exposureMode != CameraExposureMode::manual)
                [device setExposureTargetBias:std::clamp(settings.exposureBiasStops, device.minExposureTargetBias,
                                                         device.maxExposureTargetBias)
                            completionHandler:nil];
            if (settings.aeRegion && settings.aeRegion->weight > 0 && device.exposurePointOfInterestSupported)
                device.exposurePointOfInterest = CGPointMake(settings.aeRegion->x + settings.aeRegion->width / 2,
                                                             settings.aeRegion->y + settings.aeRegion->height / 2);
            // Focus. The point of interest comes first: the device takes it up when the
            // focus mode is set. A new trigger runs one scan in any mode (the device
            // locks where it ends). Otherwise a manual lens goes to the distance only
            // when the guest has written it anew, and holds from then on, as does a
            // locked one (only a continuous focus is locked, so that a later apply
            // does not abort a scan under way); the continuous mode is the rest. A
            // format change reverts the focus, so `force` puts a written distance
            // back.
            if (info.autofocus) {
                if (settings.afRegion && settings.afRegion->weight > 0 && device.focusPointOfInterestSupported)
                    device.focusPointOfInterest = CGPointMake(settings.afRegion->x + settings.afRegion->width / 2,
                                                              settings.afRegion->y + settings.afRegion->height / 2);
                const bool scan = settings.focusTriggerSerial != running.focusSerial;
                const bool move = settings.focusDistanceSerial != running.focusDistanceSerial ||
                    (force && settings.focusDistanceSerial != 0);
                if (scan && [device isFocusModeSupported:AVCaptureFocusModeAutoFocus]) {
                    device.focusMode = AVCaptureFocusModeAutoFocus;
                } else if (settings.focusMode == CameraFocusMode::manual && info.manualFocus) {
                    if (move) {
                        [device setFocusModeLockedWithLensPosition:lensPositionOf(settings.focusDistanceDioptres,
                                                                                  info.minimumFocusDistanceMillimetres)
                                                 completionHandler:nil];
                    } else if (device.focusMode == AVCaptureFocusModeContinuousAutoFocus &&
                               [device isFocusModeSupported:AVCaptureFocusModeLocked]) {
                        device.focusMode = AVCaptureFocusModeLocked;
                    }
                } else if (settings.focusMode != CameraFocusMode::continuousAuto) {
                    if (device.focusMode == AVCaptureFocusModeContinuousAutoFocus &&
                        [device isFocusModeSupported:AVCaptureFocusModeLocked])
                        device.focusMode = AVCaptureFocusModeLocked;
                } else if ([device isFocusModeSupported:AVCaptureFocusModeContinuousAutoFocus]) {
                    device.focusMode = AVCaptureFocusModeContinuousAutoFocus;
                }
            }
            // White balance.
            if (settings.whiteBalanceMode == CameraWhiteBalanceMode::manual && info.manualWhiteBalance) {
                AVCaptureWhiteBalanceGains gains;
                const float maximum = device.maxWhiteBalanceGain;
                gains.redGain = std::clamp(settings.whiteBalanceGains[0], 1.0f, maximum);
                gains.greenGain = std::clamp(settings.whiteBalanceGains[1], 1.0f, maximum);
                gains.blueGain = std::clamp(settings.whiteBalanceGains[2], 1.0f, maximum);
                [device setWhiteBalanceModeLockedWithDeviceWhiteBalanceGains:gains completionHandler:nil];
            } else if (settings.whiteBalanceMode == CameraWhiteBalanceMode::locked) {
                if ([device isWhiteBalanceModeSupported:AVCaptureWhiteBalanceModeLocked])
                    device.whiteBalanceMode = AVCaptureWhiteBalanceModeLocked;
            } else if ([device isWhiteBalanceModeSupported:AVCaptureWhiteBalanceModeContinuousAutoWhiteBalance]) {
                device.whiteBalanceMode = AVCaptureWhiteBalanceModeContinuousAutoWhiteBalance;
            }
            // Zoom.
            const float factor = std::clamp(settings.zoomRatio / entry.zoomMultiplier, float(device.minAvailableVideoZoomFactor),
                                            float(device.maxAvailableVideoZoomFactor));
            if (std::fabs(device.videoZoomFactor - factor) > 1e-4f) device.videoZoomFactor = factor;
            // Torch.
            if (info.torch) {
                if (settings.torch && device.torchMode != AVCaptureTorchModeOn)
                    [device setTorchModeOnWithLevel:1.0f error:nil];
                else if (!settings.torch && device.torchMode != AVCaptureTorchModeOff)
                    device.torchMode = AVCaptureTorchModeOff;
            }
        } @catch (NSException*) {
            // A value the device refuses in its current state leaves it as it was.
        }
        if (running.connection) {
            const AVCaptureVideoStabilizationMode mode =
                settings.stabilization ? AVCaptureVideoStabilizationModeAuto : AVCaptureVideoStabilizationModeOff;
            if ([device.activeFormat isVideoStabilizationModeSupported:mode])
                running.connection.preferredVideoStabilizationMode = mode;
        }
#else
        (void)entry;
#endif
        [device unlockForConfiguration];
        running.settings = settings;
        running.focusSerial = settings.focusTriggerSerial;
        running.focusDistanceSerial = settings.focusDistanceSerial;
        running.applied = true;
    }

    bool takeStill(std::size_t camera, const CameraStillRequest& request) override
    {
        if (stopped_ || camera >= entries_.size() || (request.jpegWidth == 0 && !request.raw)) return false;
        // On the session's queue as a whole: a camera that only takes stills
        // leaves the session when its last still is delivered, and a capture
        // started here must be seen by that before the output could go.
        bool started = false;
        onQueue([&] {
            // The photo settings and the capture itself raise for a request the
            // output does not take; the camera then leaves with nothing in flight.
            @try {
                started = startStill(camera, request);
                // A capture that is refused leaves the camera as it found it.
                if (!started) releaseStill(camera);
            } @catch (NSException*) {
                started = false;
                {
                    std::lock_guard lock(photoMutex_);
                    photos_.erase(request.token);
                }
                releaseStill(camera);
            }
        });
        return started;
    }

    bool startStill(std::size_t camera, const CameraStillRequest& request)
    {
        const Entry& entry = entries_[camera];
        auto& running = cameras_[camera];
        if (!prepareStill(camera, entry, running)) return false;
        AVCapturePhotoOutput* output = running.photoOutput;
        AVCapturePhotoSettings* settings = nil;
        // The quality belongs inside the compression properties: iOS raises
        // (NSInvalidArgumentException) for it at the top level, which refused
        // every still there.
        NSDictionary* jpeg = @{
            AVVideoCodecKey: AVVideoCodecTypeJPEG,
            AVVideoCompressionPropertiesKey: @{AVVideoQualityKey: @(request.jpegQuality / 100.0)},
        };
        if (request.raw) {
#if TARGET_OS_IOS
            OSType rawFormat = 0;
            for (NSNumber* format in output.availableRawPhotoPixelFormatTypes)
                if ([AVCapturePhotoOutput isBayerRAWPixelFormat:format.unsignedIntValue]) {
                    rawFormat = format.unsignedIntValue;
                    break;
                }
            if (rawFormat == 0) return false;
            settings = request.jpegWidth != 0
                ? [AVCapturePhotoSettings photoSettingsWithRawPixelFormatType:rawFormat processedFormat:jpeg]
                : [AVCapturePhotoSettings photoSettingsWithRawPixelFormatType:rawFormat];
#else
            return false;  // macOS has no Bayer still
#endif
        } else {
            settings = [AVCapturePhotoSettings photoSettingsWithFormat:jpeg];
        }
        const AVCaptureFlashMode flash = request.flash == CameraFlashMode::on ? AVCaptureFlashModeOn
            : request.flash == CameraFlashMode::automatic ? AVCaptureFlashModeAuto : AVCaptureFlashModeOff;
        if (entry.info.flash && [output.supportedFlashModes containsObject:@(flash)]) settings.flashMode = flash;
#if TARGET_OS_IOS
        if (request.speedPrioritized || request.raw)
            settings.photoQualityPrioritization = AVCapturePhotoQualityPrioritizationSpeed;
#endif
        // The photo has the size the guest configured or the capture is refused:
        // the output makes up to the size set here, and a format that does not
        // list the request cannot make it, however large its other sizes are.
        const auto listed = listedPhotoSizes(entry.device.activeFormat);
        const CMVideoDimensions limit = output.maxPhotoDimensions;
        const uml::platform::CameraStillSize outputMaximum {std::uint32_t(std::max(0, int(limit.width))),
                                                            std::uint32_t(std::max(0, int(limit.height)))};
        const uml::platform::CameraStillSize wanted {request.jpegWidth, request.jpegHeight};
        const auto chosen = request.jpegWidth != 0 ? uml::platform::exactPhotoSize(listed, wanted, outputMaximum)
                                                   : uml::platform::coveringPhotoSize(listed, {}, outputMaximum);
        if (request.jpegWidth != 0 && !chosen) return false;
        if (chosen) settings.maxPhotoDimensions = CMVideoDimensions {std::int32_t(chosen->width), std::int32_t(chosen->height)};
        UMLPhotoDelegate* delegate = [[UMLPhotoDelegate alloc] initWithCamera:camera
                                                                         token:request.token
                                                                      wantJpeg:request.jpegWidth != 0
                                                                      jpegSize:wanted
                                                                       wantRaw:request.raw
                                                                         state:state_];
        // The photo output holds its delegate weakly, so the session keeps it
        // until the capture finishes, or none of its callbacks would arrive.
        {
            std::lock_guard lock(photoMutex_);
            photos_[request.token] = {camera, delegate};
        }
        [output capturePhotoWithSettings:settings delegate:delegate];
        return true;
    }

    void stop() override
    {
        if (stopped_.exchange(true)) return;
        {
            std::lock_guard lock(state_->mutex);
            state_->session = nullptr;
        }
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        for (id token : {interrupted_, resumed_, failed_, started_}) {
            if (token) [center removeObserver:token];
        }
        interrupted_ = resumed_ = failed_ = started_ = nil;
        AVCaptureSession* session = session_;
        onQueue([session] {
            if (session.running) [session stopRunning];
        });
        // Turn a torch off: a session that ends must not leave a light on.
        for (const auto& [camera, running] : cameras_) {
            (void)running;
#if TARGET_OS_IOS
            AVCaptureDevice* device = entries_[camera].device;
            if (device.hasTorch && device.torchMode != AVCaptureTorchModeOff && [device lockForConfiguration:nil]) {
                device.torchMode = AVCaptureTorchModeOff;
                [device unlockForConfiguration];
            }
#endif
        }
        {
            std::lock_guard lock(photoMutex_);
            photos_.clear();
        }
        for (auto& [camera, running] : cameras_) {
            (void)camera;
            endPressureWatch(running);
        }
        cameras_.clear();
        session_ = nil;
    }

    // Called by the output delegates, under the state's mutex.
    void deliver(CameraEvent event) { delegate_.cameraEvent(event); }

    void deliverFrame(std::size_t camera, CMSampleBufferRef sample)
    {
        if (camera >= entries_.size()) return;
        CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
        if (pixels == nullptr || CVPixelBufferGetPlaneCount(pixels) != 2) return;
        if (CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
        CFRetain(sample);
        CameraFrame frame;
        frame.width = std::uint32_t(CVPixelBufferGetWidth(pixels));
        frame.height = std::uint32_t(CVPixelBufferGetHeight(pixels));
        const auto* luma = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels, 0));
        const auto* chroma = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels, 1));
        frame.lumaStride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 0);
        frame.chromaStride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 1);
        frame.luma = {luma, frame.lumaStride * CVPixelBufferGetHeightOfPlane(pixels, 0)};
        frame.chroma = {chroma, frame.chromaStride * CVPixelBufferGetHeightOfPlane(pixels, 1)};
        frame.native = pixels;
        frame.owner = std::shared_ptr<const void>(sample, [pixels](const void* held) {
            CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
            CFRelease(held);
        });
        const CMTime presentation = CMSampleBufferGetPresentationTimeStamp(sample);
        frame.timestampNanoseconds = CMTIME_IS_VALID(presentation)
            ? std::uint64_t(CMTimeGetSeconds(presentation) * 1e9) : hostNow();
        frame.exposure = exposureOf(camera, sample);
        delegate_.cameraFrame(camera, std::move(frame));
    }

    // A capture is over: its delegate is no longer needed, and the result goes on.
    void deliverStill(std::size_t camera, CameraStillResult result)
    {
        {
            std::lock_guard lock(photoMutex_);
            photos_.erase(result.token);
        }
        delegate_.cameraStill(camera, std::move(result));
        // A camera that only takes stills leaves the session with its last
        // one. This is called under the state's mutex while the session lives,
        // and stop() waits for the queue after clearing that state, so the
        // block runs before the session can end.
        if (camera < entries_.size()) dispatch_async(queue_, ^{ releaseStill(camera); });
    }

    // The device's own state now: its 3A states, white balance, zoom, lens and
    // frame duration, and what the preview exposes with. What a frame or a photo
    // exposed with is read from that frame or photo (`exposureOf`, the photo
    // delegate).
    CameraExposure deviceExposureOf(std::size_t camera) const
    {
        const Entry& entry = entries_[camera];
        CameraExposure exposure;
        exposure.activeCamera = std::uint32_t(camera);
        AVCaptureDevice* device = entry.device;
#if TARGET_OS_IOS
        exposure.exposureNanoseconds = std::uint64_t(std::max(0.0, CMTimeGetSeconds(device.exposureDuration)) * 1e9);
        exposure.iso = std::uint32_t(device.ISO);
        const auto gains = device.deviceWhiteBalanceGains;
        exposure.whiteBalanceGains = {gains.redGain, gains.greenGain, gains.blueGain};
        @try {
            const float maximum = device.maxWhiteBalanceGain;
            AVCaptureWhiteBalanceGains clamped {std::clamp(gains.redGain, 1.0f, maximum), std::clamp(gains.greenGain, 1.0f, maximum),
                                                std::clamp(gains.blueGain, 1.0f, maximum)};
            const auto values = [device temperatureAndTintValuesForDeviceWhiteBalanceGains:clamped];
            exposure.colorTemperature = std::uint32_t(std::max(0.0f, values.temperature));
            exposure.tint = values.tint;
        } @catch (NSException*) {
        }
        exposure.lensPositionDioptres = dioptresOf(device.lensPosition, entry.info.minimumFocusDistanceMillimetres);
        exposure.ae = controlState(device.adjustingExposure, device.exposureMode == AVCaptureExposureModeLocked);
        exposure.af = entry.info.autofocus
            ? controlState(device.adjustingFocus, device.focusMode == AVCaptureFocusModeLocked) : CameraControlState::inactive;
        exposure.awb = controlState(device.adjustingWhiteBalance, device.whiteBalanceMode == AVCaptureWhiteBalanceModeLocked);
        exposure.zoomRatio = float(device.videoZoomFactor) * entry.zoomMultiplier;
        exposure.aperture = device.lensAperture;
        exposure.flashFired = false;
        const CMTime duration = device.activeVideoMinFrameDuration;
        exposure.frameDurationNanoseconds = CMTIME_IS_VALID(duration) && duration.value > 0
            ? std::uint64_t(CMTimeGetSeconds(duration) * 1e9) : 0;
#endif
        (void)device;
        return exposure;
    }

    CameraExposure exposureOf(std::size_t camera, CMSampleBufferRef sample) const
    {
        CameraExposure exposure = deviceExposureOf(camera);
        // The sample's own Exif attachment states what the exposure used.
        if (auto* exif = static_cast<CFDictionaryRef>(CMGetAttachment(sample, kCGImagePropertyExifDictionary, nullptr))) {
            double time = 0;
            if (auto* number = static_cast<CFNumberRef>(CFDictionaryGetValue(exif, kCGImagePropertyExifExposureTime));
                number && CFNumberGetValue(number, kCFNumberDoubleType, &time) && time > 0)
                exposure.exposureNanoseconds = std::uint64_t(time * 1e9);
            if (auto* isos = static_cast<CFArrayRef>(CFDictionaryGetValue(exif, kCGImagePropertyExifISOSpeedRatings));
                isos && CFArrayGetCount(isos) > 0) {
                int iso = 0;
                if (CFNumberGetValue(static_cast<CFNumberRef>(CFArrayGetValueAtIndex(isos, 0)), kCFNumberIntType, &iso) && iso > 0)
                    exposure.iso = std::uint32_t(iso);
            }
        }
        return exposure;
    }

    const Entry& entry(std::size_t camera) const { return entries_[camera]; }

private:
    static bool formatTakenByMultiCamera(AVCaptureDeviceFormat* format)
    {
#if TARGET_OS_IOS
        return format != nil && format.multiCamSupported;
#else
        (void)format;
        return true;
#endif
    }

    AVCaptureDeviceFormat* chooseFormat(const Entry& entry, std::uint32_t width, std::uint32_t height, std::uint32_t frameRate,
                                        bool multiCameraOnly) const
    {
        AVCaptureDeviceFormat* best = nil;
        std::uint64_t bestArea = 0;
        for (AVCaptureDeviceFormat* format in entry.device.formats) {
            const FourCharCode subtype = CMFormatDescriptionGetMediaSubType(format.formatDescription);
            if (subtype != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange &&
                subtype != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
                continue;
            const CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
            if (std::uint32_t(dimensions.width) < width || std::uint32_t(dimensions.height) < height) continue;
            if (multiCameraOnly && !formatTakenByMultiCamera(format)) continue;
            bool rate = false;
            for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges)
                rate |= range.minFrameRate <= frameRate + 1e-3 && range.maxFrameRate >= frameRate - 1e-3;
            if (!rate) continue;
            const std::uint64_t area = std::uint64_t(dimensions.width) * dimensions.height;
            // The smallest covering format; the full-range one among equals.
            if (best == nil || area < bestArea ||
                (area == bestArea && subtype == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)) {
                best = format;
                bestArea = area;
            }
        }
        return best;
    }

    using Move = std::pair<std::size_t, AVCaptureDeviceFormat*>;

    // The cameras with an input in the session: those that stream and those
    // that only take stills, a still in flight included.
    std::vector<std::size_t> camerasInSession() const
    {
        std::vector<std::size_t> result;
        for (const auto& [index, running] : cameras_)
            if (running.input != nil) result.push_back(index);
        return result;
    }
    std::vector<std::size_t> peersOf(std::size_t camera) const
    {
        auto result = camerasInSession();
        result.erase(std::remove(result.begin(), result.end(), camera), result.end());
        return result;
    }

    // A multi-camera session takes the formats of all its inputs, whatever
    // each input is for. For the cameras that are in it already: the formats
    // those that a multi-camera session does not take move to, or false when
    // one has no format it takes.
    bool planMoves(const std::vector<std::size_t>& peers, std::vector<Move>& moves) const
    {
        for (const auto index : peers) {
            const auto& other = cameras_.at(index);
            AVCaptureDeviceFormat* current = other.format != nil ? other.format : entries_[index].device.activeFormat;
            if (formatTakenByMultiCamera(current)) continue;
            AVCaptureDeviceFormat* moved = other.video
                ? chooseFormat(entries_[index], other.width, other.height, other.frameRate, true)
                : chooseStillFormat(entries_[index], true);
            if (moved == nil) return false;
            moves.emplace_back(index, moved);
        }
        return true;
    }

    // Moves the cameras to their formats; the cameras it moved are put back
    // their settings, which a format change reverts in part.
    bool applyMoves(const std::vector<Move>& moves, std::vector<std::size_t>& moved)
    {
        for (const auto& [index, format] : moves) {
            auto& other = cameras_[index];
            bool ok;
            if (other.video) {
                other.throttle->requestedRate = other.frameRate;
                ok = setFormat(entries_[index].device, format, throttledRate(*other.throttle, format));
            } else {
                ok = setActiveFormat(entries_[index].device, format);
            }
            if (!ok) return false;
            other.format = format;
            moved.push_back(index);
        }
        return true;
    }

    void reapply(const std::vector<std::size_t>& cameras)
    {
        for (const auto index : cameras)
            if (cameras_[index].applied) applyOnQueue(index, cameras_[index].settings, true);
    }

    bool startVideoOnQueue(std::size_t camera, std::uint32_t width, std::uint32_t height, std::uint32_t frameRate)
    {
        const Entry& entry = entries_[camera];
        auto& running = cameras_[camera];
        if (running.video && running.width == width && running.height == height && running.frameRate == frameRate)
            return true;
        // Cameras that run together in a multi-camera session need formats it
        // takes. A camera alone may use any format (applications linked on or
        // after iOS 26 are not held to the rule for a single input), so the
        // cameras already in the session move to such a format when another
        // joins, or the new one is refused and nothing changes. Every camera
        // with an input counts, one that only takes stills as well.
        const auto peers = peersOf(camera);
        const bool several = multiCamera_ && !peers.empty();
        AVCaptureDeviceFormat* format = chooseFormat(entry, width, height, frameRate, several);
        if (format == nil) return false;
        std::vector<Move> moves;
        if (several && !planMoves(peers, moves)) return false;
        std::vector<std::size_t> moved;
        bool ok = applyMoves(moves, moved);
        if (ok) ok = configureVideo(camera, entry, running, format, width, height, frameRate);
        if (ok) startSession();
        reapply(moved);
        return ok;
    }

    void startSession()
    {
        @try {
            if (!session_.running) [session_ startRunning];
        } @catch (NSException*) {
            // The session's runtime error says how it went.
        }
    }

    // Whether the multi-camera session's hardware is over its budget (it would
    // not start): a join that does that is undone.
    bool overBudget() const
    {
#if TARGET_OS_IOS
        return multiCamera_ && ((AVCaptureMultiCamSession*)session_).hardwareCost > 1.0f;
#else
        return false;
#endif
    }

    // Adds a camera's input to the session; nothing is left behind on failure.
    bool addInput(const Entry& entry, RunningCamera& running)
    {
        NSError* error = nil;
        AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:entry.device error:&error];
        if (input == nil || ![session_ canAddInput:input]) return false;
#if TARGET_OS_IOS
        if (multiCamera_) {
            [(AVCaptureMultiCamSession*)session_ addInputWithNoConnections:input];
            running.input = input;
            return true;
        }
#endif
        [session_ addInput:input];
        running.input = input;
        return true;
    }

    // Takes a camera's outputs, and its input once it has none left, out of
    // the session.
    void removeFromSession(RunningCamera& running, bool video, bool photo)
    {
        [session_ beginConfiguration];
        @try {
            if (video && running.videoOutput) [session_ removeOutput:running.videoOutput];
            if (photo && running.photoOutput) [session_ removeOutput:running.photoOutput];
        } @catch (NSException*) {
        }
        if (video) {
            running.videoOutput = nil;
            running.delegate = nil;
            running.connection = nil;
            running.video = false;
        }
        if (photo) running.photoOutput = nil;
        if (running.input != nil && running.videoOutput == nil && running.photoOutput == nil) {
            @try {
                [session_ removeInput:running.input];
            } @catch (NSException*) {
            }
            running.input = nil;
            running.format = nil;
        }
        [session_ commitConfiguration];
    }

    // A camera left alone in a multi-camera session may use the format it
    // would have had alone.
    void returnAlone()
    {
        if (!multiCamera_) return;
        const auto inSession = camerasInSession();
        if (inSession.size() != 1) return;
        const auto index = inSession.front();
        auto& last = cameras_[index];
        if (!last.video) return;
        AVCaptureDeviceFormat* best = chooseFormat(entries_[index], last.width, last.height, last.frameRate, false);
        if (best == nil || best == last.format) return;
        if (!setFormat(entries_[index].device, best, throttledRate(*last.throttle, best))) return;
        last.format = best;
        reapply({index});
    }

    // Called on the session's queue after a still is delivered: a camera that
    // streams keeps its photo output, one that only takes stills leaves.
    void releaseStill(std::size_t camera)
    {
        if (stopped_ || camera >= entries_.size()) return;
        auto& running = cameras_[camera];
        if (running.video || running.photoOutput == nil || stillsInFlight(camera)) return;
        removeFromSession(running, false, true);
        returnAlone();
    }

    // The format with the largest still a camera offers (among those a
    // multi-camera session takes, when asked): what a camera that only takes
    // stills runs in a session with other cameras.
    AVCaptureDeviceFormat* chooseStillFormat(const Entry& entry, bool multiCameraOnly) const
    {
        AVCaptureDeviceFormat* best = nil;
        std::int64_t bestArea = -1;
        for (AVCaptureDeviceFormat* format in entry.device.formats) {
            if (multiCameraOnly && !formatTakenByMultiCamera(format)) continue;
            std::int64_t area = 0;
            for (NSValue* value in format.supportedMaxPhotoDimensions) {
                CMVideoDimensions photo;
                [value getValue:&photo];
                area = std::max<std::int64_t>(area, std::int64_t(photo.width) * photo.height);
            }
            if (area > bestArea) {
                best = format;
                bestArea = area;
            }
        }
        return best;
    }

    bool configureVideo(std::size_t camera, const Entry& entry, RunningCamera& running, AVCaptureDeviceFormat* format,
                        std::uint32_t width, std::uint32_t height, std::uint32_t frameRate)
    {
        const bool newInput = running.input == nil;
        AVCaptureDeviceFormat* const previousFormat = running.format;
        AVCaptureVideoDataOutput* newOutput = nil;
        UMLCameraOutputDelegate* newDelegate = nil;
        AVCaptureConnection* newConnection = nil;
        bool ok = true;
        [session_ beginConfiguration];
        // Every change here can raise; the configuration is committed either way.
        @try {
            if (running.input == nil) ok = addInput(entry, running);
            if (ok) {
                running.throttle->requestedRate = frameRate;
                ok = setFormat(entry.device, format, throttledRate(*running.throttle, format));
            }
            if (ok && running.videoOutput == nil) {
                AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
                output.videoSettings = @{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey:
                                             @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)};
                output.alwaysDiscardsLateVideoFrames = YES;
                UMLCameraOutputDelegate* delegate = [[UMLCameraOutputDelegate alloc] initWithCamera:camera state:state_];
                [output setSampleBufferDelegate:delegate queue:outputQueue_];
                if (multiCamera_) {
#if TARGET_OS_IOS
                    AVCaptureInputPort* port = [running.input portsWithMediaType:AVMediaTypeVideo
                                                                 sourceDeviceType:entry.device.deviceType
                                                             sourceDevicePosition:entry.device.position].firstObject;
                    if (port != nil && [session_ canAddOutput:output]) {
                        [(AVCaptureMultiCamSession*)session_ addOutputWithNoConnections:output];
                        newOutput = output;
                        AVCaptureConnection* connection = [[AVCaptureConnection alloc] initWithInputPorts:@[port] output:output];
                        if ([session_ canAddConnection:connection]) {
                            [session_ addConnection:connection];
                            newConnection = connection;
                        } else {
                            ok = false;
                        }
                    } else {
                        ok = false;
                    }
#endif
                } else if ([session_ canAddOutput:output]) {
                    [session_ addOutput:output];
                    newOutput = output;
                    newConnection = [output connectionWithMediaType:AVMediaTypeVideo];
                } else {
                    ok = false;
                }
                newDelegate = delegate;
            }
        } @catch (NSException*) {
            ok = false;
        }
        [session_ commitConfiguration];
        if (ok && overBudget()) ok = false;
        if (ok) {
            if (newOutput != nil) {
                running.videoOutput = newOutput;
                running.delegate = newDelegate;
                running.connection = newConnection;
            }
            running.video = true;
            running.format = format;
            running.width = width;
            running.height = height;
            running.frameRate = frameRate;
            beginPressureWatch(entry, running);
            return true;
        }
        // The join is undone: what it added leaves again, and a stream that
        // was resized has its old format back.
        [session_ beginConfiguration];
        @try {
            if (newOutput != nil) [session_ removeOutput:newOutput];
        } @catch (NSException*) {
        }
        [session_ commitConfiguration];
        if (newInput && running.input != nil && running.videoOutput == nil && running.photoOutput == nil)
            removeFromSession(running, false, false);
        if (!newInput && newOutput == nil && running.video && previousFormat != nil) {
            running.throttle->requestedRate = running.frameRate;
            (void)setFormat(entry.device, previousFormat, throttledRate(*running.throttle, previousFormat));
        }
        return false;
    }

    // The system's pressure on a camera lowers the rate it runs at, as Apple
    // advises (heat that goes on rising ends in the session's interruption);
    // the capture's client sees fewer frames, and the interruption itself
    // arrives as an event of its own.
    void beginPressureWatch(const Entry& entry, RunningCamera& running)
    {
#if TARGET_OS_IOS
        if (running.pressure != nil) return;
        AVCaptureDevice* device = entry.device;
        auto throttle = running.throttle;
        dispatch_queue_t queue = queue_;
        running.pressure = [[UMLPressureObserver alloc] initWithDevice:device
                                                               handler:^(AVCaptureSystemPressureLevel level) {
            std::uint32_t cap = 0;
            if ([level isEqualToString:AVCaptureSystemPressureLevelSerious]) cap = 20;
            else if ([level isEqualToString:AVCaptureSystemPressureLevelCritical] ||
                     [level isEqualToString:AVCaptureSystemPressureLevelShutdown])
                cap = 15;
            if (throttle->cap.exchange(cap) == cap) return;
            // The frame durations alone: setting the format again would revert
            // the device's focus and exposure points.
            dispatch_async(queue, ^{
                (void)setFrameRate(device, throttledRate(*throttle, device.activeFormat));
            });
        }];
#else
        (void)entry;
        (void)running;
#endif
    }

    static void endPressureWatch(RunningCamera& running)
    {
#if TARGET_OS_IOS
        [running.pressure invalidate];
        running.pressure = nil;
#else
        (void)running;
#endif
        running.throttle->cap = 0;
    }

    bool prepareStill(std::size_t camera, const Entry& entry, RunningCamera& running)
    {
        if (running.photoOutput != nil) return true;
        // A camera that takes only a still joins the session as a video camera
        // does: the cameras in it move to formats a multi-camera session takes,
        // as does this one, or the still is refused.
        const auto peers = peersOf(camera);
        const bool several = multiCamera_ && !peers.empty();
        AVCaptureDeviceFormat* own = nil;
        if (several && running.input == nil && !formatTakenByMultiCamera(entry.device.activeFormat)) {
            own = chooseStillFormat(entry, true);
            if (own == nil) return false;
        }
        std::vector<Move> moves;
        if (several && !planMoves(peers, moves)) return false;
        std::vector<std::size_t> moved;
        bool ok = applyMoves(moves, moved);
        if (ok && own != nil) ok = setActiveFormat(entry.device, own);
        const bool newInput = running.input == nil;
        AVCapturePhotoOutput* newOutput = nil;
        if (ok) {
            [session_ beginConfiguration];
            // Every change here can raise; the configuration is committed either way.
            @try {
                if (running.input == nil) ok = addInput(entry, running);
                if (ok) {
                    AVCapturePhotoOutput* output = [[AVCapturePhotoOutput alloc] init];
#if TARGET_OS_IOS
                    output.maxPhotoQualityPrioritization = AVCapturePhotoQualityPrioritizationQuality;
#endif
                    const auto attach = [&]() -> bool {
                        if (multiCamera_) {
#if TARGET_OS_IOS
                            AVCaptureInputPort* port = [running.input portsWithMediaType:AVMediaTypeVideo
                                                                         sourceDeviceType:entry.device.deviceType
                                                                     sourceDevicePosition:entry.device.position].firstObject;
                            if (port == nil || ![session_ canAddOutput:output]) return false;
                            [(AVCaptureMultiCamSession*)session_ addOutputWithNoConnections:output];
                            newOutput = output;
                            AVCaptureConnection* connection = [[AVCaptureConnection alloc] initWithInputPorts:@[port] output:output];
                            if (![session_ canAddConnection:connection]) return false;
                            [session_ addConnection:connection];
                            return true;
#else
                            return false;
#endif
                        }
                        if (![session_ canAddOutput:output]) return false;
                        [session_ addOutput:output];
                        newOutput = output;
                        return true;
                    };
                    // The output makes photos up to the largest size the format
                    // lists (its default is the smallest, which the Mac gets
                    // too). AVFoundation requires that size to be one of the
                    // active format's supportedMaxPhotoDimensions of the
                    // output's source device, and an unattached output has no
                    // source: the maximum is set after the output is attached.
                    const auto setMaximum = [&](uml::platform::CameraStillSize size) -> bool {
                        @try {
                            output.maxPhotoDimensions = CMVideoDimensions {std::int32_t(size.width), std::int32_t(size.height)};
                            return true;
                        } @catch (NSException* exception) {
                            std::fprintf(stderr, "Camera: the photo output refused photos up to %ux%u: %s\n", size.width, size.height,
                                         exception.reason.UTF8String);
                            return false;
                        }
                    };
                    const auto detach = [&]() {
                        @try {
                            [session_ removeOutput:output];
                        } @catch (NSException*) {
                        }
                        newOutput = nil;
                    };
                    ok = uml::platform::preparePhotoOutput(uml::platform::largestPhotoSize(listedPhotoSizes(entry.device.activeFormat)),
                                                           {attach, setMaximum, detach}) ==
                        uml::platform::PhotoOutputPreparation::ready;
                }
            } @catch (NSException*) {
                ok = false;
            }
            [session_ commitConfiguration];
            if (ok && overBudget()) ok = false;
        }
        if (ok) {
            running.photoOutput = newOutput;
            if (running.format == nil) running.format = entry.device.activeFormat;
            startSession();
        } else {
            // The join is undone: what it added leaves again.
            if (newOutput != nil) {
                [session_ beginConfiguration];
                @try {
                    [session_ removeOutput:newOutput];
                } @catch (NSException*) {
                }
                [session_ commitConfiguration];
            }
            if (newInput && running.input != nil && running.videoOutput == nil && running.photoOutput == nil)
                removeFromSession(running, false, false);
        }
        reapply(moved);
        return ok;
    }

    // The captures on their way: their delegates are held here.
    struct Photo {
        std::size_t camera {};
        UMLPhotoDelegate* delegate {};
    };
    bool stillsInFlight(std::size_t camera)
    {
        std::lock_guard lock(photoMutex_);
        return std::any_of(photos_.begin(), photos_.end(), [camera](const auto& entry) { return entry.second.camera == camera; });
    }

    std::vector<Entry> entries_;
    CameraSessionDelegate& delegate_;
    std::shared_ptr<SessionState> state_;
    std::mutex photoMutex_;
    std::map<std::uint64_t, Photo> photos_;
    AVCaptureSession* session_ {};
    dispatch_queue_t queue_;
    dispatch_queue_t outputQueue_;
    id interrupted_ {};
    id resumed_ {};
    id failed_ {};
    id started_ {};
    std::shared_ptr<std::atomic_bool> restarting_ {std::make_shared<std::atomic_bool>(false)};
    bool multiCamera_ {};
    std::atomic_bool stopped_ {};
    std::map<std::size_t, RunningCamera> cameras_;
};

class AppleCamera final : public Camera {
public:
    AppleCamera() : entries_(discover())
    {
#if TARGET_OS_IOS
        // The sets AVFoundation lists, by position in cameras().
        NSMutableArray<AVCaptureDeviceType>* types = [NSMutableArray array];
        for (const auto& entry : entries_) [types addObject:entry.device.deviceType];
        AVCaptureDeviceDiscoverySession* session =
            [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                                                                   mediaType:AVMediaTypeVideo
                                                                    position:AVCaptureDevicePositionUnspecified];
        for (NSSet<AVCaptureDevice*>* set in session.supportedMultiCamDeviceSets) {
            std::vector<std::size_t> members;
            for (AVCaptureDevice* device in set)
                for (std::size_t index = 0; index < entries_.size(); ++index)
                    if ([entries_[index].device.uniqueID isEqualToString:device.uniqueID]) members.push_back(index);
            std::sort(members.begin(), members.end());
            if (members.size() > 1) sets_.push_back(std::move(members));
        }
#endif
    }

    std::vector<CameraInfo> cameras() const override
    {
        std::vector<CameraInfo> infos;
        for (const auto& entry : entries_) infos.push_back(entry.info);
        return infos;
    }

    std::vector<std::vector<std::size_t>> concurrentSets() const override { return sets_; }

    std::string hostModel() const override
    {
        char model[128] = {};
        std::size_t size = sizeof(model);
#if TARGET_OS_IOS
        const char* name = "hw.machine";
#else
        const char* name = "hw.model";
#endif
        if (sysctlbyname(name, model, &size, nullptr, 0) != 0) return "unknown";
        return model;
    }

    std::uint64_t now() const noexcept override { return hostNow(); }

    std::unique_ptr<Nv12Transformer> makeFrameTransformer() const override { return makeAcceleratedNv12Transformer(); }

    std::unique_ptr<CameraSession> openSession(CameraSessionDelegate& delegate) override
    {
        if (entries_.empty()) return nullptr;
        // The system's answer is read, never asked for: a session is the one
        // act that starts the capture indicator, and only a granted app may.
        if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo] != AVAuthorizationStatusAuthorized)
            return nullptr;
        return std::make_unique<AppleSession>(entries_, delegate);
    }

private:
    std::vector<Entry> entries_;
    std::vector<std::vector<std::size_t>> sets_;
};

} // namespace

std::unique_ptr<Camera> makeAppleCamera()
{
    return std::make_unique<AppleCamera>();
}

} // namespace uml::platform

// ---------------------------------------------------------------------------

@implementation UMLCameraOutputDelegate {
    size_t _camera;
    std::shared_ptr<SessionState> _state;
}

- (instancetype)initWithCamera:(size_t)camera state:(std::shared_ptr<SessionState>)state
{
    if ((self = [super init])) {
        _camera = camera;
        _state = std::move(state);
    }
    return self;
}

- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection
{
    (void)output;
    (void)connection;
    std::lock_guard lock(_state->mutex);
    if (_state->session) static_cast<uml::platform::AppleSession*>(_state->session)->deliverFrame(_camera, sampleBuffer);
}

@end

@implementation UMLPhotoDelegate {
    size_t _camera;
    uint64_t _token;
    BOOL _wantJpeg;
    // The size of the JPEG the guest configured.
    uml::platform::CameraStillSize _jpegSize;
    BOOL _wantRaw;
    std::shared_ptr<SessionState> _state;
    uml::platform::CameraStillResult _result;
    // What the photos' metadata and the Bayer still's DNG state of the
    // exposure. The processed photo's readings are the exposure's when there
    // is one; the Bayer still's otherwise.
    uml::platform::PhotoReadings _jpegReadings;
    uml::platform::PhotoReadings _rawReadings;
    uml::platform::DngExposureData _dng;
}

- (instancetype)initWithCamera:(size_t)camera
                         token:(uint64_t)token
                      wantJpeg:(BOOL)wantJpeg
                      jpegSize:(uml::platform::CameraStillSize)jpegSize
                       wantRaw:(BOOL)wantRaw
                         state:(std::shared_ptr<SessionState>)state
{
    if ((self = [super init])) {
        _camera = camera;
        _token = token;
        _wantJpeg = wantJpeg;
        _jpegSize = jpegSize;
        _wantRaw = wantRaw;
        _state = std::move(state);
        _result.token = token;
    }
    return self;
}

- (void)captureOutput:(AVCapturePhotoOutput*)output
    didFinishProcessingPhoto:(AVCapturePhoto*)photo
                       error:(NSError*)error
{
    (void)output;
    // A part that fails leaves its field empty and the other part alone: a Bayer
    // still that cannot be made does not take the person's JPEG with it.
    if (error != nil || photo == nil) return;
    NSData* data = photo.fileDataRepresentation;
    // The photo's own Exif, read from its file: AVCapturePhoto.metadata is not
    // on the Mac.
    const auto readings = data == nil ? uml::platform::PhotoReadings {}
        : uml::platform::photoReadingsOfImage(
              std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data.bytes), data.length));
#if TARGET_OS_IOS
    if (photo.isRawPhoto) {
        _rawReadings = readings;
        CVPixelBufferRef pixels = photo.pixelBuffer;
        if (pixels == nullptr || data == nil) return;
        const auto order = uml::platform::bayerOrderOf(CVPixelBufferGetPixelFormatType(pixels));
        const auto width = CVPixelBufferGetWidth(pixels);
        const auto height = CVPixelBufferGetHeight(pixels);
        if (order == uml::platform::BayerOrder::none || CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
            return;
        const auto* base = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(pixels));
        const auto stride = CVPixelBufferGetBytesPerRow(pixels);
        _result.raw.resize(width * height);
        for (std::size_t row = 0; row < height; ++row)
            std::memcpy(_result.raw.data() + row * width, base + row * stride, width * sizeof(std::uint16_t));
        CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
        // The DNG that comes with the Bayer pixels states what the sensor is.
        const auto bytes = std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data.bytes), data.length);
        _dng = uml::platform::parseDngExposureData(bytes);
        if (auto color = uml::platform::parseDngColorData(bytes)) {
            _result.color = *color;
            if (_result.color.bayer == uml::platform::BayerOrder::none) _result.color.bayer = order;
            if (_result.color.rawWidth == 0) {
                _result.color.rawWidth = std::uint32_t(width);
                _result.color.rawHeight = std::uint32_t(height);
            }
        } else {
            _result.color.bayer = order;
            _result.color.rawWidth = std::uint32_t(width);
            _result.color.rawHeight = std::uint32_t(height);
        }
    } else
#endif
    if (data != nil) {
        _jpegReadings = readings;
        const auto* bytes = static_cast<const std::uint8_t*>(data.bytes);
        _result.jpeg.assign(bytes, bytes + data.length);
    }
}

- (void)captureOutput:(AVCapturePhotoOutput*)output
    didFinishCaptureForResolvedSettings:(AVCaptureResolvedPhotoSettings*)resolvedSettings
                                  error:(NSError*)error
{
    (void)output;
    (void)error;
    // The session drops its hold on this object when it delivers, which can be
    // the last one.
    __strong UMLPhotoDelegate* keepAlive = self;
    (void)keepAlive;
    // The photo is the size the guest configured or it is not passed on: the
    // output makes at most the size it was asked for, and the platform says
    // what it resolved. (The device compares the encoded picture as well.)
    if (_wantJpeg && resolvedSettings != nil && !_result.jpeg.empty()) {
        const CMVideoDimensions resolved = resolvedSettings.photoDimensions;
        const uml::platform::CameraStillSize resolvedSize {std::uint32_t(std::max(0, int(resolved.width))),
                                                           std::uint32_t(std::max(0, int(resolved.height)))};
        if (!uml::platform::resolvedSizeIsRequested(resolvedSize, _jpegSize)) {
            std::fprintf(stderr, "Camera: the photo resolved to %ux%u, not the %ux%u that was asked for\n", resolvedSize.width,
                         resolvedSize.height, _jpegSize.width, _jpegSize.height);
            _result.jpeg.clear();
        }
    }
    // Whatever arrived stands; the capture failed only when nothing asked for did.
    _result.ok = (_wantJpeg && !_result.jpeg.empty()) || (_wantRaw && !_result.raw.empty());
    // Whether the flash fired is the metadata's word, and the platform's decision
    // for the capture when the metadata has none.
    std::optional<bool> flashResolved;
#if TARGET_OS_IOS
    if (resolvedSettings != nil) flashResolved = bool(resolvedSettings.isFlashEnabled);
#else
    (void)resolvedSettings;
#endif
    std::lock_guard lock(_state->mutex);
    if (!_state->session) return;
    auto* session = static_cast<uml::platform::AppleSession*>(_state->session);
    const bool haveJpegReadings = _jpegReadings.exposureSeconds || _jpegReadings.iso || _jpegReadings.fNumber ||
        _jpegReadings.focalLengthMillimetres || _jpegReadings.flash;
    // The photo's own readings come from its Exif. The 3A states, white balance,
    // zoom and lens are read from the session now, at delivery: they are what it
    // reports then, not what they were at the exposure, and on macOS the session
    // reports none of them, so they keep their defaults.
    _result.exposure = uml::platform::stillExposureOf(
        session->deviceExposureOf(_camera), haveJpegReadings ? _jpegReadings : _rawReadings, flashResolved, _dng);
    session->deliverStill(_camera, std::move(_result));
}

@end

#if TARGET_OS_IOS
@implementation UMLPressureObserver {
    AVCaptureDevice* _device;
    void (^_handler)(AVCaptureSystemPressureLevel);
    BOOL _observing;
}

- (instancetype)initWithDevice:(AVCaptureDevice*)device handler:(void (^)(AVCaptureSystemPressureLevel))handler
{
    if ((self = [super init])) {
        _device = device;
        _handler = [handler copy];
        [device addObserver:self forKeyPath:@"systemPressureState" options:NSKeyValueObservingOptionNew context:nil];
        _observing = YES;
    }
    return self;
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary*)change
                       context:(void*)context
{
    (void)change;
    (void)context;
    if (![keyPath isEqualToString:@"systemPressureState"]) return;
    AVCaptureSystemPressureState* state = static_cast<AVCaptureDevice*>(object).systemPressureState;
    if (state != nil && _handler != nil) _handler(state.level);
}

- (void)invalidate
{
    if (!_observing) return;
    _observing = NO;
    [_device removeObserver:self forKeyPath:@"systemPressureState"];
    _handler = nil;
    _device = nil;
}

- (void)dealloc
{
    [self invalidate];
}

@end
#endif
