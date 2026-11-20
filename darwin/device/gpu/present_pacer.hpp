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

/* What a draw came to: the frame was shown, the drawable or the render pass
 * was not there (the frame is still the newest), or it was not there after
 * the drawable's own timeout, a wait of about a second on the main thread
 * that a retry would repeat. */
enum class DrawOutcome : std::uint8_t {
    shown,
    unavailable,
    stalled,
};

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
 * at. A tick shows only the newest frame, so a link on a step shows at most
 * that step's rate: a guest above it by more than 2 frames a second must
 * not stay on it (the setting off shows every frame up to the 60 of its
 * mode), and a rise is judged over a second, which a frame more or less
 * moves by 1 frame a second only. A fall is judged over 2 s, so that a guest
 * that alternates across a step's band for less than that stays on the
 * higher step, and the thresholds leave the two quantizations between them
 * (a rise needs 32.3 over the 30 step and 62.5 over the 60 step, a fall
 * under 30.7 and 61), so that no steady guest makes the link flip. A link that
 * wakes asks for the fastest rate the ceiling allows, because the first
 * frame of a burst should wait for the shortest tick, and settles after the
 * first second of the burst. The ceiling is the fastest rate the host allows
 * now (the mode's rate, or less when the screen can show less, as in Low
 * Power Mode).
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
         * faster than the step the link is on, and the link rises to the
         * step above. */
        double riseToHigh {62.5};
        double riseToNormal {32.3};
        /* ... and below this the guest renders slower than the step, and the
         * link falls from it. */
        double fallFromHigh {61.0};
        double fallFromNormal {30.7};
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
     * the display's own draw when it has no link). A draw that found no
     * drawable owes the frame another one at the next ticks, up to the
     * limit, and a draw that shows the frame ends them; a draw that waited
     * out the drawable's timeout spends the whole limit, so that a stalled
     * GPU does not hold the main thread again and again. */
    void drew(DrawOutcome outcome) noexcept;

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

    /* The rate the link now asks for, 120, 60 or 30. */
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
    std::array<Sample, capacity> samples_ {};
    std::size_t first_ {};
    std::size_t count_ {};
};

} // namespace uml::gpu
