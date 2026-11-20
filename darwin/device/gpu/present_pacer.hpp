// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace uml::gpu {

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
 * at: it rises at once when the guest renders faster and falls when the
 * recent frames show a slower guest. A link that wakes asks for the fastest
 * rate the ceiling allows, because the first frame of a burst should wait for
 * the shortest tick, and settles on the evidence of the burst. The ceiling is
 * the fastest rate the host allows now (the mode's rate, or less when the
 * screen can show less, as in Low Power Mode).
 *
 * The link sleeps when no frame has been waiting for idleSeconds. Whoever
 * runs the link keeps one flag, whether a frame waits, and clears it when a
 * tick presents. */
class PresentPacer final {
public:
    struct Parameters {
        /* How long without a frame before the link sleeps. */
        double idleSeconds {0.1};
        /* The window a rise is judged over, and the window a fall is. */
        double riseWindowSeconds {0.2};
        double fallWindowSeconds {0.4};
        /* A rate in frames a second: at or above this the guest renders
         * faster than the next step down, and the link rises to it ... */
        double riseToHigh {92.0};
        double riseToNormal {50.0};
        /* ... and below this the guest renders slower than the step, and the
         * link falls from it. A guest between the two on a step stays on it:
         * the gap is wider than a frame or two counted in either window. */
        double fallFromHigh {72.0};
        double fallFromNormal {36.0};
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
     * most of it. */
    [[nodiscard]] std::optional<double> framesPerSecond(
        double now, double window) const noexcept;

    Parameters parameters_;
    unsigned rate_ {60};
    double lastPresent_ {};
    std::array<Sample, capacity> samples_ {};
    std::size_t first_ {};
    std::size_t count_ {};
};

} // namespace uml::gpu
