// SPDX-License-Identifier: GPL-2.0-or-later
//
// The platform Camera over AVFoundation. Discovery reads devices and formats
// and needs no authorization. A session is one AVCaptureMultiCamSession on iOS
// (an AVCaptureSession on macOS, which has no multi-camera session) with one
// input, one video data output and, once a still is asked for, one photo
// output per running camera.

#include "camera.hpp"
#include "dng_color.hpp"

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
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

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
        if ([type isEqualToString:AVCaptureDeviceTypeBuiltInUltraWideCamera]) info.lens = CameraLensKind::ultraWide;
        else if ([type isEqualToString:AVCaptureDeviceTypeBuiltInTelephotoCamera]) info.lens = CameraLensKind::telephoto;
        else if (device.virtualDevice) info.lens = CameraLensKind::zoom;
        if (info.position == CameraPosition::external) info.lens = CameraLensKind::external;
#endif
        info.autofocus = [device isFocusModeSupported:AVCaptureFocusModeContinuousAutoFocus];
        info.flash = device.hasFlash;
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
                       wantRaw:(BOOL)wantRaw
                         state:(std::shared_ptr<SessionState>)state;
@end

namespace uml::platform {
namespace {

struct RunningCamera {
    AVCaptureDeviceInput* input {};
    AVCaptureVideoDataOutput* videoOutput {};
    AVCapturePhotoOutput* photoOutput {};
    UMLCameraOutputDelegate* delegate {};
    AVCaptureConnection* connection {};
    bool video {};
    CameraSettings settings;
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t frameRate {};
    std::uint32_t focusSerial {};
};

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
        failed_ = [center addObserverForName:AVCaptureSessionRuntimeErrorNotification
                                      object:session_
                                       queue:nil
                                  usingBlock:^(NSNotification*) {
                                      // A media-services reset is the recoverable case; try once.
                                      post({allCameras, CameraEventKind::interrupted});
                                      AVCaptureSession* session = session_;
                                      dispatch_async(queue_, ^{
                                          if (!session.running) [session startRunning];
                                      });
                                  }];
    }

    ~AppleSession() override { stop(); }

    // Runs `work` on the session's queue and waits for it. A lambda keeps the
    // references it captures as references, which a block would copy.
    void onQueue(const std::function<void()>& work) { dispatch_sync(queue_, ^{ work(); }); }

    bool startVideo(std::size_t camera, std::uint32_t width, std::uint32_t height, std::uint32_t frameRate) override
    {
        if (stopped_ || camera >= entries_.size() || width == 0 || height == 0 || frameRate == 0) return false;
        const Entry& entry = entries_[camera];
        auto& running = cameras_[camera];
        if (running.video && running.width == width && running.height == height && running.frameRate == frameRate)
            return true;
        AVCaptureDeviceFormat* format = chooseFormat(entry, width, height, frameRate);
        if (format == nil) return false;
        bool ok = true;
        onQueue([&] {
            ok = configureVideo(camera, entry, running, format, width, height, frameRate);
            if (ok && !session_.running) [session_ startRunning];
        });
        return ok;
    }

    void stopVideo(std::size_t camera) override
    {
        const auto found = cameras_.find(camera);
        if (stopped_ || found == cameras_.end() || !found->second.video) return;
        auto& running = found->second;
        onQueue([&] {
            [session_ beginConfiguration];
            if (running.videoOutput) [session_ removeOutput:running.videoOutput];
            running.videoOutput = nil;
            running.delegate = nil;
            running.connection = nil;
            if (running.photoOutput == nil && running.input) {
                [session_ removeInput:running.input];
                running.input = nil;
            }
            [session_ commitConfiguration];
            running.video = false;
        });
    }

    void apply(std::size_t camera, const CameraSettings& settings) override
    {
        if (stopped_ || camera >= entries_.size()) return;
        auto& running = cameras_[camera];
        if (running.settings == settings && running.focusSerial == settings.focusTriggerSerial) return;
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
            // Focus.
            if (info.autofocus) {
                if (settings.focusMode == CameraFocusMode::manual && info.manualFocus) {
                    [device setFocusModeLockedWithLensPosition:lensPositionOf(settings.focusDistanceDioptres,
                                                                              info.minimumFocusDistanceMillimetres)
                                             completionHandler:nil];
                } else if (settings.focusMode == CameraFocusMode::locked) {
                    if ([device isFocusModeSupported:AVCaptureFocusModeLocked]) device.focusMode = AVCaptureFocusModeLocked;
                } else if (settings.focusTriggerSerial != running.focusSerial &&
                           [device isFocusModeSupported:AVCaptureFocusModeAutoFocus]) {
                    device.focusMode = AVCaptureFocusModeAutoFocus;
                } else if ([device isFocusModeSupported:AVCaptureFocusModeContinuousAutoFocus]) {
                    device.focusMode = AVCaptureFocusModeContinuousAutoFocus;
                }
                if (settings.afRegion && settings.afRegion->weight > 0 && device.focusPointOfInterestSupported)
                    device.focusPointOfInterest = CGPointMake(settings.afRegion->x + settings.afRegion->width / 2,
                                                              settings.afRegion->y + settings.afRegion->height / 2);
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
    }

    bool takeStill(std::size_t camera, const CameraStillRequest& request) override
    {
        if (stopped_ || camera >= entries_.size() || (request.jpegWidth == 0 && !request.raw)) return false;
        const Entry& entry = entries_[camera];
        auto& running = cameras_[camera];
        bool ok = true;
        onQueue([&] { ok = prepareStill(camera, entry, running); });
        if (!ok) return false;
        AVCapturePhotoOutput* output = running.photoOutput;
        AVCapturePhotoSettings* settings = nil;
        NSDictionary* jpeg = @{AVVideoCodecKey: AVVideoCodecTypeJPEG, AVVideoQualityKey: @(request.jpegQuality / 100.0)};
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
        // The closest size the format lists that covers the request.
        CMVideoDimensions chosen {0, 0};
        for (NSValue* value in entry.device.activeFormat.supportedMaxPhotoDimensions) {
            CMVideoDimensions candidate;
            [value getValue:&candidate];
            const bool covers = std::uint32_t(candidate.width) >= request.jpegWidth &&
                std::uint32_t(candidate.height) >= request.jpegHeight;
            const bool closer = chosen.width == 0 ||
                std::int64_t(candidate.width) * candidate.height < std::int64_t(chosen.width) * chosen.height;
            if (covers && closer) chosen = candidate;
        }
        if (chosen.width != 0 && chosen.width <= output.maxPhotoDimensions.width) settings.maxPhotoDimensions = chosen;
#endif
        UMLPhotoDelegate* delegate = [[UMLPhotoDelegate alloc] initWithCamera:camera
                                                                         token:request.token
                                                                       wantRaw:request.raw
                                                                         state:state_];
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
        for (id token : {interrupted_, resumed_, failed_}) {
            if (token) [center removeObserver:token];
        }
        interrupted_ = resumed_ = failed_ = nil;
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

    void deliverStill(std::size_t camera, CameraStillResult result) { delegate_.cameraStill(camera, std::move(result)); }

    CameraExposure exposureOf(std::size_t camera, CMSampleBufferRef sample) const
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
        (void)device;
        return exposure;
    }

    const Entry& entry(std::size_t camera) const { return entries_[camera]; }

private:
    AVCaptureDeviceFormat* chooseFormat(const Entry& entry, std::uint32_t width, std::uint32_t height,
                                        std::uint32_t frameRate) const
    {
        AVCaptureDeviceFormat* best = nil;
        std::uint64_t bestArea = 0;
        // A second camera must use formats a multi-camera session accepts; so
        // must the first, which is restarted on one when the second joins.
        const bool needMultiCamera = multiCamera_ &&
            (cameras_.size() > 1 || (!cameras_.empty() && !cameras_.contains(indexOf(entry))));
        for (AVCaptureDeviceFormat* format in entry.device.formats) {
            const FourCharCode subtype = CMFormatDescriptionGetMediaSubType(format.formatDescription);
            if (subtype != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange &&
                subtype != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
                continue;
            const CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
            if (std::uint32_t(dimensions.width) < width || std::uint32_t(dimensions.height) < height) continue;
#if TARGET_OS_IOS
            if (needMultiCamera && !format.multiCamSupported) continue;
#else
            (void)needMultiCamera;
#endif
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

    std::size_t indexOf(const Entry& entry) const
    {
        return std::size_t(&entry - entries_.data());
    }

    bool configureVideo(std::size_t camera, const Entry& entry, RunningCamera& running, AVCaptureDeviceFormat* format,
                        std::uint32_t width, std::uint32_t height, std::uint32_t frameRate)
    {
        [session_ beginConfiguration];
        bool ok = true;
        NSError* error = nil;
        if (running.input == nil) {
            running.input = [AVCaptureDeviceInput deviceInputWithDevice:entry.device error:&error];
            if (running.input == nil) ok = false;
            else if (multiCamera_) {
#if TARGET_OS_IOS
                if ([session_ canAddInput:running.input]) [(AVCaptureMultiCamSession*)session_ addInputWithNoConnections:running.input];
                else ok = false;
#endif
            } else if ([session_ canAddInput:running.input]) {
                [session_ addInput:running.input];
            } else {
                ok = false;
            }
        }
        if (ok && [entry.device lockForConfiguration:&error]) {
            entry.device.activeFormat = format;
            const CMTime duration = CMTimeMake(1, std::int32_t(frameRate));
            entry.device.activeVideoMinFrameDuration = duration;
            entry.device.activeVideoMaxFrameDuration = duration;
            [entry.device unlockForConfiguration];
        } else {
            ok = false;
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
                    AVCaptureConnection* connection = [[AVCaptureConnection alloc] initWithInputPorts:@[port] output:output];
                    if ([session_ canAddConnection:connection]) {
                        [session_ addConnection:connection];
                        running.connection = connection;
                    } else {
                        ok = false;
                    }
                } else {
                    ok = false;
                }
#endif
            } else if ([session_ canAddOutput:output]) {
                [session_ addOutput:output];
                running.connection = [output connectionWithMediaType:AVMediaTypeVideo];
            } else {
                ok = false;
            }
            if (ok) {
                running.videoOutput = output;
                running.delegate = delegate;
            }
        }
        [session_ commitConfiguration];
        if (ok) {
            running.video = true;
            running.width = width;
            running.height = height;
            running.frameRate = frameRate;
        }
        return ok;
    }

    bool prepareStill(std::size_t camera, const Entry& entry, RunningCamera& running)
    {
        if (running.photoOutput != nil) return true;
        [session_ beginConfiguration];
        bool ok = true;
        NSError* error = nil;
        if (running.input == nil) {
            running.input = [AVCaptureDeviceInput deviceInputWithDevice:entry.device error:&error];
            if (running.input == nil) ok = false;
            else if (multiCamera_) {
#if TARGET_OS_IOS
                if ([session_ canAddInput:running.input]) [(AVCaptureMultiCamSession*)session_ addInputWithNoConnections:running.input];
                else ok = false;
#endif
            } else if ([session_ canAddInput:running.input]) {
                [session_ addInput:running.input];
            } else {
                ok = false;
            }
        }
        if (ok) {
            AVCapturePhotoOutput* output = [[AVCapturePhotoOutput alloc] init];
#if TARGET_OS_IOS
            CMVideoDimensions largest {0, 0};
            for (NSValue* value in entry.device.activeFormat.supportedMaxPhotoDimensions) {
                CMVideoDimensions candidate;
                [value getValue:&candidate];
                if (std::int64_t(candidate.width) * candidate.height > std::int64_t(largest.width) * largest.height)
                    largest = candidate;
            }
            if (largest.width != 0) output.maxPhotoDimensions = largest;
            output.maxPhotoQualityPrioritization = AVCapturePhotoQualityPrioritizationQuality;
#endif
            if (multiCamera_) {
#if TARGET_OS_IOS
                AVCaptureInputPort* port = [running.input portsWithMediaType:AVMediaTypeVideo
                                                             sourceDeviceType:entry.device.deviceType
                                                         sourceDevicePosition:entry.device.position].firstObject;
                if (port != nil && [session_ canAddOutput:output]) {
                    [(AVCaptureMultiCamSession*)session_ addOutputWithNoConnections:output];
                    AVCaptureConnection* connection = [[AVCaptureConnection alloc] initWithInputPorts:@[port] output:output];
                    ok = [session_ canAddConnection:connection];
                    if (ok) [session_ addConnection:connection];
                } else {
                    ok = false;
                }
#endif
            } else if ([session_ canAddOutput:output]) {
                [session_ addOutput:output];
            } else {
                ok = false;
            }
            if (ok) running.photoOutput = output;
        }
        [session_ commitConfiguration];
        if (ok && !session_.running) [session_ startRunning];
        (void)camera;
        return ok;
    }

    std::vector<Entry> entries_;
    CameraSessionDelegate& delegate_;
    std::shared_ptr<SessionState> state_;
    AVCaptureSession* session_ {};
    dispatch_queue_t queue_;
    dispatch_queue_t outputQueue_;
    id interrupted_ {};
    id resumed_ {};
    id failed_ {};
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
    BOOL _wantRaw;
    std::shared_ptr<SessionState> _state;
    uml::platform::CameraStillResult _result;
    bool _failed;
}

- (instancetype)initWithCamera:(size_t)camera
                         token:(uint64_t)token
                       wantRaw:(BOOL)wantRaw
                         state:(std::shared_ptr<SessionState>)state
{
    if ((self = [super init])) {
        _camera = camera;
        _token = token;
        _wantRaw = wantRaw;
        _state = std::move(state);
        _result.token = token;
        _failed = false;
    }
    return self;
}

- (void)captureOutput:(AVCapturePhotoOutput*)output
    didFinishProcessingPhoto:(AVCapturePhoto*)photo
                       error:(NSError*)error
{
    (void)output;
    if (error != nil) {
        _failed = true;
        return;
    }
    NSData* data = photo.fileDataRepresentation;
#if TARGET_OS_IOS
    if (photo.isRawPhoto) {
        CVPixelBufferRef pixels = photo.pixelBuffer;
        if (pixels == nullptr || data == nil) {
            _failed = true;
            return;
        }
        const auto order = uml::platform::bayerOrderOf(CVPixelBufferGetPixelFormatType(pixels));
        const auto width = CVPixelBufferGetWidth(pixels);
        const auto height = CVPixelBufferGetHeight(pixels);
        if (order == uml::platform::BayerOrder::none || CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
            _failed = true;
            return;
        }
        const auto* base = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(pixels));
        const auto stride = CVPixelBufferGetBytesPerRow(pixels);
        _result.raw.resize(width * height);
        for (std::size_t row = 0; row < height; ++row)
            std::memcpy(_result.raw.data() + row * width, base + row * stride, width * sizeof(std::uint16_t));
        CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
        // The DNG that comes with the Bayer pixels states what the sensor is.
        const auto bytes = std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data.bytes), data.length);
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
        const auto* bytes = static_cast<const std::uint8_t*>(data.bytes);
        _result.jpeg.assign(bytes, bytes + data.length);
    }
}

- (void)captureOutput:(AVCapturePhotoOutput*)output
    didFinishCaptureForResolvedSettings:(AVCaptureResolvedPhotoSettings*)resolvedSettings
                                  error:(NSError*)error
{
    (void)output;
    (void)resolvedSettings;
    _result.ok = error == nil && !_failed && (!_result.jpeg.empty() || !_result.raw.empty());
    std::lock_guard lock(_state->mutex);
    if (_state->session) static_cast<uml::platform::AppleSession*>(_state->session)->deliverStill(_camera, std::move(_result));
}

@end
