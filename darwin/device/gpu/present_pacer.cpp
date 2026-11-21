// SPDX-License-Identifier: GPL-2.0-or-later

#include "present_pacer.hpp"

namespace uml::gpu {

PresentPacer::PresentPacer(Parameters parameters)
    : parameters_(parameters)
{
}

unsigned PresentPacer::rateForCeiling(double ceiling) noexcept
{
    /* Any ceiling under the 120 step's is the 60 step: a mode that states 47
     * (a size too large for even a 60 Hz timing) is shown whole by it. */
    return ceiling >= highStepMinimumCeiling ? 120 : 60;
}

LinkFrameRate PresentPacer::rangeFor(unsigned rate) noexcept
{
    /* The top step asks for a range that cannot be met by 60, so that iOS
     * gives a ProMotion screen its fastest rate (Apple's guidance for
     * content that wants it); the 60 step states one rate. */
    if (rate == 120)
        return {80.0F, 120.0F, 120.0F};
    return {60.0F, 60.0F, 60.0F};
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

void PresentPacer::drew(bool unavailable) noexcept
{
    if (!unavailable) {
        retryOwed_ = false;
        retries_ = 0;
        return;
    }
    retryOwed_ = retries_ < parameters_.maximumDrawRetries;
    if (retryOwed_)
        ++retries_;
}

bool PresentPacer::retryOwed() const noexcept
{
    return retryOwed_;
}

void PresentPacer::frameArrived() noexcept
{
    retries_ = 0;
}

bool PresentPacer::arrangeRetry() noexcept
{
    if (!retryOwed_ || retryArranged_)
        return false;
    retryArranged_ = true;
    return true;
}

void PresentPacer::retryDelayRan() noexcept
{
    retryArranged_ = false;
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
    double now, double window, double minimumSpan) const noexcept
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
    /* Fewer samples than the minimum cannot say a rate: a frame more or less
     * is 1 / span frames a second. */
    if (span < minimumSpan || span <= 0.0)
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
    /* The rise measure decides the step, and the fall test runs only where
     * the rise measure no longer supports it: a fall measure over 2 s lags the
     * one over a second, and would otherwise take the link down as it rises,
     * tick after tick. The step is supported down to the fall threshold, which
     * is under the rise one by more than the measure's frame. */
    const auto rise = framesPerSecond(
        now, parameters_.riseWindowSeconds, parameters_.riseMinimumSpanSeconds);
    if (rise && *rise >= parameters_.riseToHigh) {
        next = 120;
    } else if (rate_ == 120 && (!rise || *rise < parameters_.fallFromHigh)) {
        if (const auto fall = framesPerSecond(
                now, parameters_.fallWindowSeconds, parameters_.fallMinimumSpanSeconds)) {
            if (*fall < parameters_.fallFromHigh)
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
    /* A frame that arrived has its own retries. */
    if (frameWaiting)
        frameArrived();
    if (frameWaiting || retryOwed_) {
        decision.present = true;
        lastPresent_ = now;
        retryOwed_ = false;
    } else if (now - lastPresent_ >= parameters_.idleSeconds) {
        decision.sleep = true;
    }
    return decision;
}

} // namespace uml::gpu
