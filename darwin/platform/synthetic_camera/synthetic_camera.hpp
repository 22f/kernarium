// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// A camera with no hardware. It implements darwin/platform/camera.hpp for unit
// tests and for the launchers that run a guest against a deterministic chart.
// It is never a real camera, on any device, and an App Store runtime does not
// contain it.
//
// The chart. Every frame is an NV12 picture of a luma gradient with a 6 x 4
// grid of colour patches and, in the top-left corner, a row of 64 marker cells
// (each `markerCell(width)` samples wide and 8 high) that carry, most
// significant bit first, a 16-bit tag naming the camera, a 32-bit frame counter
// and a 16-bit check word (the exclusive or of the three 16-bit groups before
// it). A cell is luma 235 for a one and 16 for a zero. decodeChartMarker() reads
// them back from a picture of the camera's own size.

#include "camera.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace uml::platform {

enum class SyntheticCameraProfile : std::uint8_t {
    // A phone: a back zoom camera over wide, ultra wide and telephoto lenses,
    // and a front camera. The wide lens has 48 MP stills and Bayer stills.
    phone,
    // A tablet: a back wide camera and a front camera.
    tablet,
    // An iPad's: a back wide camera and two front cameras (wide and ultra wide,
    // one sensor behind both), each of which can run with the back camera. The
    // back camera also has a size a multi-camera session does not take.
    tabletDualFront,
    // A laptop: a front camera and nothing else.
    laptop,
};

struct SyntheticCameraOptions {
    SyntheticCameraProfile profile {SyntheticCameraProfile::tablet};
    // Frames are produced by SyntheticCamera::pump() instead of by a timer
    // thread, and the clock is the one the test sets.
    bool manualClock {};
    // Added between the start of an exposure and its delivery.
    std::uint32_t latencyMilliseconds {};
    // The system has granted access. A session of a host that has not is null.
    bool authorized {true};
};

class SyntheticCamera : public Camera {
public:
    // Sessions opened since the object was made, and sessions open now. A
    // blocked guest never opens one.
    [[nodiscard]] virtual unsigned sessionsOpened() const noexcept = 0;
    [[nodiscard]] virtual unsigned sessionsOpen() const noexcept = 0;
    // The settings last applied to a camera, and the cameras running video.
    [[nodiscard]] virtual CameraSettings appliedSettings(std::size_t camera) const = 0;
    [[nodiscard]] virtual bool videoRunning(std::size_t camera) const = 0;
    [[nodiscard]] virtual std::uint32_t videoFrameRate(std::size_t camera) const = 0;
    // Produces `frames` frames for a camera, as its timer would have (manual
    // clock only). The clock advances by one frame interval for each.
    virtual void pump(std::size_t camera, unsigned frames = 1) = 0;
    virtual void advanceClock(std::uint64_t nanoseconds) = 0;
    // The size of the format a camera runs video at; 0 x 0 when it does not.
    [[nodiscard]] virtual std::pair<std::uint32_t, std::uint32_t> videoSize(std::size_t camera) const = 0;
    // Interrupts every session, as the host does; no frames flow until resume().
    virtual void interrupt(CameraEventKind kind = CameraEventKind::interrupted) = 0;
    virtual void resume() = 0;
    // Delivers an event to every session, as the host does, and changes
    // nothing else (a failed session or an unavailable camera, for instance).
    virtual void post(CameraEvent event) = 0;
    // A Bayer still is refused when it is asked for; or it is taken and fails
    // alone, leaving the JPEG of the same capture.
    virtual void refuseRaw(bool refuse) = 0;
    virtual void failRaw(bool fail) = 0;
};

[[nodiscard]] std::unique_ptr<SyntheticCamera> makeSyntheticCamera(const SyntheticCameraOptions& options = {});

// The chart a frame of the given size carries.
[[nodiscard]] std::uint32_t markerCell(std::uint32_t width) noexcept;
struct ChartMarker {
    std::uint16_t tag {};
    std::uint32_t counter {};
    bool valid {};
};
[[nodiscard]] ChartMarker decodeChartMarker(const std::uint8_t* luma, std::size_t stride, std::uint32_t width,
                                            std::uint32_t height) noexcept;
// The tag the chart of a camera of the given index carries.
[[nodiscard]] std::uint16_t chartTag(std::size_t camera) noexcept;

// Encodes an NV12 picture as a JPEG (a complete JFIF file with an Exif segment).
[[nodiscard]] std::vector<std::uint8_t> encodeSyntheticJpeg(const std::uint8_t* luma, std::size_t lumaStride,
                                                            const std::uint8_t* chroma, std::size_t chromaStride,
                                                            std::uint32_t width, std::uint32_t height, unsigned quality);

} // namespace uml::platform
