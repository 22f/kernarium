// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The platform's cameras. The Apple backend implements it over AVFoundation and
// a synthetic backend implements it with no hardware (synthetic_camera/);
// nothing here names AVFoundation, V4L2 or the Virtio wire, and the Virtio
// camera device maps these types to and from the wire.
//
// Discovery needs no authorization and never prompts. A capture session does:
// openSession() fails when the system has not granted access, and nothing in
// this interface asks for it (camera_authorization.hpp does, at a user's act).

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace uml::platform {

enum class CameraPosition : std::uint8_t { back, front, external };

// zoom is a virtual device that switches between lenses by zoom ratio and
// accepts automatic exposure, focus and white balance only; every other kind is
// one sensor behind one lens, and lets a guest set them by hand where the
// device can.
enum class CameraLensKind : std::uint8_t { zoom, wide, ultraWide, telephoto, external };

enum class BayerOrder : std::uint8_t { none, rggb, grbg, gbrg, bggr };

// A capture format: the size of a frame and the frame rates it runs at, in
// frames per second.
struct CameraVideoFormat {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t minimumFrameRate {};
    std::uint32_t maximumFrameRate {};
    // The format may join a session that runs several cameras at once.
    bool multiCamera {};
    friend bool operator==(const CameraVideoFormat&, const CameraVideoFormat&) = default;
};

struct CameraStillSize {
    std::uint32_t width {};
    std::uint32_t height {};
    friend bool operator==(const CameraStillSize&, const CameraStillSize&) = default;
};

// What a Bayer still teaches about a sensor. The matrices are DNG's, row-major.
struct CameraColorData {
    BayerOrder bayer {BayerOrder::none};
    std::uint32_t blackLevel {};
    std::uint32_t whiteLevel {};
    std::uint32_t rawWidth {};
    std::uint32_t rawHeight {};
    // DNG LightSource codes of the two calibrations.
    std::uint32_t illuminant1 {};
    std::uint32_t illuminant2 {};
    std::array<double, 9> colorMatrix1 {};
    std::array<double, 9> colorMatrix2 {};
    std::array<double, 9> forwardMatrix1 {};
    std::array<double, 9> forwardMatrix2 {};
    // Optics the same capture reveals; zero when it did not.
    double focalLengthMillimetres {};
    std::array<double, 4> intrinsics {};  // fx, fy, cx, cy in pixels of the pixel array
    friend bool operator==(const CameraColorData&, const CameraColorData&) = default;
};

// One camera of the host, as discovery reports it. Ranges are the camera's
// own; a camera that cannot do a thing leaves its flag false.
struct CameraInfo {
    // The host's stable name for the camera. It identifies the camera across
    // starts and is never shown to a guest.
    std::string identifier;
    CameraPosition position {CameraPosition::back};
    CameraLensKind lens {CameraLensKind::wide};

    bool autofocus {};
    bool flash {};
    bool torch {};
    // The host lets a client set these by hand on this camera. A zoom camera
    // sets none of them.
    bool manualExposure {};
    bool manualFocus {};
    bool manualWhiteBalance {};
    // The host can take a Bayer still with this camera.
    bool rawStills {};

    // The zoom ratio as Android states it: 0.5 is the ultra wide lens of a zoom
    // camera, 1.0 its main lens. switchOvers are the ratios where it changes lens.
    float zoomMinimum {1.0f};
    float zoomMaximum {1.0f};
    std::vector<float> switchOvers;
    // A zoom camera: the identifiers of the lenses it switches between.
    std::vector<std::string> constituents;

    // The full field of view, in pixels, and its horizontal angle in degrees.
    std::uint32_t pixelArrayWidth {};
    std::uint32_t pixelArrayHeight {};
    float horizontalFieldOfView {};
    float aperture {};                // f-number; zero when unknown
    float minimumFocusDistanceMillimetres {};  // zero for a fixed focus
    std::uint32_t isoMinimum {};
    std::uint32_t isoMaximum {};
    std::uint64_t exposureMinimumNanoseconds {};
    std::uint64_t exposureMaximumNanoseconds {};
    float maximumWhiteBalanceGain {};
    // Clockwise degrees (0, 90, 180, 270) from the sensor's top to the upright
    // side of a frame in the portrait orientation of the host's display.
    std::uint32_t sensorOrientation {};

    std::vector<CameraVideoFormat> videoFormats;
    std::vector<CameraStillSize> stillSizes;  // JPEG
    CameraStillSize rawStillSize {};          // zero: no Bayer still
};

// 3A states as a platform reports them. The device maps them to a guest's
// numbering.
enum class CameraControlState : std::uint8_t { inactive, searching, converged, locked, failed };

// What one exposure actually did.
struct CameraExposure {
    std::uint64_t exposureNanoseconds {};
    std::uint64_t frameDurationNanoseconds {};
    std::uint32_t iso {};
    float lensPositionDioptres {};
    std::array<float, 3> whiteBalanceGains {1.0f, 1.0f, 1.0f};  // red, green, blue
    std::uint32_t colorTemperature {};
    float tint {};
    CameraControlState ae {CameraControlState::inactive};
    CameraControlState af {CameraControlState::inactive};
    CameraControlState awb {CameraControlState::inactive};
    float zoomRatio {1.0f};
    // The lens that made the exposure: an index of cameras().
    std::uint32_t activeCamera {};
    float aperture {};
    float focalLengthMillimetres {};
    bool flashFired {};
    // Stills only.
    std::array<float, 3> neutralPoint {};
    std::array<float, 8> noiseProfile {};  // (S, O) per colour channel; zero when unknown
};

// An NV12 frame, BT.709 full range. The planes stay valid while `owner` is held,
// and holding it for long starves the capture.
struct CameraFrame {
    std::uint32_t width {};
    std::uint32_t height {};
    std::span<const std::uint8_t> luma;
    std::size_t lumaStride {};
    std::span<const std::uint8_t> chroma;  // interleaved Cb, Cr at half the width and height
    std::size_t chromaStride {};
    std::shared_ptr<const void> owner;
    // When the exposure began, in nanoseconds of the host's monotonic clock
    // (CameraSession::now()).
    std::uint64_t timestampNanoseconds {};
    CameraExposure exposure;
};

struct CameraStillResult {
    std::uint64_t token {};
    bool ok {};
    // A complete JFIF file when a JPEG was asked for.
    std::vector<std::uint8_t> jpeg;
    // A Bayer still when one was asked for: 16-bit samples, right-aligned and
    // tightly packed (rawWidth * 2 bytes a row).
    std::vector<std::uint16_t> raw;
    CameraColorData color;  // bayer is none when the capture taught nothing
    CameraExposure exposure;
};

enum class CameraExposureMode : std::uint8_t { continuousAuto, locked, manual };
enum class CameraFocusMode : std::uint8_t { continuousAuto, locked, manual };
enum class CameraWhiteBalanceMode : std::uint8_t { continuousAuto, locked, manual };
enum class CameraFlashMode : std::uint8_t { off, automatic, on };

// A metering or focus region, as fractions of the frame; weight 0 clears it.
struct CameraRegion {
    float x {};
    float y {};
    float width {};
    float height {};
    std::uint32_t weight {};
    friend bool operator==(const CameraRegion&, const CameraRegion&) = default;
};

// Everything a client may set on one camera. A session applies the whole value:
// it compares with what it applied last and changes what differs.
struct CameraSettings {
    CameraExposureMode exposureMode {CameraExposureMode::continuousAuto};
    std::uint64_t exposureNanoseconds {};
    std::uint32_t iso {};
    float exposureBiasStops {};
    CameraFocusMode focusMode {CameraFocusMode::continuousAuto};
    float focusDistanceDioptres {};
    // A focus mode of continuousAuto runs a single scan whenever this changes.
    std::uint32_t focusTriggerSerial {};
    CameraWhiteBalanceMode whiteBalanceMode {CameraWhiteBalanceMode::continuousAuto};
    std::array<float, 3> whiteBalanceGains {1.0f, 1.0f, 1.0f};
    float zoomRatio {1.0f};
    bool torch {};
    std::optional<CameraRegion> aeRegion;
    std::optional<CameraRegion> afRegion;
    bool stabilization {true};
    friend bool operator==(const CameraSettings&, const CameraSettings&) = default;
};

struct CameraStillRequest {
    std::uint64_t token {};
    // Zero: no JPEG. The host takes the closest size it lists.
    std::uint32_t jpegWidth {};
    std::uint32_t jpegHeight {};
    std::uint32_t jpegQuality {95};
    bool raw {};
    // The capture must not use multi-frame processing (a Bayer still needs it).
    bool speedPrioritized {};
    CameraFlashMode flash {CameraFlashMode::off};
};

inline constexpr std::size_t allCameras = std::numeric_limits<std::size_t>::max();

enum class CameraEventKind : std::uint8_t {
    // The capture cannot run for a reason that ends by itself: another
    // application, a call, system pressure.
    interrupted,
    // The same, because other applications share the screen (iPad
    // multitasking). A host that cannot tell reports interrupted.
    interruptedByMultitasking,
    // The capture runs again.
    resumed,
    // The camera went away or came back (an external camera).
    unavailable,
    available,
    // The session failed and will not recover.
    failed,
};

struct CameraEvent {
    // An index of cameras(), or allCameras.
    std::size_t camera {allCameras};
    CameraEventKind kind {CameraEventKind::interrupted};
};

// Receives what a session produces. Every method runs on a thread of the
// platform and must return quickly; a frame is taken by holding its owner. A
// delegate must outlive the session it was given to.
class CameraSessionDelegate {
public:
    virtual ~CameraSessionDelegate() = default;
    virtual void cameraFrame(std::size_t camera, CameraFrame frame) = 0;
    virtual void cameraStill(std::size_t camera, CameraStillResult result) = 0;
    virtual void cameraEvent(CameraEvent event) = 0;
};

// One capture session over the cameras of a host. The Apple backend runs a
// single AVCaptureMultiCamSession whatever the number of cameras.
class CameraSession {
public:
    virtual ~CameraSession() = default;

    // Starts a camera, or restarts it at another size or rate, with the
    // smallest format of the camera that covers the size at the rate. Fails
    // when the camera cannot join the cameras already running.
    [[nodiscard]] virtual bool startVideo(std::size_t camera, std::uint32_t width, std::uint32_t height,
                                          std::uint32_t frameRate) = 0;
    virtual void stopVideo(std::size_t camera) = 0;
    virtual void apply(std::size_t camera, const CameraSettings& settings) = 0;
    // Takes one still; the result arrives through the delegate. A still needs no
    // running video. Fails when the camera cannot take what the request asks.
    [[nodiscard]] virtual bool takeStill(std::size_t camera, const CameraStillRequest& request) = 0;
    // Stops every camera. The session cannot be used afterwards.
    virtual void stop() = 0;
};

class Camera {
public:
    virtual ~Camera() = default;

    // The host's cameras. Their order is stable for the life of the object and
    // every index in this interface is an index into it.
    [[nodiscard]] virtual std::vector<CameraInfo> cameras() const = 0;
    // Sets of cameras the host can run at once, as indices of cameras().
    [[nodiscard]] virtual std::vector<std::vector<std::size_t>> concurrentSets() const = 0;
    // Names the host model; calibration is kept per model and lens.
    [[nodiscard]] virtual std::string hostModel() const = 0;
    // Nanoseconds of the monotonic clock that frame timestamps use.
    [[nodiscard]] virtual std::uint64_t now() const noexcept = 0;
    // Opens a capture session, or returns null when the system has not granted
    // access or the host has no camera. This is the only call that can light a
    // capture indicator, and a blocked guest session never makes it.
    [[nodiscard]] virtual std::unique_ptr<CameraSession> openSession(CameraSessionDelegate& delegate) = 0;
};

// The system's cameras through AVFoundation.
[[nodiscard]] std::unique_ptr<Camera> makeAppleCamera();

} // namespace uml::platform
