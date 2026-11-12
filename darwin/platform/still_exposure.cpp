// SPDX-License-Identifier: GPL-2.0-or-later
#include "still_exposure.hpp"

#include <cmath>
#include <limits>

namespace uml::platform {
namespace {

// A reading that can be a measurement: finite and positive.
[[nodiscard]] std::optional<double> measured(const std::optional<double>& reading, double largest)
{
    if (!reading || !std::isfinite(*reading) || *reading <= 0.0 || *reading > largest) return std::nullopt;
    return reading;
}

} // namespace

CameraExposure stillExposureOf(const CameraExposure& device, const PhotoReadings& readings, std::optional<bool> flashResolved,
                               const DngExposureData& dng)
{
    CameraExposure exposure = device;
    // The preview's exposure and sensitivity are not the photo's.
    exposure.exposureNanoseconds = 0;
    exposure.iso = 0;
    exposure.focalLengthMillimetres = 0;
    exposure.flashFired = false;
    exposure.neutralPoint = {};
    exposure.noiseProfile = {};

    // An hour is longer than any camera exposes and keeps the product in range.
    if (const auto seconds = measured(readings.exposureSeconds, 3600.0))
        exposure.exposureNanoseconds = std::uint64_t(std::llround(*seconds * 1e9));
    if (const auto iso = measured(readings.iso, 4.0e9)) exposure.iso = std::uint32_t(std::llround(*iso));
    // A lens has one aperture: when the photo names no f-number, the lens's own
    // (the session's lensAperture) is a true reading; a lens that states none
    // has none.
    if (const auto fNumber = measured(readings.fNumber, 1000.0)) exposure.aperture = float(*fNumber);
    else if (!std::isfinite(exposure.aperture) || exposure.aperture <= 0.0f) exposure.aperture = 0.0f;
    // The focal length stays zero when the photo names none; the camera device
    // then states the lens's own, which is fixed too.
    if (const auto focal = measured(readings.focalLengthMillimetres, 10000.0)) exposure.focalLengthMillimetres = float(*focal);

    if (readings.flash) exposure.flashFired = (*readings.flash & 1) != 0;
    else if (flashResolved) exposure.flashFired = *flashResolved;

    if (dng.neutralPoint) exposure.neutralPoint = *dng.neutralPoint;
    if (dng.noiseProfile) exposure.noiseProfile = *dng.noiseProfile;
    return exposure;
}

} // namespace uml::platform
