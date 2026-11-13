// SPDX-License-Identifier: GPL-2.0-or-later
#include "camera.hpp"
#include "camera_authorization.hpp"
#include "frame_bytes.hpp"
#include "require.hpp"

#include <set>
#include <string>

using namespace uml::platform;
using uml::testing::require;

/* Discovery only. The test reads what AVFoundation lists and never opens a
 * session, never asks for access and never captures, so it runs on a host
 * with any cameras, or with none, and cannot light a capture indicator. */
int main()
{
    const auto permissionBefore = makeAppleCameraAuthorization()->permission();
    const auto camera = makeAppleCamera();
    require(camera != nullptr);
    const auto cameras = camera->cameras();
    require(!camera->hostModel().empty());
    // The system's camera states the working storage its photo pipeline holds, which the reserve counts.
    require(camera->stillWorkingBytesPerPixel() == systemCameraStillWorkingBytesPerPixel, "the system camera's working storage");
    std::set<std::string> identifiers;
    for (const auto& info : cameras) {
        require(!info.identifier.empty() && identifiers.insert(info.identifier).second);
        require(!info.videoFormats.empty());
        for (const auto& format : info.videoFormats) {
            require(format.width > 0 && format.height > 0);
            require(format.minimumFrameRate >= 1 && format.maximumFrameRate >= format.minimumFrameRate);
        }
        require(info.zoomMinimum > 0 && info.zoomMaximum >= info.zoomMinimum);
        require(info.pixelArrayWidth > 0 && info.pixelArrayHeight > 0);
        require(info.sensorOrientation % 90 == 0 && info.sensorOrientation < 360);
        // A zoom camera takes no manual control, and names lenses the host lists.
        if (info.lens == CameraLensKind::zoom) {
            require(!info.manualExposure && !info.manualFocus && !info.manualWhiteBalance && !info.rawStills);
            require(!info.constituents.empty());
        }
        // A manual control needs its range.
        if (info.manualExposure) require(info.isoMaximum >= info.isoMinimum && info.exposureMaximumNanoseconds > 0);
        if (info.rawStills) require(info.rawStillSize.width > 0);
    }
    // The order is stable, and so are the sets, which index into it.
    const auto again = camera->cameras();
    require(again.size() == cameras.size());
    for (std::size_t index = 0; index < cameras.size(); ++index) require(again[index].identifier == cameras[index].identifier);
    for (const auto& set : camera->concurrentSets()) {
        require(set.size() > 1);
        for (const auto member : set) require(member < cameras.size());
    }
    // Discovery changed nothing about the permission.
    require(makeAppleCameraAuthorization()->permission() == permissionBefore);
    return 0;
}
