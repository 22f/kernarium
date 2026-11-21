// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace uml::gpu {

/* The slowest screen that can show the 120 step: a ceiling at or above it asks
 * for 120 (iOS gives a 100 Hz screen its 100), and one below it can ask for no
 * more than 60, so a screen that shows 61 to 99 would be given a 120 Hz mode
 * that it cannot pace. The 120 Hz display is granted only to a screen at or
 * above it, and the application offers the setting only for such a screen. */
inline constexpr double highStepMinimumCeiling = 100.0;

/* What a display link asks the system for, in frames per second: the
 * minimum, the maximum and the preferred rate of a CAFrameRateRange. */
struct LinkFrameRate {
    float minimum {};
    float maximum {};
    float preferred {};

    [[nodiscard]] bool operator==(const LinkFrameRate&) const = default;
};

/* The decisions of the display link that paces the presents of a 120 Hz
 * display (native_display_ios.mm): what the link asks iOS for, whether a tick
 * presents, and when the link sleeps. It is a state machine with no clock and
 * no platform call: the caller gives the time (seconds, monotonic) and the
 * guest's frame counter, and does what the answer says.
 *
 * The link follows the rate the guest renders at, in two steps, 120 and 60
 * frames a second: a guest that renders at 60 on a 120 Hz mode, which is what
 * Android does for most content, would keep the display at 120 for ticks that
 * carry no frame, so the link asks for 60 for it. A tick shows only the newest
 * frame, so a link on the 60 step shows at most 60: a guest above it (the
 * setting off shows every frame up to the 60 of its mode) rises to the 120
 * step, judged over a second. The 120 step is left only when the rise measure
 * no longer supports it and a fall measure over 2 s, which lags it, is below
 * it too: a guest that alternates across the threshold for less than that
 * stays on the 120 step. A step changes no sooner than the dwell (2 s) after
 * the last change, whatever the measures say, so that frames that arrive early
 * and late (which move the count of a second by two between ticks, over a gap
 * of 1.5 frames a second between a rise at 62.5 and a hold under 61) or a rate
 * that wobbles about a threshold change the step at most once in the dwell.
 * Once the link has settled a steady guest of 60 or less, or 63 or more, makes
 * no change; one between them changes it at most once when its frames are
 * early or late by up to 4 ms, or when the interval between them has up to
 * 2 ms of jitter, and never twice within the dwell. The time that a rule
 * states (the second of a rise, the 2 s of a fall and of the dwell) is a time
 * plus one tick: the link decides at its ticks. A link that wakes asks for the fastest rate the
 * ceiling allows, because the first frame of a burst should wait for the
 * shortest tick, and settles after the first second of the burst. The ceiling
 * is the fastest rate the host allows now (the mode's rate, or less when the
 * screen can show less, as in Low Power Mode): the 120 step is for a ceiling
 * of 100 or more, and any lower ceiling, a mode of 47 Hz included, is the 60
 * step.
 *
 * The link sleeps when no frame has been waiting for idleSeconds. Whoever
 * runs the link keeps one flag, whether a frame waits, and clears it when a
 * tick presents. A tick that presents draws, and tells the pacer when the
 * drawable was not there (`drew`): the frame is then drawn again on the next
 * ticks, up to maximumDrawRetries of them, so that a transient failure
 * does not leave the newest frame unshown while the link sleeps. */
class PresentPacer final {
public:
    struct Parameters {
        /* How long without a frame before the link sleeps. */
        double idleSeconds {0.1};
        /* The windows a rise and a fall are judged over, and the least span
         * of samples that says a rate: a window the link has not lived yet
         * (it woke a moment ago) waits for it. */
        double riseWindowSeconds {1.0};
        double riseMinimumSpanSeconds {0.5};
        double fallWindowSeconds {2.0};
        double fallMinimumSpanSeconds {1.0};
        /* A rate in frames a second: at or above this the guest renders
         * faster than the 60 step shows, and the link rises to the 120 step.
         * It stays there while the rise measure holds this ... */
        double riseToHigh {62.5};
        /* ... and falls from it when that measure does not and the fall
         * measure is below this. */
        double fallFromHigh {61.0};
        /* The least time between two step changes. */
        double minimumDwellSeconds {2.0};
        /* How many times a frame is drawn again after a draw that found no
         * drawable, at the next ticks: 30 is a quarter of a second at the
         * 120 step and half a second at the 60 step. */
        unsigned maximumDrawRetries {30};
    };

    /* What a tick is to do. */
    struct Decision {
        /* Present the newest frame. */
        bool present {};
        /* Pause the link: nothing has waited for idleSeconds. */
        bool sleep {};
        /* The range to ask for from now on, when it changed. */
        std::optional<LinkFrameRate> range;
    };

    explicit PresentPacer(Parameters parameters);
    PresentPacer() : PresentPacer(Parameters {}) {}

    /* The link wakes from sleep (or starts) at `now`, with `guestFrames`
     * frames offered so far: forgets the evidence of the last burst, keeps a
     * retry that is owed, and returns the range to ask for, the fastest the
     * ceiling allows. */
    [[nodiscard]] LinkFrameRate wake(
        double now, std::uint64_t guestFrames, double ceiling) noexcept;

    /* A tick at `now`. `guestFrames` is the count of frames the guest has
     * offered (never decreasing) and `frameWaiting` whether one has not been
     * presented. */
    [[nodiscard]] Decision tick(
        double now,
        std::uint64_t guestFrames,
        bool frameWaiting,
        double ceiling) noexcept;

    /* A draw has been made for the newest frame (a tick that presented, or
     * the display's own draw when it has no link): `unavailable` says it
     * found no drawable or render pass, which owes the frame another draw at
     * the next ticks, up to the limit; a draw that shows the frame ends
     * them. */
    void drew(bool unavailable) noexcept;

    /* Whether the newest frame is owed another draw. The debt, and what is
     * left of the limit, outlive a wake: a link made again (the scene it was
     * on went) goes on with the frame that was not shown, and only a frame
     * that arrives renews the limit (`frameArrived`, which a tick does for
     * the frame it is told of). */
    [[nodiscard]] bool retryOwed() const noexcept;
    void frameArrived() noexcept;

    /* For a display with no link to tick, which draws a frame itself and
     * asks again after a delay: whether the caller is to arrange that delay
     * now, which is when a retry is owed and none is arranged already.
     * There is at most one, whatever frames arrive; `retryDelayRan` says it
     * ran. */
    [[nodiscard]] bool arrangeRetry() noexcept;
    void retryDelayRan() noexcept;

    /* The rate the link now asks for, 120 or 60. */
    [[nodiscard]] unsigned rate() const noexcept;

    [[nodiscard]] static LinkFrameRate rangeFor(unsigned rate) noexcept;

private:
    struct Sample {
        double time {};
        std::uint64_t frames {};
    };
    /* The newest samples, oldest first: a tick at 120 Hz over the longest
     * window (2 s) needs 240; the rest is room for a link that ticks
     * faster. */
    static constexpr std::size_t capacity = 512;

    [[nodiscard]] static unsigned rateForCeiling(double ceiling) noexcept;
    void remember(double now, std::uint64_t guestFrames) noexcept;
    /* Frames a second over the samples of the last `window` seconds: the
     * frames counted over the span they cover, when it is at least
     * `minimumSpan`. */
    [[nodiscard]] std::optional<double> framesPerSecond(
        double now, double window, double minimumSpan) const noexcept;

    Parameters parameters_;
    unsigned rate_ {60};
    double lastPresent_ {};
    /* A draw owed to the newest frame, and how many have been tried since a
     * frame last arrived. */
    bool retryOwed_ {};
    unsigned retries_ {};
    bool retryArranged_ {};
    /* When the step last changed since the last wake. */
    std::optional<double> changedAt_;
    std::array<Sample, capacity> samples_ {};
    std::size_t first_ {};
    std::size_t count_ {};
};

} // namespace uml::gpu
