// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Which video stabilization the camera session asks the host for when the guest
// wants the picture stabilized (CameraSettings::stabilization). A pure function:
// the Apple backend (apple/camera_capture.mm) turns the answer into the
// capture connection's preferred mode, and the tests call the same code.
//
// The guest's preview and its recording share one capture output, so the
// stabilization's latency is the viewfinder's. The choice is therefore never the
// system's automatic one, which on an iPhone picks a cinematic mode that holds
// a second or more of frames before the first one is delivered. It is the
// low-latency mode where the format supports it, else the standard one (which
// adds a little latency), else none.

namespace uml::platform {

enum class VideoStabilization {
    off,
    standard,
    lowLatency,
};

// What the active capture format of a camera supports.
struct VideoStabilizationSupport {
    bool standard {};
    bool lowLatency {};
};

[[nodiscard]] VideoStabilization chooseVideoStabilization(bool wanted, VideoStabilizationSupport support) noexcept;

} // namespace uml::platform
