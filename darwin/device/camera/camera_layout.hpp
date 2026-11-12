// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "camera_description.hpp"
#include "camera_tiers.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uml::camera {

/* The camera layouts of the Android image. One image serves every host: the
 * host discovers its cameras and passes the layout that describes them as
 * androidboot.product.vendor.sku, a closed set of 39 names whose permission
 * directories and media profiles the image carries
 * (android/lineage-24/README.md, "Camera"). android/lineage-24/tools/camera_layouts.py
 * is the only source of the set; this file composes a name from a host's
 * cameras the way that file says, and a test holds the two together
 * (camera-layout reads overlay/camera/generated/layouts.tsv).
 *
 * The dimensions are the host's cameras (none, the front camera alone, or back
 * and front), and for back and front the memory tier, the back flash, manual
 * sensor control, RAW (only with manual control), the concurrent streaming of
 * the front and the back camera, and for the large tier the front camera's
 * video at 2160p. A front camera alone has manual control or not. */
enum class LayoutCameras : std::uint8_t { none, front, backAndFront };

struct LayoutDimensions {
    LayoutCameras cameras {LayoutCameras::none};
    /* Only back and front have a tier. */
    CameraTier tier {CameraTier::small};
    bool flash {};
    bool manual {};
    bool raw {};
    bool concurrent {};
    /* Only the large tier has it. */
    bool front2160 {};

    friend bool operator==(const LayoutDimensions&, const LayoutDimensions&) = default;
};

/* The dimensions with what the set cannot say taken away: no tier, flash,
 * RAW, concurrency or 2160p without back cameras; no RAW without manual
 * control; no 2160p in the small tier. */
[[nodiscard]] LayoutDimensions normalizedLayout(LayoutDimensions dimensions) noexcept;

/* The name of the layout of `dimensions` (normalized first). */
[[nodiscard]] std::string layoutName(const LayoutDimensions& dimensions);

/* Every layout, in the order of the image's table. */
[[nodiscard]] std::vector<LayoutDimensions> allLayouts();
[[nodiscard]] std::vector<std::string> allLayoutNames();

/* The layout and then, nearest first, each layout it falls back to when an
 * image lacks it: dropping concurrent streaming, then RAW, then the front
 * camera's 2160p, then the flash, then the large tier, then manual control,
 * then the back cameras ("front"), then every camera ("none"). */
[[nodiscard]] std::vector<std::string> layoutFallbackChain(const LayoutDimensions& dimensions);

/* The first layout of the chain of `wanted` that `available` accepts; "none"
 * when it accepts none. */
[[nodiscard]] std::string chooseLayout(
    const LayoutDimensions& wanted, const std::function<bool(std::string_view)>& available);

/* Whether the camera meets the sizes the camera API guarantees a camera that
 * streams beside another, as the Android camera HAL derives them from the
 * camera's video modes and stills (camera_plan.hpp derive_sizes and
 * meets_concurrent_streams): a 1280x720 size when its largest size reaches that,
 * and 1920x1440 with it when its largest reaches 1920x1440, for the YUV list and
 * for the JPEG list. A size is cut from a video mode that covers it. */
[[nodiscard]] bool meetsConcurrentStreams(const CameraDescription& camera) noexcept;

/* The dimensions a host's cameras (in wire order, `arrangeCameras`) earn at a
 * tier. A host with a back camera and no front one, or with no camera that is
 * front or back, has none: the set has no layout that says so, and a front
 * camera that is not there would be declared falsely. */
[[nodiscard]] LayoutDimensions layoutDimensionsOf(std::span<const CameraDescription> cameras, CameraTier tier) noexcept;

} // namespace uml::camera
