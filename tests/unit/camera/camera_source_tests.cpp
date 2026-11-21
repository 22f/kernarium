// SPDX-License-Identifier: GPL-2.0-or-later
#include "camera_source.hpp"
#include "require.hpp"


using namespace uml::camera;
using uml::testing::require;

/* The launcher's way to the platform cameras. Nothing here opens a session,
 * asks for access or captures: the host's camera is only discovered, and the
 * permission is only read. */
int main()
{
    const auto phone = makeSyntheticCameraSource(SyntheticProfile::phone);
    const auto tablet = makeSyntheticCameraSource(SyntheticProfile::tablet);
    const auto laptop = makeSyntheticCameraSource(SyntheticProfile::laptop);
    require(phone && tablet && laptop);
    // The profile picks the cameras: a laptop has its front camera alone.
    require(laptop->cameras().size() == 1);
    require(phone->cameras().size() > 1 && tablet->cameras().size() > 1);
    require(phone->cameras().size() != laptop->cameras().size());

    // The permission is a read: asking twice gives one answer, and no prompt.
    const bool granted = hostCameraPermissionGranted();
    require(granted == hostCameraPermissionGranted());
    const auto host = makeHostCameraSource();
    require(host != nullptr && !host->hostModel().empty());
    return 0;
}
