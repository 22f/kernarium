// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Which photo size a camera is asked for, and whether the photo that came is
// the one asked for. A guest configures a JPEG stream at one size and gets
// that size or an error: a platform picture of another size is not passed on.
// The platform lists its photo sizes per device format, and the format a
// camera runs is not the one that was listed first, so a size another format
// lists is not one the active format can make.

#include "camera.hpp"

#include <optional>
#include <span>

namespace uml::platform {

// The size to ask the photo output for when a JPEG of `request` is wanted: the
// request itself, when the active format lists it and the output's maximum
// covers it. Nothing otherwise; the nearest larger size is not the request.
[[nodiscard]] std::optional<CameraStillSize> exactPhotoSize(std::span<const CameraStillSize> listed,
                                                            CameraStillSize request, CameraStillSize outputMaximum);

// For a capture that names no JPEG size (a Bayer still alone): the smallest
// listed size that covers `minimum` within the output's maximum.
[[nodiscard]] std::optional<CameraStillSize> coveringPhotoSize(std::span<const CameraStillSize> listed,
                                                               CameraStillSize minimum, CameraStillSize outputMaximum);

// Whether the size the platform resolved for a photo is the size asked for. A
// resolved size of zero by zero is the platform stating none (a Bayer still
// alone has no processed photo), which is not a different size.
[[nodiscard]] bool resolvedSizeIsRequested(CameraStillSize resolved, CameraStillSize requested) noexcept;

} // namespace uml::platform
