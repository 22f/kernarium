// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// What a still's exposure was, as the platform's photo states it. A photo's own
// metadata knows the exposure time, the ISO sensitivity, the f-number, the
// focal length and whether the flash fired; the device's state at the moment
// the photo is delivered knows the rest (3A, white balance, zoom, which lens).
// A reading the platform did not give, or gave as a number that cannot be a
// measurement, is left out of the result as zero, which the camera device and
// the guest read as absent, never as a value: no exposure takes zero seconds
// and no sensor has zero sensitivity.

#include "camera.hpp"
#include "dng_color.hpp"

#include <cstdint>
#include <optional>

namespace uml::platform {

// The numbers of a photo's metadata, as it states them. A key the metadata
// does not have is absent; a key it has is passed on as the number it is, zero
// and not-a-number included, for `stillExposureOf` to judge.
struct PhotoReadings {
    std::optional<double> exposureSeconds;
    std::optional<double> iso;
    std::optional<double> fNumber;
    std::optional<double> focalLengthMillimetres;
    // The Exif Flash field: bit 0 says the flash fired.
    std::optional<std::int64_t> flash;
};

// The exposure of a still. `device` is the device's state when the photo is
// delivered: its 3A states, white balance, zoom, lens position and the lens
// itself are the still's, and so is its lens aperture where the photo gives no
// f-number; its exposure time and ISO are the live preview's and are not used.
// `flashResolved` is the platform's decision for the capture (the resolved
// photo settings), which decides when the metadata does not say. `dng` is what
// a Bayer still's DNG states of the exposure's neutral point and noise; absent
// for a JPEG.
[[nodiscard]] CameraExposure stillExposureOf(const CameraExposure& device,
                                             const PhotoReadings& readings,
                                             std::optional<bool> flashResolved,
                                             const DngExposureData& dng);

} // namespace uml::platform
