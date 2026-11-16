// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "KernariumRestartLoop.hpp"
#include "camera/background_video.hpp"

#include <chrono>
#include <mutex>
#include <type_traits>
#include <utility>

namespace kernarium::gpu_hold {

/*
 * What the application's hold of the guest's GPU requests does
 * (kernarium_runtime_set_gpu_hold), apart from the globals it works on: the
 * GPU service that a guest registers, the mutex that guards it, and the run
 * loop's Lifecycle belong to the runtime, which hands them in. The unit tests
 * hand in their own, so the sequence runs as the runtime runs it.
 */

/* The values of `held`. */
inline constexpr int released = 0;
inline constexpr int withCamera = 1;
inline constexpr int atOnce = 2;

struct Timing final {
    /* The longest the gate waits for a GPU request that is running. */
    std::chrono::milliseconds drain {1000};
    /* The longest a hold waits for the guest to stop its camera video. */
    std::chrono::nanoseconds camera {uml::camera::backgroundVideoBound};
};

/* The hold on the guest's GPU service. `Gpu` is the in-process GPU service
 * (`holdRequests`, `releaseRequests`); `mutex` guards `gpu`, which is null when
 * no guest runs. */
template <class Gpu>
class Gate final {
public:
    Gate(std::mutex& mutex, Gpu*& gpu, restart::Lifecycle& lifecycle,
         Timing timing)
        : mutex_(mutex)
        , gpu_(gpu)
        , lifecycle_(lifecycle)
        , timing_(timing)
    {
    }

    /* Holds the guest's GPU requests: 1 when it is held, 0 when a request that
     * was running outlasted the drain, -1 when no guest runs (the hold then
     * waits for the next one). */
    int close()
    {
        std::lock_guard lock(mutex_);
        lifecycle_.setGpuHoldRequested(true);
        if (gpu_ == nullptr)
            return -1;
        return gpu_->holdRequests(timing_.drain) ? 1 : 0;
    }

    int release()
    {
        std::lock_guard lock(mutex_);
        lifecycle_.setGpuHoldRequested(false);
        if (gpu_ != nullptr)
            gpu_->releaseRequests();
        /* A restarted guest waits for this. */
        lifecycle_.changed();
        return gpu_ != nullptr ? 1 : -1;
    }

    /* `video` is the camera's way to end the guest's video: a
     * `uml::camera::BackgroundVideo*` (null for none), or a null pointer
     * constant for a guest with no camera. `log` takes what the camera's wait
     * has to say. */
    template <class Video, class Log>
    int set(int held, Video video, Log&& log)
    {
        static_cast<void>(log);
        if (held == released)
            return release();
        if (held == atOnce) {
            /* The bound's hold, when the edge is already late: no wait for the
             * camera, and a hold that waits for it meanwhile is of no use once
             * this has landed, and ends. */
            const int result = close();
            if constexpr (!std::is_null_pointer_v<Video>) {
                if (video != nullptr)
                    video->abandon();
            }
            return result;
        }
        /* The application has asked: the next boot waits from here on, though
         * the gate closes only after the camera's wait. */
        lifecycle_.setGpuHoldRequested(true);
        if constexpr (std::is_null_pointer_v<Video>) {
            return close();
        } else {
            if (video == nullptr)
                return close();
            return uml::camera::holdAfterVideoEnds(
                       *video, [this] { return close(); }, timing_.camera,
                       std::forward<Log>(log))
                .held;
        }
    }

private:
    std::mutex& mutex_;
    Gpu*& gpu_;
    restart::Lifecycle& lifecycle_;
    const Timing timing_;
};

} // namespace kernarium::gpu_hold
