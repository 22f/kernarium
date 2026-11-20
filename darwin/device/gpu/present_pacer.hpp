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
 * The link follows the rate the guest renders at, in three steps, 120, 60 and
 * 30 frames a second. A guest that renders at 60 on a 120 Hz mode, which is
 * what Android does for most content, would keep the display at 120 for
 * ticks that carry no frame, so the link asks for the rate the frames come
 * at: it rises at once when the guest renders faster than its step and falls
 * when the recent frames show a slower guest. A guest above a step's own rate
 * by more than a window can tell does not stay on that step, because a tick
 * shows only the newest frame, so a link that stayed would show fewer frames
 * than the guest renders. A link that wakes asks for the fastest rate the
 * ceiling allows, because the first frame of a burst should wait for the
 * shortest tick, and settles on the evidence of the burst. The ceiling is the
 * fastest rate the host allows now (the mode's rate, or less when the screen
 * can show less, as in Low Power Mode).
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
        /* The window a rise is judged over, and the window a fall is. */
        double riseWindowSeconds {0.2};
        double fallWindowSeconds {0.4};
        /* A rate in frames a second: at or above this the guest renders
         * faster than the step the link is on, and the link rises to the
         * step above. It is just above the step's own rate, by the frame or
         * two that the 0.2 s window counts (a frame is 5 fps in it): 36 over
         * the 30 step is 8 frames in the window, 72 over the 60 step is 15 ...
         * */
        double riseToHigh {72.0};
        double riseToNormal {36.0};
        /* ... and below this the guest renders slower than the step, and the
         * link falls from it. The fall is under the rise by more than the
         * window's frame, so that a guest between the two stays on its step
         * and no steady guest makes the link flip: 33 is over the 30 step's
         * own rate and under what 8 frames of 0.2 s count, 64 is over the 60
         * step's. */
        double fallFromHigh {64.0};
        double fallFromNormal {33.0};
        /* How many times a frame is drawn again after a draw that found no
         * drawable, at the next ticks: 30 is a quarter of a second at the
         * 120 step and a second at the 30 step. */
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
     * frames offered so far: forgets the evidence of the last burst and
     * returns the range to ask for, the fastest the ceiling allows. */
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

    /* A tick that presented has drawn: `unavailable` says the draw found no
     * drawable or render pass, so that nothing was shown and the frame is
     * still the newest. The next ticks present again, up to the limit, and a
     * draw that shows the frame ends them. */
    void drew(bool unavailable) noexcept;

    /* The rate the link now asks for, 120, 60 or 30. */
    [[nodiscard]] unsigned rate() const noexcept;

    [[nodiscard]] static LinkFrameRate rangeFor(unsigned rate) noexcept;

private:
    struct Sample {
        double time {};
        std::uint64_t frames {};
    };
    /* The newest samples, oldest first: a tick at 120 Hz over the longest
     * window needs 48; the rest is room for a link that ticks faster. */
    static constexpr std::size_t capacity = 128;

    [[nodiscard]] static unsigned rateForCeiling(double ceiling) noexcept;
    void remember(double now, std::uint64_t guestFrames) noexcept;
    /* Frames a second over the last `window` seconds, when the samples span
     * most of it: the frames counted over the span the samples cover, or over
     * the whole window with `wholeWindow`. */
    [[nodiscard]] std::optional<double> framesPerSecond(
        double now, double window, bool wholeWindow) const noexcept;

    Parameters parameters_;
    unsigned rate_ {60};
    double lastPresent_ {};
    /* A draw owed to the newest frame, and how many have been tried since a
     * frame last arrived. */
    bool retryOwed_ {};
    unsigned retries_ {};
    std::array<Sample, capacity> samples_ {};
    std::size_t first_ {};
    std::size_t count_ {};
};

} // namespace uml::gpu
