// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The colour calibration and optics a DNG file carries, read from its TIFF
// structure. A Bayer still is the only place the host reveals a sensor's colour
// matrices, so the camera learns them from the DNG that capture also produces.

#include "camera.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace uml::platform {

// Reads ColorMatrix1/2, ForwardMatrix1/2, CalibrationIlluminant1/2, BlackLevel
// and WhiteLevel from the first image directory (and the directories it
// links), and the lens focal length from the Exif directory. Returns nothing
// when `dng` is not a TIFF, or lacks a colour matrix; a missing optional tag
// leaves its field zero.
[[nodiscard]] std::optional<CameraColorData> parseDngColorData(std::span<const std::uint8_t> dng);

} // namespace uml::platform
