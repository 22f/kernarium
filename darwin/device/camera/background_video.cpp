// SPDX-License-Identifier: GPL-2.0-or-later
#include "background_video.hpp"

#include <utility>

namespace uml::camera {

BackgroundVideo::Outcome BackgroundVideo::end(Clock::time_point deadline)
{
    std::lock_guard serial(callMutex_);
    std::unique_lock lock(mutex_);
    if (!attached_) return {};
    const auto request = ++asked_;
    requested_.store(true, std::memory_order_release);
    wake_();
    changed_.wait_until(lock, deadline, [&] { return answered_ >= request || !attached_; });
    if (!attached_) return {};
    if (answered_ < request) return {0, false, false};
    const auto streams = streams_;
    changed_.wait_until(lock, deadline, [&] { return remaining_ == 0 || !attached_; });
    return {streams, remaining_ == 0 || !attached_, true};
}

void BackgroundVideo::attach(std::function<void()> wake)
{
    std::lock_guard lock(mutex_);
    wake_ = std::move(wake);
    attached_ = true;
    requested_.store(false, std::memory_order_relaxed);
    remaining_ = 0;
}

void BackgroundVideo::detach() noexcept
{
    {
        std::lock_guard lock(mutex_);
        attached_ = false;
        wake_ = {};
        requested_.store(false, std::memory_order_relaxed);
        remaining_ = 0;
    }
    changed_.notify_all();
}

bool BackgroundVideo::take(std::uint64_t& request)
{
    if (!requested_.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(mutex_);
    if (!requested_.load(std::memory_order_relaxed)) return false;
    requested_.store(false, std::memory_order_relaxed);
    request = asked_;
    return true;
}

void BackgroundVideo::ended(std::uint64_t request, std::uint32_t streams)
{
    {
        std::lock_guard lock(mutex_);
        answered_ = request;
        streams_ = streams;
        remaining_ = streams;
    }
    changed_.notify_all();
}

void BackgroundVideo::remaining(std::uint32_t streams)
{
    {
        std::lock_guard lock(mutex_);
        remaining_ = streams;
    }
    changed_.notify_all();
}

} // namespace uml::camera
