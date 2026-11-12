// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "camera_device.hpp"

#include <cstdint>
#include <cstdio>

namespace uml::camera {

/* Diagnostic readings for a development run (the launcher and a runtime that
 * is not an App Store build print them); an App Store build uses none. */

/* The line of a development run, printed to stderr: what ROTATE_AND_CROP
 * values reach the host. A launcher or a runtime that is not an App Store
 * build assigns it to the device's options. */
inline void printRotateAndCrop(std::uint32_t camera, std::uint32_t setting, std::uint32_t degrees) noexcept
{
    std::fprintf(stderr, "Camera: camera %u: ROTATE_AND_CROP %u, picture turned %u degrees\n", camera, setting, degrees);
}

} // namespace uml::camera
