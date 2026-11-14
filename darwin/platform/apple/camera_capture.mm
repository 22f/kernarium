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
#include "frame_bytes.hpp"
#include "key_path_observer.h"
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
#include <optional>
#include <set>
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

#if TARGET_OS_IOS
// Keeps a connection's pictures in the camera's own orientation, unturned and unmirrored, which its stated
// SENSOR_ORIENTATION describes (sensorOrientationOf): a device may turn or mirror a camera's pictures by default (some
// iPads turn their front camera's video by 180 degrees). A connection that refuses keeps what it has.
void keepSensorOrientation(AVCaptureConnection* connection)
{
    if (connection == nil) return;
    @try {
        if (connection.supportsVideoMirroring) {
            connection.automaticallyAdjustsVideoMirroring = NO;
            connection.videoMirrored = NO;
        }
        if (connection.videoRotationAngle != 0 && [connection isVideoRotationAngleSupported:0]) connection.videoRotationAngle = 0;
    } @catch (NSException* exception) {
        std::fprintf(stderr, "Camera: a connection kept its own orientation: %s\n",
                     exception.reason != nil ? exception.reason.UTF8String : "");
    }
}

// The same for a photo output: its stills stay in the camera's own orientation, without the turn to an earlier
// model's sensor orientation that the output otherwise applies where it supports one. The host's Exif, which
// would state a turn, is not passed on (jpeg_gap.hpp).
void keepStillOrientation(AVCapturePhotoOutput* output)
{
    // A front camera that states orientation 0 and supports the compensation has its stills rejected here (the session
    // record's camera slot says how); the fix for it waits for a bug report that shows what the photo output delivers.
    @try {
        if (output.cameraSensorOrientationCompensationSupported) output.cameraSensorOrientationCompensationEnabled = NO;
    } @catch (NSException* exception) {
        std::fprintf(stderr, "Camera: a photo output kept its orientation compensation: %s\n",
                     exception.reason != nil ? exception.reason.UTF8String : "");
    }
    keepSensorOrientation([output connectionWithMediaType:AVMediaTypeVideo]);
}
#endif

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

// The photo sizes a device format lists (supportedMaxPhotoDimensions) that a
// synchronous capture makes. AVFoundation services the 24 MP setting
// (5712x4284), where a format lists it, only as a deferred photo proxy, which
// this backend neither opts into nor receives (AVCapturePhotoOutput.h), so the
// size is not listed and no tier can offer it.
std::vector<CameraStillSize> listedPhotoSizes(AVCaptureDeviceFormat* format)
{
    constexpr CameraStillSize deferredOnly {5712, 4284};
    std::vector<CameraStillSize> listed;
    for (NSValue* value in format.supportedMaxPhotoDimensions) {
        CMVideoDimensions candidate;
        [value getValue:&candidate];
        if (candidate.width <= 0 || candidate.height <= 0) continue;
        const CameraStillSize size {std::uint32_t(candidate.width), std::uint32_t(candidate.height)};
        if (!(size == deferredOnly)) listed.push_back(size);
    }
    return listed;
}

[[nodiscard]] bool formatTakenByMultiCameraSession(AVCaptureDeviceFormat* format)
{
#if TARGET_OS_IOS
    return format != nil && format.multiCamSupported;
#else
    (void)format;
    return true;
#endif
}

// A camera's 4:2:0 formats as data for the choice of its anchor, with the device
// formats they stand for in the same order.
struct FormatSet {
    std::vector<VideoFormatFacts> facts;
    std::vector<AVCaptureDeviceFormat*> formats;
};

// A 4:2:0 device format as data; nothing for any other.
std::optional<VideoFormatFacts> factsOf(AVCaptureDeviceFormat* format)
{
    const FourCharCode subtype = CMFormatDescriptionGetMediaSubType(format.formatDescription);
    if (subtype != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange && subtype != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
        return std::nullopt;
    const CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
    if (dimensions.width <= 0 || dimensions.height <= 0) return std::nullopt;
    VideoFormatFacts facts;
    facts.width = std::uint32_t(dimensions.width);
    facts.height = std::uint32_t(dimensions.height);
    for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges)
        facts.frameRates.push_back({range.minFrameRate, range.maxFrameRate});
    facts.photoSizes = listedPhotoSizes(format);
    facts.fullRange = subtype == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    facts.multiCamera = formatTakenByMultiCameraSession(format);
    return facts;
}

FormatSet formatSetOf(AVCaptureDevice* device)
{
    FormatSet set;
    for (AVCaptureDeviceFormat* format in device.formats) {
        auto facts = factsOf(format);
        if (!facts) continue;
        set.facts.push_back(std::move(*facts));
        set.formats.push_back(format);
    }
    return set;
}

// What a video output delivers: NV12, at the size the stream needs. The
// anchor's own size arrives unless it decides another (outputSizesFor: the
// anchor's aspect ratio).
NSDictionary* videoSettingsOf(const VideoFormatFacts& facts, std::uint32_t width, std::uint32_t height);

// Sets what a video output delivers. An output left to configure its own buffer dimensions may have been set to
// deliver preview-sized buffers (the platform decides from the session's other outputs, a photo output of the full
// sensor size among them), and a size set then raises ("Width/height may not be changed when
// deliversPreviewSizedOutputBuffers is set to YES"): such an output is made to deliver the size asked for first. One
// that already delivers full-size buffers is left as the platform configured it.
void setVideoOutputSettings(AVCaptureVideoDataOutput* output, NSDictionary* settings)
{
#if TARGET_OS_IOS
    if (output.deliversPreviewSizedOutputBuffers) {
        std::fprintf(stderr, "Camera: a stream's output delivered preview-sized buffers: set to deliver the size asked for\n");
        output.automaticallyConfiguresOutputBufferDimensions = NO;
        output.deliversPreviewSizedOutputBuffers = NO;
    }
#endif
    output.videoSettings = settings;
}

NSDictionary* videoSettingsOf(const VideoFormatFacts& facts, std::uint32_t width, std::uint32_t height)
{
    NSMutableDictionary* settings = [NSMutableDictionary dictionaryWithObject:@(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
                                                                      forKey:(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey];
    if (const auto frame = outputSizesFor(facts, width, height, 0).frame) {
        settings[(__bridge NSString*)kCVPixelBufferWidthKey] = @(frame->width);
        settings[(__bridge NSString*)kCVPixelBufferHeightKey] = @(frame->height);
    }
    return settings;
}

// One line per camera, once for the life of the process: the device facts the
// anchor is chosen from (a format's size, its rate spans, `m` when a
// multi-camera session takes it, `f` when it is full range and the photo sizes
// it lists in braces). No identifier and no frame data.
void logFormatTables(const std::vector<Entry>& entries)
{
    static std::mutex mutex;
    static std::set<std::string> logged;
    std::lock_guard lock(mutex);
    for (std::size_t camera = 0; camera < entries.size(); ++camera) {
        if (!logged.insert(entries[camera].info.identifier).second) continue;
        std::string line = "Camera: camera " + std::to_string(camera) + " formats:";
        char text[64];
        for (const auto& format : entries[camera].info.captureFormats) {
            line += " " + std::to_string(format.width) + "x" + std::to_string(format.height);
            for (const auto& span : format.frameRates) {
                std::snprintf(text, sizeof(text), " %g-%g", span.minimum, span.maximum);
                line += text;
            }
            if (format.multiCamera) line += " m";
            if (format.fullRange) line += " f";
            line += " {";
            for (std::size_t index = 0; index < format.photoSizes.size(); ++index)
                line += (index != 0 ? " " : "") + std::to_string(format.photoSizes[index].width) + "x" +
                    std::to_string(format.photoSizes[index].height);
            line += "};";
        }
        std::fprintf(stderr, "%s\n", line.c_str());
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
                if (std::uint64_t(size.width) * size.height > std::uint64_t(largestPhoto.width) * largestPhoto.height)
                    largestPhoto = size;
            }
            if (std::uint64_t(dimensions.width) * dimensions.height > std::uint64_t(pixelWidth) * pixelHeight) {
                pixelWidth = std::uint32_t(dimensions.width);
                pixelHeight = std::uint32_t(dimensions.height);
            }
            for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges) {
                // A rate within 0.15 % of a whole number is that number (the Mac's
                // external camera lists 29.97, 23.976 and 14.99).
                const auto whole = wholeRatesOf({range.minFrameRate, range.maxFrameRate});
                if (!whole) continue;
                CameraVideoFormat candidate;
                candidate.width = std::uint32_t(dimensions.width);
                candidate.height = std::uint32_t(dimensions.height);
                candidate.minimumFrameRate = whole->minimum;
                candidate.maximumFrameRate = whole->maximum;
                candidate.multiCamera = multiCamera;
                if (std::find(info.videoFormats.begin(), info.videoFormats.end(), candidate) == info.videoFormats.end())
                    info.videoFormats.push_back(candidate);
            }
        }
        if (info.videoFormats.empty()) continue;
        // The formats go to the device as data: it picks each camera's anchor and
        // offers what the anchor does (photo_dimensions.hpp).
        {
            const FormatSet set = formatSetOf(device);
            info.captureFormats = set.facts;
            // Every size some format makes: what the host can do at all.
            const auto made = stillsMadeByAnyFormat(set.facts, 0);
            info.stillSizes.assign(made.begin(), made.end());
        }
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
        // The platform states each camera's mounting; the connections keep the pictures in it (keepSensorOrientation).
        AVCaptureDeviceRotationCoordinator* rotation = [[AVCaptureDeviceRotationCoordinator alloc] initWithDevice:device previewLayer:nil];
        info.sensorOrientation = sensorOrientationOf(
            info.position, [rotation videoRotationAngleRelativeToDeviceOrientation:AVCaptureVideoOrientationPortrait]);
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
    logFormatTables(entries);
    return entries;
}

class AppleSession;

// One camera's output delegate. It forwards to the session through a state
// that outlives it, so a callback that races the session's end finds it gone.
struct SessionState {
    std::mutex mutex;
    AppleSession* session {};
    // Which samples are from before the last repair of the cameras (RepairBoundary): read by the output callbacks
    // as they are entered, set by the recovery, both with `mutex` held for the decision.
    RepairBoundary boundary;
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
                  maximumBytes:(uint64_t)maximumBytes
                       wantRaw:(BOOL)wantRaw
                       attempt:(unsigned)attempt
                         state:(std::shared_ptr<SessionState>)state;
@end

namespace uml::platform {
namespace {


// The format a camera runs in and what its choice stands on: the device format,
// its facts (equal to what discovery listed) and the rates the guest is offered.
struct Anchor {
    AVCaptureDeviceFormat* format {};
    VideoFormatFacts facts;
    std::vector<WholeRates> rates;
};

// The stream's size and rate (StreamCache) are what the stream was asked for, which the anchor covers, and what the
// backend believes the device holds of them.
struct RunningCamera : StreamCache {
    AVCaptureDeviceInput* input {};
    AVCaptureVideoDataOutput* videoOutput {};
    AVCapturePhotoOutput* photoOutput {};
    // When the session last committed a configuration with a photo maximum that needs the sensor's full-resolution
    // mode (host nanoseconds); zero if there is none (photoPipelinesSettle, stillSettleNanoseconds).
    std::uint64_t photoMaximumCommittedAt {};
    UMLCameraOutputDelegate* delegate {};
    AVCaptureConnection* connection {};
    AVCaptureDeviceFormat* format {};
#if TARGET_OS_IOS
    UMLKeyPathObserver* pressure {};
#endif
    // What the system's pressure leaves of a running stream's rate: made when the
    // stream starts, ended when it stops or its session does. The observer's
    // handler and the work it queues hold it themselves.
    std::shared_ptr<PressureWatch> watch;
    CameraSettings settings;
    std::uint32_t focusSerial {};
    std::uint32_t focusDistanceSerial {};
    // Settings have been applied to the device at least once, so that a format
    // change (which reverts some of them) can put them back.
    bool applied {};
};

// The rate a stream runs at while the system's pressure caps it, and how it is
// programmed on the anchor (programmedRate).
std::optional<ProgrammedRate> programmedFor(const Anchor& anchor, std::uint32_t requested, std::uint32_t pressureCap)
{
    return programmedRate(anchor.facts, anchor.rates, requested, pressureCap);
}

// The cap the system's pressure puts on a camera's stream now.
std::uint32_t pressureCapOf(const RunningCamera& running)
{
    return running.watch ? running.watch->cap() : 0;
}

// What a device holds of its format, frame durations and manual exposure, as the
// device reports them (uml::platform::DeviceTiming): a long manual exposure
// lengthens the maximum beyond what the rate asked for, and a shorter maximum
// shortens the exposure, so a refused change is undone with these, not with a
// duration recomputed from a rate.
CameraTime cameraTimeOf(CMTime time)
{
    return {time.value, time.timescale};
}

CMTime cmTimeOf(CameraTime time)
{
    return CMTimeMake(time.value, time.scale);
}

// A time on the capture's clock, in nanoseconds; nothing for one that is not a number (invalid, indefinite, infinite).
std::optional<std::int64_t> nanosecondsOf(CMTime time)
{
    if (!CMTIME_IS_NUMERIC(time)) return std::nullopt;
    const CMTime scaled = CMTimeConvertScale(time, 1'000'000'000, kCMTimeRoundingMethod_RoundTowardZero);
    return CMTIME_IS_NUMERIC(scaled) ? std::optional<std::int64_t>(scaled.value) : std::nullopt;
}

DeviceTiming timingOf(AVCaptureDevice* device)
{
    DeviceTiming timing;
    const NSUInteger format = [device.formats indexOfObject:device.activeFormat];
    timing.format = format == NSNotFound ? ~0u : std::uint32_t(format);
    timing.minimum = cameraTimeOf(device.activeVideoMinFrameDuration);
    timing.maximum = cameraTimeOf(device.activeVideoMaxFrameDuration);
#if TARGET_OS_IOS
    timing.exposure.custom = device.exposureMode == AVCaptureExposureModeCustom;
    timing.exposure.duration = cameraTimeOf(device.exposureDuration);
    timing.exposure.iso = device.ISO;
#endif
    return timing;
}

// The calls uml::platform::putBackTiming makes on a device.
TimingSteps timingStepsOf(AVCaptureDevice* device)
{
    const auto guarded = [](const std::function<void()>& write) {
        @try {
            write();
            return true;
        } @catch (NSException*) {
            return false;
        }
    };
    TimingSteps steps;
    steps.lock = [device] {
        NSError* error = nil;
        return [device lockForConfiguration:&error] == YES;
    };
    steps.unlock = [device] { [device unlockForConfiguration]; };
    steps.read = [device] { return timingOf(device); };
    steps.setFormat = [device, guarded](std::uint32_t format) {
        return format < device.formats.count && guarded([&] { device.activeFormat = device.formats[format]; });
    };
    // A duration the device did not report (a zero scale: an invalid time) is not written back.
    steps.setMinimum = [device, guarded](CameraTime time) {
        return time.scale <= 0 || guarded([&] { device.activeVideoMinFrameDuration = cmTimeOf(time); });
    };
    steps.setMaximum = [device, guarded](CameraTime time) {
        return time.scale <= 0 || guarded([&] { device.activeVideoMaxFrameDuration = cmTimeOf(time); });
    };
    steps.setCustomExposure = [device, guarded](CameraTime duration, float iso) {
#if TARGET_OS_IOS
        return guarded([&] { [device setExposureModeCustomWithDuration:cmTimeOf(duration) ISO:iso completionHandler:nil]; });
#else
        (void)device;
        (void)guarded;
        (void)duration;
        (void)iso;
        return false;
#endif
    };
    return steps;
}

// Puts a device in its anchor: the format only when it is not the active one
// (setting it again would revert the device's focus and exposure points), and,
// for a stream, the frame durations the rate programs. False when the device
// refuses.
bool activate(AVCaptureDevice* device, const Anchor& anchor, const std::optional<ProgrammedRate>& rate)
{
    NSError* error = nil;
    if (![device lockForConfiguration:&error]) return false;
    bool ok = true;
    @try {
        // A format the session does not take raises instead of failing.
        if (device.activeFormat != anchor.format) device.activeFormat = anchor.format;
        if (rate) {
            CMTime duration = CMTimeMake(1, std::int32_t(rate->rate));
            if (rate->setting.duration != FrameDuration::reciprocal) {
                // The span's own duration, where 1 / the nominal rate would fall
                // a hair outside it.
                AVFrameRateRange* span = anchor.format.videoSupportedFrameRateRanges[rate->setting.span];
                duration = rate->setting.duration == FrameDuration::spanShortest ? span.minFrameDuration : span.maxFrameDuration;
            }
            // The one that moves out of the other's way goes first (maximumFirst): the device may be where a refused
            // change left it, at the durations of a faster stream.
            if (maximumFirst(cameraTimeOf(device.activeVideoMaxFrameDuration), cameraTimeOf(duration))) {
                device.activeVideoMaxFrameDuration = duration;
                device.activeVideoMinFrameDuration = duration;
            } else {
                device.activeVideoMinFrameDuration = duration;
                device.activeVideoMaxFrameDuration = duration;
            }
        }
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
        // An interruption that ends, and a restart after an error, are recoveries:
        // the system may have changed a device's format meanwhile, so the session
        // puts its cameras back in their anchors first and only then says it is
        // back (recoverAfterInterruption); the client delivers nothing from a
        // camera until it is.
        const auto recover = [state] {
            std::lock_guard lock(state->mutex);
            if (state->session) state->session->requestRecovery();
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
                                   usingBlock:^(NSNotification*) { recover(); }];
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
                                       if (restarting->exchange(false)) recover();
                                   }];
    }

    ~AppleSession() override { stop(); }

    // Runs `work` on the session's queue and waits for it. A lambda keeps the
    // references it captures as references, which a block would copy.
    void onQueue(const std::function<void()>& work) { dispatch_sync(queue_, ^{ work(); }); }

    // Resolves each camera's anchor against the device's own formats: the facts
    // the device chose from must be the facts the device lists now, or the camera
    // has no anchor and its streams and stills are refused.
    void setAnchors(std::span<const std::optional<std::size_t>> formatOfCamera, const CameraLimits& limits) override
    {
        if (stopped_) return;
        onQueue([&] {
            limits_ = limits;
            anchors_.assign(entries_.size(), nullptr);
            refusals_ = RefusalLog {};
            for (std::size_t camera = 0; camera < entries_.size() && camera < formatOfCamera.size(); ++camera) {
                if (!formatOfCamera[camera]) continue;
                const std::size_t index = *formatOfCamera[camera];
                const FormatSet set = formatSetOf(entries_[camera].device);
                const auto& listed = entries_[camera].info.captureFormats;
                if (index >= set.facts.size() || index >= listed.size() || !(set.facts[index] == listed[index])) {
                    refuse(camera, Refusal::formatChanged, "the anchor is not the format discovery listed");
                    continue;
                }
                auto anchor = std::make_shared<Anchor>();
                anchor->format = set.formats[index];
                // The facts the device offered from: a front camera's photo sizes within its limit, so the photo
                // output's maximum (photoMaximum) and the sizes it makes are the sizes offered.
                anchor->facts = limitedStillFormats(entries_[camera].info.position, set.facts)[index];
                anchor->rates = offeredRatesOf(anchor->facts, videoRateCap);
                anchors_[camera] = std::move(anchor);
            }
        });
    }

    // Called under the state's mutex while the session lives, like
    // deliverStill's release: the block runs before the session can end.
    void requestRecovery() { dispatch_async(queue_, ^{ recoverAfterInterruption(); }); }

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
            refusals_.reset(camera);
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
        if (running.mayLeaveSettings(force, running.settings == settings && running.focusSerial == settings.focusTriggerSerial)) return;
        // What the device holds is not known (a restore was refused): everything is written.
        force = force || running.unrestored;
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
            } @catch (NSException* exception) {
                refuse(camera, Refusal::raised, std::string("a still: ") + exception.reason.UTF8String);
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

    // `attempt` is the capture of this request that starts (from one): a photo that comes back at a size other than the
    // one asked for makes the next (uml::platform::stillSizeOutcome). `waited` is what the earlier attempts waited for
    // the settle time, of which the next has what is left.
    bool startStill(std::size_t camera, const CameraStillRequest& request, unsigned attempt = 1, std::uint64_t waited = 0)
    {
        const Entry& entry = entries_[camera];
        auto& running = cameras_[camera];
        if (!prepareStill(camera)) return false;
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
            if (rawFormat == 0) {
                refuse(camera, Refusal::rawUnavailable, "the photo output lists none");
                return false;
            }
            settings = request.jpegWidth != 0
                ? [AVCapturePhotoSettings photoSettingsWithRawPixelFormatType:rawFormat processedFormat:jpeg]
                : [AVCapturePhotoSettings photoSettingsWithRawPixelFormatType:rawFormat];
#else
            refuse(camera, Refusal::rawUnavailable, "macOS has no Bayer still");
            return false;
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
        // the output makes up to the size set at admission (the anchor's
        // photoMaximum), and the anchor's offer holds only sizes it makes, so a
        // size that fails here is one that was not offered.
        const auto listed = listedPhotoSizes(entry.device.activeFormat);
        const CMVideoDimensions limit = output.maxPhotoDimensions;
        const uml::platform::CameraStillSize outputMaximum {std::uint32_t(std::max(0, int(limit.width))),
                                                            std::uint32_t(std::max(0, int(limit.height)))};
        const uml::platform::CameraStillSize wanted {request.jpegWidth, request.jpegHeight};
        const auto chosen = request.jpegWidth != 0 ? uml::platform::exactPhotoSize(listed, wanted, outputMaximum)
                                                   : uml::platform::coveringPhotoSize(listed, {}, outputMaximum);
        if (request.jpegWidth != 0 && !chosen) {
            refuse(camera, Refusal::stillNotMade,
                   "a JPEG of " + std::to_string(wanted.width) + "x" + std::to_string(wanted.height) + " (output maximum " +
                       std::to_string(outputMaximum.width) + "x" + std::to_string(outputMaximum.height) + ", " +
                       std::to_string(listed.size()) + " sizes listed)");
            return false;
        }
        if (chosen) settings.maxPhotoDimensions = CMVideoDimensions {std::int32_t(chosen->width), std::int32_t(chosen->height)};
        UMLPhotoDelegate* delegate = [[UMLPhotoDelegate alloc] initWithCamera:camera
                                                                         token:request.token
                                                                      wantJpeg:request.jpegWidth != 0
                                                                      jpegSize:wanted
                                                                  maximumBytes:request.maximumJpegBytes
                                                                       wantRaw:request.raw
                                                                       attempt:attempt
                                                                         state:state_];
        // The photo output holds its delegate weakly, so the session keeps it
        // until the capture finishes, or none of its callbacks would arrive.
        {
            std::lock_guard lock(photoMutex_);
            photos_[request.token] = {camera, delegate, request, attempt, waited};
        }
        // The pipeline applies a maximum that needs the sensor's full-resolution mode some time after the session commits
        // a configuration, and a capture that starts before is served at the binned size: a still starts when that time
        // has passed (the first after the admission, and one after a stream started, resized or stopped, or after a
        // repair), scheduled on the session's queue, which stays free for the other cameras and the stop meanwhile. The waits of
        // a request are bounded (stillWaitBudgetNanoseconds): commits that keep coming fail it instead of postponing it.
        const auto step = uml::platform::scheduledStillStep(false, true, running.photoMaximumCommittedAt, hostNow(), waited);
        if (step.step == ScheduledStillStep::fail) {
            // Commits kept postponing it past its budget: the guest is told at once (the caller's failure).
            std::fprintf(stderr, "Camera: camera %zu: the still failed: the photo output's full-resolution maximum did not settle within %llu ms\n",
                         camera, static_cast<unsigned long long>(stillWaitBudgetNanoseconds / 1000000));
            std::lock_guard lock(photoMutex_);
            photos_.erase(request.token);
            return false;
        }
        if (step.step == ScheduledStillStep::wait) {
            std::fprintf(stderr, "Camera: camera %zu: the still starts in %llu ms, when the photo output's full-resolution maximum has taken effect\n",
                         camera, static_cast<unsigned long long>(step.waitNanoseconds / 1000000));
            {
                std::lock_guard lock(photoMutex_);
                photos_[request.token].waited = step.waitedNanoseconds;
            }
            scheduleStill(camera, request.token, output, settings, delegate, step.waitNanoseconds);
            return true;
        }
        return captureStill(camera, output, settings, delegate);
    }

    bool captureStill(std::size_t camera, AVCapturePhotoOutput* output, AVCapturePhotoSettings* settings, UMLPhotoDelegate* delegate)
    {
        [output capturePhotoWithSettings:settings delegate:delegate];
        refusals_.reset(camera);
        return true;
    }

    // Starts a registered capture `wait` nanoseconds from now on the session's queue. The block holds the state, not the
    // session: a session that ended (stop() clears the state's pointer first and drains the queue after) is not
    // touched, and one that is running is held by that drain until the block is done.
    void scheduleStill(std::size_t camera, std::uint64_t token, AVCapturePhotoOutput* output, AVCapturePhotoSettings* settings,
                       UMLPhotoDelegate* delegate, std::uint64_t wait)
    {
        std::shared_ptr<SessionState> state = state_;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, std::int64_t(wait)), queue_, ^{
            AppleSession* session = nullptr;
            {
                std::lock_guard lock(state->mutex);
                session = static_cast<AppleSession*>(state->session);
            }
            if (session == nullptr) {
                std::fprintf(stderr, "Camera: a scheduled still was dropped: the session ended\n");
                return;
            }
            session->fireStill(camera, token, output, settings, delegate);
        });
    }

    // The time of a scheduled capture came (on the session's queue).
    void fireStill(std::size_t camera, std::uint64_t token, AVCapturePhotoOutput* output, AVCapturePhotoSettings* settings,
                   UMLPhotoDelegate* delegate)
    {
        bool known = false;
        std::uint64_t waited = 0;
        {
            std::lock_guard lock(photoMutex_);
            const auto found = photos_.find(token);
            known = found != photos_.end() && found->second.camera == camera;
            if (known) waited = found->second.waited;
        }
        const auto running = cameras_.find(camera);
        const std::uint64_t committedAt = running == cameras_.end() ? 0 : running->second.photoMaximumCommittedAt;
        const auto step = uml::platform::scheduledStillStep(stopped_, known && running != cameras_.end(), committedAt, hostNow(), waited);
        switch (step.step) {
        case ScheduledStillStep::drop:
            std::fprintf(stderr, "Camera: a scheduled still was dropped: the session ended or the request is gone\n");
            return;
        case ScheduledStillStep::wait: {
            {
                std::lock_guard lock(photoMutex_);
                const auto found = photos_.find(token);
                if (found != photos_.end()) found->second.waited = step.waitedNanoseconds;
            }
            scheduleStill(camera, token, output, settings, delegate, step.waitNanoseconds);
            return;
        }
        case ScheduledStillStep::fail:
            // Commits kept postponing it past its budget: failed once, here, and nothing is scheduled again.
            std::fprintf(stderr, "Camera: camera %zu: the still failed: the photo output's full-resolution maximum did not settle within %llu ms\n",
                         camera, static_cast<unsigned long long>(stillWaitBudgetNanoseconds / 1000000));
            failStill(camera, token);
            return;
        case ScheduledStillStep::fire:
            break;
        }
        bool started = false;
        @try {
            started = captureStill(camera, output, settings, delegate);
        } @catch (NSException* exception) {
            refuse(camera, Refusal::raised, std::string("a still: ") + exception.reason.UTF8String);
        }
        // The capture fails at once: the guest is told, never left to wait for it.
        if (!started) failStill(camera, token);
    }

    // The result of a capture that cannot be made, delivered at once (the retry's failure and a scheduled capture's).
    void failStill(std::size_t camera, std::uint64_t token)
    {
        CameraStillResult failed;
        failed.token = token;
        failed.failure.stage = CameraStillStage::notStarted;
        std::lock_guard lock(state_->mutex);
        if (state_->session) deliverStill(camera, std::move(failed));
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
        // The pressure watches end first, inside the queue and before it is
        // drained for the last time: nothing a notification queued behind this
        // programs a device afterwards.
        onQueue([&, session] {
            for (auto& [camera, running] : cameras_) {
                (void)camera;
                endPressureWatch(running);
            }
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
        cameras_.clear();
        session_ = nil;
    }

    // Called by the output delegates, under the state's mutex.
    void deliver(CameraEvent event) { delegate_.cameraEvent(event); }

    void deliverFrame(std::size_t camera, CMSampleBufferRef sample)
    {
        if (camera >= entries_.size()) return;
        // What the capture made before the cameras were put back in their anchors does not get here
        // (RepairBoundary, applied by the output delegate before it calls this).
        const CMTime presentation = CMSampleBufferGetPresentationTimeStamp(sample);
        const std::uint64_t capturedAt = CMTIME_IS_VALID(presentation) ? std::uint64_t(CMTimeGetSeconds(presentation) * 1e9) : hostNow();
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
        frame.timestampNanoseconds = capturedAt;
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

    // The capture of a request came back at a size other than the one asked for and has another attempt: it is made
    // again on the session's queue, and the camera stays in the session for it. False when there is none to make
    // (the request is gone, or the session is ending), and the caller fails the capture. Called by the photo delegate,
    // under the state's mutex.
    bool remakeStill(std::size_t camera, std::uint64_t token)
    {
        if (stopped_ || camera >= entries_.size()) return false;
        {
            std::lock_guard lock(photoMutex_);
            const auto found = photos_.find(token);
            if (found == photos_.end() || found->second.camera != camera) return false;
        }
        dispatch_async(queue_, ^{ remakeStillOnQueue(camera, token); });
        return true;
    }

    void remakeStillOnQueue(std::size_t camera, std::uint64_t token)
    {
        Photo photo;
        {
            std::lock_guard lock(photoMutex_);
            const auto found = photos_.find(token);
            if (found == photos_.end()) return;
            photo = found->second;
        }
        bool started = false;
        @try {
            started = !stopped_ && startStill(camera, photo.request, photo.attempt + 1, photo.waited);
        } @catch (NSException* exception) {
            refuse(camera, Refusal::raised, std::string("a still: ") + exception.reason.UTF8String);
        }
        // The capture fails at once: the guest is told, never left to wait for it.
        if (!started) failStill(camera, token);
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
    // Logs a refusal once per camera and reason until the camera's configuration
    // changes (RefusalLog), whoever asks again and however often.
    void refuse(std::size_t camera, Refusal reason, const std::string& detail = {})
    {
        if (!refusals_.first(camera, reason)) return;
        std::fprintf(stderr, "Camera: camera %zu is refused (%s)%s%s\n", camera, std::string(refusalName(reason)).c_str(),
                     detail.empty() ? "" : ": ", detail.c_str());
    }

    // Runs one platform call; an exception it raises is a refusal.
    bool guarded(std::size_t camera, const std::function<bool()>& work)
    {
        @try {
            return work();
        } @catch (NSException* exception) {
            refuse(camera, Refusal::raised, exception.reason != nil ? exception.reason.UTF8String : "");
            return false;
        }
    }

    // Puts a device's timing back as a refused change found it (putBackTiming). A
    // device that would not be put back is logged, once, and what the camera's
    // stream and settings are believed to hold is no longer known: the next
    // request for the stream writes the format, the durations and the size again,
    // and the settings with them, or fails (StreamCache).
    void restoreTiming(std::size_t camera, AVCaptureDevice* device, const std::optional<DeviceTiming>& held)
    {
        if (!held) return;
        const bool ok = guarded(camera, [&] { return putBackTiming(*held, timingStepsOf(device)); });
        if (ok) return;
        cameras_[camera].restoreRefused();
        refuse(camera, Refusal::notRestored);
    }

    const Anchor* anchorOf(std::size_t camera) const
    {
        return camera < anchors_.size() ? anchors_[camera].get() : nullptr;
    }

    // The cameras with an input in the session: those that stream and those
    // that only take stills, a still in flight included.
    std::vector<std::size_t> peersOf(std::size_t camera) const
    {
        std::vector<std::size_t> result;
        for (const auto& [index, running] : cameras_)
            if (index != camera && running.input != nil) result.push_back(index);
        return result;
    }

    // Brings a camera into the session as its anchor: the input, the format and
    // the rate, and the outputs it gains (a stream's video output and a still's
    // photo output). One transaction for every entry (uml::platform::admit): a
    // refusal removes what this call added and nothing else, so a camera that
    // was in the session stays as it was, in its anchor.
    bool admitCamera(std::size_t camera, bool video, bool photo, std::uint32_t width, std::uint32_t height, std::uint32_t frameRate)
    {
        const Entry& entry = entries_[camera];
        auto& running = cameras_[camera];
        const Anchor* anchor = anchorOf(camera);
        if (anchor == nullptr) {
            refuse(camera, Refusal::noAnchor);
            return false;
        }
        // A multi-camera session takes the formats it lists for every input,
        // whatever each is for: this anchor and every other input's must be one.
        const auto peers = multiCamera_ ? peersOf(camera) : std::vector<std::size_t> {};
        auto peerFormats = std::make_unique<bool[]>(peers.size() + 1);
        for (std::size_t index = 0; index < peers.size(); ++index) {
            const Anchor* other = anchorOf(peers[index]);
            peerFormats[index] = other != nullptr && other->facts.multiCamera;
        }
        if (!mayJoin(!peers.empty(), anchor->facts.multiCamera, std::span<const bool>(peerFormats.get(), peers.size()))) {
            refuse(camera, Refusal::notJoinable);
            return false;
        }
        std::optional<ProgrammedRate> rate;
        if (video) {
            rate = programmedFor(*anchor, frameRate, pressureCapOf(running));
            if (!rate) {
                refuse(camera, Refusal::raised, "the anchor takes no rate near " + std::to_string(frameRate));
                return false;
            }
        }
        AVCaptureDeviceFormat* const activeBefore = entry.device.activeFormat;
        std::optional<DeviceTiming> held;
        AVCaptureVideoDataOutput* newVideo = nil;
        UMLCameraOutputDelegate* newDelegate = nil;
        AVCaptureConnection* newConnection = nil;
        AVCapturePhotoOutput* newPhoto = nil;
        AdmitPlan plan;
        plan.addInput = running.input == nil;
        plan.activate = plan.addInput || video;
        plan.addVideo = video && running.videoOutput == nil;
        plan.addPhoto = photo && running.photoOutput == nil;
        AdmitSteps steps;
        steps.begin = [&] { [session_ beginConfiguration]; };
        steps.commit = [&] {
            [session_ commitConfiguration];
            photoPipelinesSettle();
        };
        steps.addInput = [&] { return guarded(camera, [&] { return addInput(entry, running); }); };
        steps.snapshotTiming = [&] { held = timingOf(entry.device); };
        steps.restoreTiming = [&] { restoreTiming(camera, entry.device, held); };
        steps.activate = [&] { return guarded(camera, [&] { return activate(entry.device, *anchor, rate); }); };
        steps.addVideo = [&] {
            return guarded(camera, [&] {
                AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
                output.videoSettings = @{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey:
                                             @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)};
                output.alwaysDiscardsLateVideoFrames = YES;
                UMLCameraOutputDelegate* delegate = [[UMLCameraOutputDelegate alloc] initWithCamera:camera state:state_];
                [output setSampleBufferDelegate:delegate queue:outputQueue_];
                newDelegate = delegate;
                if (multiCamera_) {
#if TARGET_OS_IOS
                    AVCaptureInputPort* port = [running.input portsWithMediaType:AVMediaTypeVideo
                                                                 sourceDeviceType:entry.device.deviceType
                                                             sourceDevicePosition:entry.device.position].firstObject;
                    if (port == nil || ![session_ canAddOutput:output]) return false;
                    [(AVCaptureMultiCamSession*)session_ addOutputWithNoConnections:output];
                    newVideo = output;
                    AVCaptureConnection* connection = [[AVCaptureConnection alloc] initWithInputPorts:@[port] output:output];
                    if (![session_ canAddConnection:connection]) return false;
                    [session_ addConnection:connection];
                    newConnection = connection;
#else
                    return false;
#endif
                } else {
                    if (![session_ canAddOutput:output]) return false;
                    [session_ addOutput:output];
                    newVideo = output;
                    newConnection = [output connectionWithMediaType:AVMediaTypeVideo];
                }
                // The stream's size, scaled by the platform from the anchor's at
                // the anchor's aspect ratio: set once the output has its source.
                setVideoOutputSettings(output, videoSettingsOf(anchor->facts, width, height));
#if TARGET_OS_IOS
                keepSensorOrientation(newConnection);
#endif
                return true;
            });
        };
        steps.addPhoto = [&] {
            return guarded(camera, [&] {
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
                        newPhoto = output;
                        AVCaptureConnection* connection = [[AVCaptureConnection alloc] initWithInputPorts:@[port] output:output];
                        if (![session_ canAddConnection:connection]) return false;
                        [session_ addConnection:connection];
                        keepStillOrientation(output);
                        return true;
#else
                        return false;
#endif
                    }
                    if (![session_ canAddOutput:output]) return false;
                    [session_ addOutput:output];
                    newPhoto = output;
#if TARGET_OS_IOS
                    keepStillOrientation(output);
#endif
                    return true;
                };
                // The output makes photos up to the anchor's photoMaximum, which
                // AVFoundation requires to be one of the active format's sizes
                // of the output's source: set after the output is attached.
                const auto setMaximum = [&](CameraStillSize size) -> bool {
                    @try {
                        output.maxPhotoDimensions = CMVideoDimensions {std::int32_t(size.width), std::int32_t(size.height)};
                        return true;
                    } @catch (NSException* exception) {
                        // The first refusal's detail is the one logged; the admission's own
                        // refusal of the output after it is the same reason and stays quiet.
                        refuse(camera, Refusal::outputRefused,
                               "the photo output refused photos up to " + std::to_string(size.width) + "x" + std::to_string(size.height) +
                                   ": " + (exception.reason != nil ? exception.reason.UTF8String : ""));
                        return false;
                    }
                };
                const auto detach = [&]() {
                    @try {
                        [session_ removeOutput:output];
                    } @catch (NSException*) {
                    }
                    newPhoto = nil;
                };
                return preparePhotoOutput(photoMaximum(anchor->facts, limits_.maximumStillPixels), {attach, setMaximum, detach}) ==
                    PhotoOutputPreparation::ready;
            });
        };
        steps.overBudget = [&] { return overBudget(); };
        steps.removeVideo = [&] {
            if (newVideo == nil) return;
            @try {
                [session_ removeOutput:newVideo];
            } @catch (NSException*) {
            }
            newVideo = nil;
            newDelegate = nil;
            newConnection = nil;
        };
        steps.removePhoto = [&] {
            if (newPhoto == nil) return;
            @try {
                [session_ removeOutput:newPhoto];
            } @catch (NSException*) {
            }
            newPhoto = nil;
        };
        steps.removeInput = [&] {
            if (running.input == nil) return;
            @try {
                [session_ removeInput:running.input];
            } @catch (NSException*) {
            }
            running.input = nil;
            running.format = nil;
        };
        const Admission result = admit(plan, steps);
        if (result != Admission::admitted) {
            switch (result) {
            case Admission::inputRefused: refuse(camera, Refusal::inputRefused); break;
            case Admission::formatRefused: refuse(camera, Refusal::raised, "the device refused its anchor"); break;
            case Admission::videoRefused:
            case Admission::photoRefused: refuse(camera, Refusal::outputRefused); break;
            case Admission::overBudget: refuse(camera, Refusal::overBudget); break;
            case Admission::admitted: break;
            }
            return false;
        }
        if (newVideo != nil) {
            running.videoOutput = newVideo;
            running.delegate = newDelegate;
            running.connection = newConnection;
        }
        if (newPhoto != nil) {
            running.photoOutput = newPhoto;
            photoPipelinesSettle();
        }
        running.format = anchor->format;
        // The timing written again is known; what was applied before is written again after it.
        const bool settingsStale = plan.activate && running.timingWritten();
        if (video) {
            running.video = true;
            running.width = width;
            running.height = height;
            running.frameRate = frameRate;
            beginPressureWatch(entry, running, anchors_[camera], frameRate);
        }
        refusals_.reset(camera);
        // A format change reverts some settings of the device: put them back.
        if ((activeBefore != anchor->format || settingsStale) && running.applied) applyOnQueue(camera, running.settings, true);
        logJoin(camera);
        startSession();
        return true;
    }

    // A running stream changes size or rate (uml::platform::resizeStream): the
    // durations first, then the size; a refusal programs the old durations again.
    bool resizeCamera(std::size_t camera, std::uint32_t width, std::uint32_t height, std::uint32_t frameRate)
    {
        const Entry& entry = entries_[camera];
        auto& running = cameras_[camera];
        const Anchor* anchor = anchorOf(camera);
        if (anchor == nullptr) {
            refuse(camera, Refusal::noAnchor);
            return false;
        }
        const bool rateChanges = running.rateChanges(frameRate);
        const bool sizeChanges = running.sizeChanges(width, height);
        std::optional<DeviceTiming> held;
        ResizeSteps steps;
        steps.begin = [&] { [session_ beginConfiguration]; };
        steps.commit = [&] {
            [session_ commitConfiguration];
            photoPipelinesSettle();
        };
        steps.snapshotTiming = [&] { held = timingOf(entry.device); };
        if (rateChanges)
            steps.setDurations = [&] {
                return guarded(camera, [&] {
                    const auto rate = programmedFor(*anchor, frameRate, pressureCapOf(running));
                    return rate && activate(entry.device, *anchor, rate);
                });
            };
        if (sizeChanges)
            steps.setVideoSize = [&] {
                return guarded(camera, [&] {
                    setVideoOutputSettings(running.videoOutput, videoSettingsOf(anchor->facts, width, height));
                    return true;
                });
            };
        steps.restoreTiming = [&] { restoreTiming(camera, entry.device, held); };
        const Resizing result = resizeStream(steps);
        if (result != Resizing::resized) {
            refuse(camera, result == Resizing::durationsRefused ? Refusal::raised : Refusal::outputRefused,
                   result == Resizing::durationsRefused ? "the device refused the rate" : "the output refused the size");
            return false;
        }
        running.width = width;
        running.height = height;
        running.frameRate = frameRate;
        if (running.watch) running.watch->request(frameRate);
        refusals_.reset(camera);
        // Durations that were not known are written again, so are the settings the shorter ones may have cut.
        if (running.timingWritten() && running.applied) applyOnQueue(camera, running.settings, true);
        return true;
    }

    bool startVideoOnQueue(std::size_t camera, std::uint32_t width, std::uint32_t height, std::uint32_t frameRate)
    {
        switch (cameras_[camera].pathFor(width, height, frameRate)) {
        case StreamCache::Path::cached: return true;
        case StreamCache::Path::resize: return resizeCamera(camera, width, height, frameRate);
        case StreamCache::Path::admit: return admitCamera(camera, true, false, width, height, frameRate);
        }
        return false;
    }

    // After an interruption or a restart (uml::platform::recoverAnchors): an
    // input whose device is no longer in its anchor is put back, with the sizes
    // of its outputs, which follow the format (a stream's frame and a photo's
    // maximum). Only when every camera is in its anchor is the session said to be
    // back; a repair that is refused (the device would not lock, a setter raised)
    // says it failed, which the client answers by closing the session and opening
    // another, whose cameras start in their anchors. This runs on the session's
    // queue and the announcement is the last thing it does.
    void recoverAfterInterruption()
    {
        if (stopped_) return;
        std::vector<std::size_t> inputs;
        for (const auto& [camera, running] : cameras_)
            if (running.input != nil) inputs.push_back(camera);
        RecoverySteps steps;
        steps.outOfAnchor = [&](std::size_t camera) {
            const Anchor* anchor = anchorOf(camera);
            return anchor != nullptr && entries_[camera].device.activeFormat != anchor->format;
        };
        bool repaired = false;
        steps.repair = [&](std::size_t camera) {
            const bool ok = repairAnchor(camera);
            repaired = repaired || ok;
            return ok;
        };
        steps.announce = [&](Recovery outcome) {
            if (outcome == Recovery::failed) std::fprintf(stderr, "Camera: the cameras could not be put back in their anchors\n");
            // What the capture made before this moment is from before the repair: its clock now, and the boundary
            // drawn under the lock that delivery holds, before the session is said to be back.
            const bool drawBoundary = outcome == Recovery::resumed && repaired;
            const std::optional<std::int64_t> captureClock = drawBoundary ? captureClockNow() : std::nullopt;
            std::lock_guard lock(state_->mutex);
            if (drawBoundary) state_->boundary.repaired(captureClock);
            if (state_->session) deliver({allCameras, outcome == Recovery::resumed ? CameraEventKind::resumed : CameraEventKind::failed});
        };
        // A failed recovery is final for the session: a second notification (the end
        // of the interruption and the restart both arrive) repairs and announces
        // nothing more, whatever state the first left the cameras in.
        (void)recovery_.recover(inputs, steps);
    }

    // The capture's own clock now: the timeline of its samples' timestamps (the session's synchronization clock),
    // which is not assumed to be the host's.
    std::optional<std::int64_t> captureClockNow() const
    {
        CMClockRef clock = session_.synchronizationClock;
        return clock != nullptr ? nanosecondsOf(CMClockGetTime(clock)) : std::nullopt;
    }

    // Puts a camera whose device was moved out of its anchor back: the format
    // and the durations of its stream, the sizes of its outputs, its settings.
    bool repairAnchor(std::size_t camera)
    {
        auto& running = cameras_[camera];
        const Anchor* anchor = anchorOf(camera);
        const Entry& entry = entries_[camera];
        std::fprintf(stderr, "Camera: camera %zu's device was moved out of its anchor: put back\n", camera);
        [session_ beginConfiguration];
        const bool ok = guarded(camera, [&] {
            if (!activate(entry.device, *anchor, running.video ? programmedFor(*anchor, running.frameRate, pressureCapOf(running)) : std::nullopt))
                return false;
            if (running.videoOutput != nil)
                setVideoOutputSettings(running.videoOutput, videoSettingsOf(anchor->facts, running.width, running.height));
            if (running.photoOutput != nil)
                if (const auto maximum = photoMaximum(anchor->facts, limits_.maximumStillPixels))
                    running.photoOutput.maxPhotoDimensions = CMVideoDimensions {std::int32_t(maximum->width), std::int32_t(maximum->height)};
            return true;
        });
        [session_ commitConfiguration];
        photoPipelinesSettle();
        // A stream's durations are written with its anchor; a still-only camera's are written when it streams.
        if (ok && running.video) running.timingWritten();
        if (ok && running.applied) applyOnQueue(camera, running.settings, true);
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

    // The first time a camera joins others in the session, its hardware cost.
    void logJoin(std::size_t camera)
    {
#if TARGET_OS_IOS
        if (!multiCamera_ || peersOf(camera).empty() || !joinsLogged_.insert(camera).second) return;
        std::fprintf(stderr, "Camera: camera %zu joined the multi-camera session: hardware cost %.2f of 1.00\n", camera,
                     double(((AVCaptureMultiCamSession*)session_).hardwareCost));
#else
        (void)camera;
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
        photoPipelinesSettle();
    }

    // The session committed a configuration (an admission, a stream's resize, a repair of an anchor, a removal), and
    // the pipeline of every photo output whose maximum needs the full-resolution mode is reconfigured again: it takes
    // its time from now. The one bookkeeping of every commit (photoMaximumCommittedAt).
    void photoPipelinesSettle()
    {
        const std::uint64_t now = hostNow();
        for (auto& [camera, running] : cameras_) {
            const Anchor* anchor = anchorOf(camera);
            if (running.photoOutput == nil || anchor == nullptr) continue;
            running.photoMaximumCommittedAt =
                uml::platform::photoMaximumCommittedAt(photoMaximum(anchor->facts, limits_.maximumStillPixels), now);
        }
    }

    // Called on the session's queue after a still is delivered: a camera that
    // streams keeps its photo output, one that only takes stills leaves.
    void releaseStill(std::size_t camera)
    {
        if (stopped_ || camera >= entries_.size()) return;
        auto& running = cameras_[camera];
        if (running.video || running.photoOutput == nil || stillsInFlight(camera)) return;
        removeFromSession(running, false, true);
    }

    // The system's pressure on a camera lowers the rate it runs at, as Apple
    // advises (heat that goes on rising ends in the session's interruption);
    // the capture's client sees fewer frames, and the interruption itself
    // arrives as an event of its own. The handler and the work it queues hold
    // the watch, the device and the anchor themselves (copied into the blocks
    // from locals: a block that named a parameter or a member by reference would
    // reach what the session may have freed by the time it runs), and the work
    // does nothing once the watch has ended.
    void beginPressureWatch(const Entry& entry, RunningCamera& running, std::shared_ptr<const Anchor> anchor, std::uint32_t frameRate)
    {
        if (running.watch) return;
        auto watch = std::make_shared<PressureWatch>(anchor->facts, anchor->rates);
        watch->request(frameRate);
        running.watch = watch;
#if TARGET_OS_IOS
        AVCaptureDevice* device = entry.device;
        dispatch_queue_t queue = queue_;
        // The device's system pressure (heat, mostly) as it changes.
        running.pressure = [[UMLKeyPathObserver alloc] initWithObject:device
                                                              keyPath:@"systemPressureState"
                                                              handler:^(id value) {
            AVCaptureSystemPressureState* state = value;
            if (state == nil) return;
            AVCaptureSystemPressureLevel level = state.level;
            std::uint32_t cap = 0;
            if ([level isEqualToString:AVCaptureSystemPressureLevelSerious]) cap = 20;
            else if ([level isEqualToString:AVCaptureSystemPressureLevelCritical] ||
                     [level isEqualToString:AVCaptureSystemPressureLevelShutdown])
                cap = 15;
            if (!watch->pressure(cap)) return;
            // The frame durations alone: the device is in its anchor already.
            dispatch_async(queue, ^{
                if (const auto rate = watch->programmed()) (void)activate(device, *anchor, rate);
            });
        }];
#else
        (void)entry;
#endif
    }

    // Nothing queued by the observer programs the device afterwards.
    static void endPressureWatch(RunningCamera& running)
    {
        if (running.watch) running.watch->end();
        running.watch.reset();
#if TARGET_OS_IOS
        [running.pressure invalidate];
        running.pressure = nil;
#endif
    }

    // A still needs the anchor, the input and a photo output: the camera joins
    // the session as a video camera does.
    bool prepareStill(std::size_t camera)
    {
        if (cameras_[camera].photoOutput != nil) return true;
        return admitCamera(camera, false, true, 0, 0, 0);
    }

    // The captures on their way: their delegates are held here.
    struct Photo {
        std::size_t camera {};
        UMLPhotoDelegate* delegate {};
        // What was asked, for the capture that follows one whose photo is not the size asked for.
        CameraStillRequest request;
        unsigned attempt {1};
        // What the request has waited for the photo output's settle time, over its attempts (stillWaitBudgetNanoseconds).
        std::uint64_t waited {};
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
    // What the anchors were chosen for: the still cap bounds a photo output.
    CameraLimits limits_;
    // Each camera's anchor, by index of cameras(); null for a camera the device
    // does not offer.
    std::vector<std::shared_ptr<const Anchor>> anchors_;
    RefusalLog refusals_;
    std::set<std::size_t> joinsLogged_;
    std::atomic_bool stopped_ {};
    // The recoveries of this session: a failure stays until the session is torn
    // down.
    RecoveryLatch recovery_;
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

    std::uint32_t stillWorkingBytesPerPixel() const noexcept override { return systemCameraStillWorkingBytesPerPixel; }

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
    // The sample's capture time on the capture's clock, and the boundary's generation (read inside deliverSample),
    // are both taken before this callback waits for the lock the recovery's announcement holds.
    const auto capturedAt = uml::platform::nanosecondsOf(CMSampleBufferGetPresentationTimeStamp(sampleBuffer));
    uml::platform::deliverSample(_state->boundary, _state->mutex, capturedAt, [&] {
        if (_state->session) static_cast<uml::platform::AppleSession*>(_state->session)->deliverFrame(_camera, sampleBuffer);
    });
}

@end

namespace {

// What a still that did not end in a picture says about itself (camera_still_failure.hpp). The first failure of a
// capture stands: a later callback of the same capture names the same fault.
void noteStillFailure(uml::platform::CameraStillFailure& failure, uml::platform::CameraStillStage stage)
{
    if (failure.stage == uml::platform::CameraStillStage::none) failure.stage = stage;
}

// The framework's error as a domain word and a code: nothing else of it (no message, no description) is kept.
void noteFrameworkError(uml::platform::CameraStillFailure& failure, NSError* error)
{
    if (error == nil || failure.stage != uml::platform::CameraStillStage::none) return;
    failure.stage = uml::platform::CameraStillStage::frameworkError;
    failure.errorDomain = uml::platform::errorDomainOf(error.domain != nil ? error.domain.UTF8String : "");
    failure.errorCode = std::int32_t(std::clamp<NSInteger>(error.code, INT32_MIN, INT32_MAX));
}

// Whether the photo output supports turning its stills to an earlier model's sensor orientation, and whether it did for
// this capture; unknown where the host has no such output (macOS).
void noteCompensation(uml::platform::CameraStillFailure& failure, AVCapturePhotoOutput* output)
{
#if TARGET_OS_IOS
    if (output == nil) return;
    @try {
        const bool supported = output.cameraSensorOrientationCompensationSupported;
        failure.compensationSupported = supported ? uml::platform::CameraFact::yes : uml::platform::CameraFact::no;
        failure.compensationEnabled = supported && output.cameraSensorOrientationCompensationEnabled
            ? uml::platform::CameraFact::yes : uml::platform::CameraFact::no;
    } @catch (NSException*) {
        // The facts stay unknown.
    }
#else
    (void)failure;
    (void)output;
#endif
}

} // namespace

@implementation UMLPhotoDelegate {
    size_t _camera;
    uint64_t _token;
    BOOL _wantJpeg;
    // The size of the JPEG the guest configured.
    uml::platform::CameraStillSize _jpegSize;
    // The largest file the host takes; zero for no bound.
    uint64_t _maximumBytes;
    BOOL _wantRaw;
    // Which capture of the request this is (from one); a photo of another size makes the next.
    unsigned _attempt;
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
                  maximumBytes:(uint64_t)maximumBytes
                       wantRaw:(BOOL)wantRaw
                       attempt:(unsigned)attempt
                         state:(std::shared_ptr<SessionState>)state
{
    if ((self = [super init])) {
        _attempt = attempt;
        _camera = camera;
        _token = token;
        _wantJpeg = wantJpeg;
        _jpegSize = jpegSize;
        _maximumBytes = maximumBytes;
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
    noteCompensation(_result.failure, output);
    // A part that fails leaves its field empty and the other part alone: a Bayer
    // still that cannot be made does not take the person's JPEG with it.
    if (error != nil || photo == nil) {
        // The framework's error is kept as a domain word and a code for the session record; the capture goes on
        // without this part. A photo that is not there at all is missing data.
        noteFrameworkError(_result.failure, error);
        noteStillFailure(_result.failure, uml::platform::CameraStillStage::missingData);
        return;
    }
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
        // A file larger than the host takes for the size the guest configured (its JPEG buffer) is not
        // copied: the capture fails with those bytes still the system's alone (the host's cost of a
        // capture counts this file).
        const std::span<const std::uint8_t> file(static_cast<const std::uint8_t*>(data.bytes), data.length);
        if (!uml::platform::takeJpegFile(file, _maximumBytes, _wantJpeg, _result.jpeg)) {
            if (_wantJpeg) {
                std::fprintf(stderr, "Camera: the photo's file is %lu bytes, more than the %llu a %ux%u photo may take\n",
                             static_cast<unsigned long>(data.length), static_cast<unsigned long long>(_maximumBytes), _jpegSize.width,
                             _jpegSize.height);
                _result.failure.stage = uml::platform::CameraStillStage::fileSize;
            }
            return;
        }
        _jpegReadings = readings;
    }
}

- (void)captureOutput:(AVCapturePhotoOutput*)output
    didFinishCaptureForResolvedSettings:(AVCaptureResolvedPhotoSettings*)resolvedSettings
                                  error:(NSError*)error
{
    noteCompensation(_result.failure, output);
    // The capture's own error: the photo callback's, when it had one, is the one kept.
    noteFrameworkError(_result.failure, error);
    // The session drops its hold on this object when it delivers, which can be
    // the last one.
    __strong UMLPhotoDelegate* keepAlive = self;
    (void)keepAlive;
    if (_wantJpeg && resolvedSettings != nil) {
        const CMVideoDimensions resolved = resolvedSettings.photoDimensions;
        _result.failure.resolvedWidth = std::uint32_t(std::max(0, int(resolved.width)));
        _result.failure.resolvedHeight = std::uint32_t(std::max(0, int(resolved.height)));
    }
    // The photo is the size the guest configured or it is not passed on: the
    // output makes at most the size it was asked for, and the platform says
    // what it resolved. (The device compares the encoded picture as well.)
    if (_wantJpeg && resolvedSettings != nil && !_result.jpeg.empty()) {
        const CMVideoDimensions resolved = resolvedSettings.photoDimensions;
        const uml::platform::CameraStillSize resolvedSize {std::uint32_t(std::max(0, int(resolved.width))),
                                                           std::uint32_t(std::max(0, int(resolved.height)))};
        const auto outcome = uml::platform::stillSizeOutcome(resolvedSize, _jpegSize, _attempt);
        if (outcome != uml::platform::StillSizeOutcome::deliver) {
            std::fprintf(stderr, "Camera: the photo resolved to %ux%u, not the %ux%u that was asked for (capture %u of %u)\n",
                         resolvedSize.width, resolvedSize.height, _jpegSize.width, _jpegSize.height, _attempt,
                         uml::platform::stillAttemptLimit);
            _result.jpeg.clear();
            _result.failure.stage = uml::platform::CameraStillStage::resolvedSize;
            if (outcome == uml::platform::StillSizeOutcome::retry) {
                // The photo of the first capture is dropped and the request is made again; the result is the
                // next capture's.
                std::lock_guard lock(_state->mutex);
                if (_state->session && static_cast<uml::platform::AppleSession*>(_state->session)->remakeStill(_camera, _token)) return;
            }
        }
    }
    // A JPEG that was asked for and did not arrive, for a reason no callback gave, is missing data.
    if (_wantJpeg && _result.jpeg.empty()) noteStillFailure(_result.failure, uml::platform::CameraStillStage::missingData);
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
