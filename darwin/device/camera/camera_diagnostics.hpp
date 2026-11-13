// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "camera_device.hpp"

#include <mach/mach.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace uml::camera {

/* Diagnostic readings for a development run (the launcher and a runtime that
 * is not an App Store build print them); an App Store build uses none. */

[[nodiscard]] constexpr const char* checkpointName(CameraCheckpoint where) noexcept
{
    switch (where) {
    case CameraCheckpoint::sessionOpened:
        return "capture session opened";
    case CameraCheckpoint::stillDelivered:
        return "still delivered";
    case CameraCheckpoint::sessionClosed:
        return "capture session closed";
    }
    return "unknown";
}

/* The process's physical footprint in bytes, the figure the system holds
 * against the process's memory limit (task_vm_info's phys_footprint); zero when
 * it cannot be read. */
[[nodiscard]] inline std::uint64_t physicalFootprintBytes() noexcept
{
    task_vm_info_data_t info {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return info.phys_footprint;
}

/* The two hooks of a development run, printed to stderr: what ROTATE_AND_CROP
 * values reach the host and the footprint at each checkpoint. A launcher or a
 * runtime that is not an App Store build assigns them to the device's
 * options. */
inline void printRotateAndCrop(std::uint32_t camera, std::uint32_t setting, std::uint32_t degrees) noexcept
{
    std::fprintf(stderr, "Camera: camera %u: ROTATE_AND_CROP %u, picture turned %u degrees\n", camera, setting, degrees);
}

namespace detail {

/* The highest footprint seen since the last checkpoint. A thread that starts
 * with the first checkpoint samples the footprint every 50 ms, so the peak a
 * capture makes between two checkpoints is read, not only the figure at one,
 * and prints a line (with the wall clock, in seconds) whenever the footprint
 * has moved 8 MiB from the last one printed. */
inline std::atomic<std::uint64_t>& footprintPeak() noexcept
{
    static std::atomic<std::uint64_t> peak {0};
    return peak;
}

inline void startFootprintSampler() noexcept
{
    static const bool started = [] {
        std::thread([] {
            std::uint64_t printed = 0;
            for (;;) {
                const auto now = physicalFootprintBytes();
                if ((now > printed ? now - printed : printed - now) >= (8ull << 20)) {
                    printed = now;
                    const auto wall = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
                    std::fprintf(stderr, "Camera: memory: sample t=%.3f, footprint %llu MiB\n", wall, static_cast<unsigned long long>(now >> 20));
                }
                auto& peak = footprintPeak();
                auto seen = peak.load(std::memory_order_relaxed);
                while (now > seen && !peak.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }).detach();
        return true;
    }();
    (void)started;
}

} // namespace detail

inline void printCheckpoint(CameraCheckpoint where) noexcept
{
    detail::startFootprintSampler();
    const auto now = physicalFootprintBytes();
    const auto peak = std::max(now, detail::footprintPeak().exchange(now, std::memory_order_relaxed));
    std::fprintf(stderr, "Camera: memory: %s, footprint %llu MiB, peak %llu MiB since the last checkpoint\n", checkpointName(where),
                 static_cast<unsigned long long>(now >> 20), static_cast<unsigned long long>(peak >> 20));
}

} // namespace uml::camera
