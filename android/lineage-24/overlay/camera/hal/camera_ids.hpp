// SPDX-License-Identifier: Apache-2.0
//
// The camera IDs Android sees. They are stable across devices so that an
// overlay can name them: "0" is the back zoom camera (or the primary back lens
// of a host that has no zoom camera), "1" the front camera, and each other back
// lens has an ID of its own, "2" the wide, "3" the ultra wide and "4" the
// telephoto. A host without a lens has no ID for it, and a host with no back
// camera numbers its front camera "0", so that the IDs of the cameras that exist
// stay consecutive from zero for Android's legacy interfaces.
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "camera_abi.hpp"

namespace andarium::camera {

// What the assignment needs of a camera.
struct CameraKey {
    std::uint32_t index = 0; // the host's number for it (virtio_video_camera_desc::index)
    std::uint32_t facing = 0;
    std::uint32_t lens = 0;
};

struct IdAssignment {
    std::uint32_t id = 0;
    std::uint32_t index = 0;
};

constexpr std::uint32_t wide_camera_id = 2;
constexpr std::uint32_t ultra_wide_camera_id = 3;
constexpr std::uint32_t telephoto_camera_id = 4;

// The assignment, by ascending ID. External cameras and a second camera of one
// lens are left out.
inline std::vector<IdAssignment> assign_ids(std::span<const CameraKey> cameras)
{
    std::vector<IdAssignment> result;
    const CameraKey* primary = nullptr;
    for (const auto& camera : cameras)
        if (camera.facing == abi::facing::back && camera.lens == abi::lens::zoom) {
            primary = &camera;
            break;
        }
    if (primary == nullptr)
        for (const auto& camera : cameras)
            if (camera.facing == abi::facing::back && camera.lens != abi::lens::external) {
                primary = &camera;
                break;
            }
    if (primary != nullptr) result.push_back({0, primary->index});

    for (const auto& camera : cameras) {
        if (camera.facing != abi::facing::front) continue;
        result.push_back({primary != nullptr ? 1u : 0u, camera.index});
        break;
    }

    for (const auto& camera : cameras) {
        if (camera.facing != abi::facing::back || &camera == primary) continue;
        std::uint32_t id = 0;
        switch (camera.lens) {
        case abi::lens::wide: id = wide_camera_id; break;
        case abi::lens::ultra_wide: id = ultra_wide_camera_id; break;
        case abi::lens::telephoto: id = telephoto_camera_id; break;
        default: break;
        }
        if (id == 0) continue;
        if (std::any_of(result.begin(), result.end(), [id](const IdAssignment& taken) { return taken.id == id; })) continue;
        result.push_back({id, camera.index});
    }
    std::sort(result.begin(), result.end(), [](const IdAssignment& a, const IdAssignment& b) { return a.id < b.id; });
    return result;
}

} // namespace andarium::camera
