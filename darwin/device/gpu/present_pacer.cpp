// SPDX-License-Identifier: GPL-2.0-or-later

#include "present_pacer.hpp"

namespace uml::gpu {

PresentPacer::PresentPacer(Parameters parameters)
    : parameters_(parameters)
{
}

unsigned PresentPacer::rateForCeiling(double ceiling) noexcept
{
    if (ceiling >= 100.0)
        return 120;
    return ceiling >= 50.0 ? 60 : 30;
}

LinkFrameRate PresentPacer::rangeFor(unsigned rate) noexcept
{
    /* The top step asks for a range that cannot be met by 60, so that iOS
     * gives a ProMotion screen its fastest rate (Apple's guidance for
     * content that wants it); the others state one rate. */
    switch (rate) {
    case 120:
        return {80.0F, 120.0F, 120.0F};
    case 30:
        return {30.0F, 30.0F, 30.0F};
    default:
        return {60.0F, 60.0F, 60.0F};
    }
}

unsigned PresentPacer::rate() const noexcept
{
    return rate_;
}

LinkFrameRate PresentPacer::wake(
    double now, std::uint64_t guestFrames, double ceiling) noexcept
{
    first_ = 0;
    count_ = 0;
    remember(now, guestFrames);
    rate_ = rateForCeiling(ceiling);
    lastPresent_ = now;
    return rangeFor(rate_);
}

void PresentPacer::remember(double now, std::uint64_t guestFrames) noexcept
{
    if (count_ == capacity) {
        first_ = (first_ + 1U) % capacity;
        --count_;
    }
    samples_[(first_ + count_) % capacity] = {now, guestFrames};
    ++count_;
}

std::optional<double> PresentPacer::framesPerSecond(
    double now, double window) const noexcept
{
    if (count_ < 2)
        return std::nullopt;
    const Sample newest = samples_[(first_ + count_ - 1U) % capacity];
    std::size_t oldest = count_ - 1U;
    while (oldest > 0) {
        const Sample& earlier = samples_[(first_ + oldest - 1U) % capacity];
        if (earlier.time < now - window)
            break;
        --oldest;
    }
    if (oldest == count_ - 1U)
        return std::nullopt;
    const Sample start = samples_[(first_ + oldest) % capacity];
    const double span = newest.time - start.time;
    /* Fewer samples than most of the window cannot say a rate. */
    if (span < 0.6 * window)
        return std::nullopt;
    return static_cast<double>(newest.frames - start.frames) / span;
}

PresentPacer::Decision PresentPacer::tick(
    double now,
    std::uint64_t guestFrames,
    bool frameWaiting,
    double ceiling) noexcept
{
    remember(now, guestFrames);
    Decision decision;
    unsigned next = rate_;
    if (const auto fast = framesPerSecond(now, parameters_.riseWindowSeconds)) {
        if (*fast >= parameters_.riseToHigh)
            next = 120;
        else if (next == 30 && *fast >= parameters_.riseToNormal)
            next = 60;
    }
    if (next == rate_) {
        if (const auto slow =
                framesPerSecond(now, parameters_.fallWindowSeconds)) {
            if (*slow < parameters_.fallFromNormal && rate_ > 30)
                next = 30;
            else if (*slow < parameters_.fallFromHigh && rate_ == 120)
                next = 60;
        }
    }
    const unsigned cap = rateForCeiling(ceiling);
    if (next > cap)
        next = cap;
    if (next != rate_) {
        rate_ = next;
        decision.range = rangeFor(next);
    }
    if (frameWaiting) {
        decision.present = true;
        lastPresent_ = now;
    } else if (now - lastPresent_ >= parameters_.idleSeconds) {
        decision.sleep = true;
    }
    return decision;
}

} // namespace uml::gpu
