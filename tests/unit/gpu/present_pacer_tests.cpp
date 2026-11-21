// SPDX-License-Identifier: GPL-2.0-or-later

#include "present_pacer.hpp"
#include "require.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using uml::gpu::LinkFrameRate;
using uml::gpu::PresentPacer;
using uml::testing::require;

constexpr LinkFrameRate high {80.0F, 120.0F, 120.0F};
constexpr LinkFrameRate normal {60.0F, 60.0F, 60.0F};

/* The link as the display runs it, on a clock the test moves a millisecond at
 * a time: a guest that offers frames at a rate that can change, a link that
 * sleeps until a frame arrives, wakes at the pacer's fastest rate and ticks
 * at the rate the pacer last asked for (a tick is when iOS gives the link a
 * turn, so the first tick after a wake is one interval later). */
class Link {
public:
    explicit Link(double ceiling = 120.0, PresentPacer::Parameters parameters = {})
        : pacer_(parameters)
        , ceiling_(ceiling)
    {
    }

    void setGuestRate(double framesPerSecond) { guestRate_ = framesPerSecond; }
    void setCeiling(double ceiling) { ceiling_ = ceiling; }
    /* The peak-to-peak jitter of a guest frame's time (1 ms by default; none
     * makes a guest as even as a fake clock can). */
    void setJitter(double seconds) { jitter_ = seconds; }
    /* The next `count` draws find no drawable (a transient failure of the
     * view's drawable or render pass), and show nothing. */
    void failNextDraws(unsigned count) { failures_ = count; }
    /* The link is made again, as when the scene it was on disconnects and
     * another activates before the next tick: the pacer wakes at once, with
     * whatever the display owes the newest frame. */
    void rebind()
    {
        asleep_ = false;
        ++wakes_;
        request(pacer_.wake(now_, frames_, ceiling_));
        nextTick_ = now_ + 1.0 / tickRate();
    }
    /* One frame, now, whatever the guest rate. */
    void offer()
    {
        ++frames_;
        arrived();
        wake();
    }

    /* Runs `seconds` more. */
    void run(double seconds)
    {
        const double end = now_ + seconds;
        while (now_ < end) {
            now_ += step;
            guestFrame();
            if (!asleep_ && now_ + 1e-9 >= nextTick_)
                tick();
        }
    }

    [[nodiscard]] double now() const { return now_; }
    [[nodiscard]] std::uint64_t frames() const { return frames_; }
    /* Draws that showed a frame, and draws made (a failed one included). */
    [[nodiscard]] std::uint64_t presents() const { return presents_; }
    [[nodiscard]] std::uint64_t draws() const { return draws_; }
    /* A frame has arrived that no draw has shown. */
    [[nodiscard]] bool frameWaiting() const { return unshown_; }
    [[nodiscard]] std::uint64_t ticks() const { return ticks_; }
    [[nodiscard]] std::uint64_t wakes() const { return wakes_; }
    [[nodiscard]] bool asleep() const { return asleep_; }
    [[nodiscard]] unsigned rate() const { return pacer_.rate(); }
    [[nodiscard]] const std::vector<LinkFrameRate>& requests() const
    {
        return requests_;
    }
    /* When the link last asked for a range other than the one before. */
    [[nodiscard]] double lastRequestAt() const { return lastRequestAt_; }
    [[nodiscard]] double longestWait() const { return longestWait_; }

private:
    static constexpr double step = 0.001;

    void guestFrame()
    {
        if (guestRate_ <= 0.0)
            return;
        /* Frame times with a deterministic jitter of up to a millisecond. */
        if (nextFrame_ <= 0.0)
            nextFrame_ = now_;
        while (nextFrame_ <= now_) {
            ++frames_;
            arrived();
            wake();
            seed_ = seed_ * 1103515245U + 12345U;
            const double jitter =
                (static_cast<double>((seed_ >> 16U) % 1000U) / 1000.0 - 0.5) *
                jitter_;
            nextFrame_ += 1.0 / guestRate_ + jitter;
        }
    }

    /* The producer's side: the flag the tick takes, and the frame that stays
     * unshown until a draw shows it. */
    void arrived()
    {
        if (!unshown_)
            waitingSince_ = now_;
        unshown_ = true;
        waiting_ = true;
    }

    void wake()
    {
        if (!asleep_)
            return;
        asleep_ = false;
        ++wakes_;
        const auto range = pacer_.wake(now_, frames_, ceiling_);
        request(range);
        nextTick_ = now_ + 1.0 / tickRate();
    }

    void request(LinkFrameRate range)
    {
        if (requests_.empty() || !(requests_.back() == range)) {
            requests_.push_back(range);
            lastRequestAt_ = now_;
        }
        preferred_ = range.preferred;
    }

    [[nodiscard]] double tickRate() const { return preferred_; }

    void tick()
    {
        ++ticks_;
        /* The tick takes the flag before it decides, as the display does. */
        const bool waiting = std::exchange(waiting_, false);
        const auto decision = pacer_.tick(now_, frames_, waiting, ceiling_);
        if (decision.range)
            request(*decision.range);
        if (decision.present) {
            ++draws_;
            /* The tick draws, and says whether the drawable was there. */
            const bool unavailable = failures_ > 0;
            if (unavailable)
                --failures_;
            pacer_.drew(unavailable);
            if (!unavailable) {
                ++presents_;
                longestWait_ = std::max(longestWait_, now_ - waitingSince_);
                unshown_ = false;
            }
        }
        if (decision.sleep) {
            asleep_ = true;
            return;
        }
        nextTick_ += 1.0 / tickRate();
        if (nextTick_ <= now_)
            nextTick_ = now_ + 1.0 / tickRate();
    }

    PresentPacer pacer_;
    double ceiling_;
    double now_ {};
    double guestRate_ {};
    double nextFrame_ {};
    double nextTick_ {};
    double preferred_ {120.0};
    double lastRequestAt_ {};
    double waitingSince_ {};
    double longestWait_ {};
    double jitter_ {0.001};
    std::uint32_t seed_ {1};
    std::uint64_t frames_ {};
    std::uint64_t presents_ {};
    std::uint64_t draws_ {};
    unsigned failures_ {};
    std::uint64_t ticks_ {};
    std::uint64_t wakes_ {};
    bool waiting_ {};
    bool unshown_ {};
    bool asleep_ {true};
    std::vector<LinkFrameRate> requests_;
};

void aLinkThatWakesAsksForTheFastestRateTheCeilingAllows()
{
    PresentPacer pacer;
    require(pacer.wake(1.0, 0, 120.0) == high, "a 120 Hz ceiling");
    require(pacer.rate() == 120);
    require(pacer.wake(2.0, 0, 90.0) == normal, "a 90 Hz ceiling is a 60 step");
    /* The 120 step needs a screen that shows 100 or more: the display is
     * granted the 120 Hz mode only on one (uml::gpu::highStepMinimumCeiling),
     * so a ceiling under it is never the mode's. */
    require(uml::gpu::highStepMinimumCeiling == 100.0);
    require(pacer.wake(2.5, 0, 99.0) == normal, "a 99 Hz ceiling is a 60 step");
    require(pacer.wake(2.6, 0, 100.0) == high, "a 100 Hz ceiling is the 120 step");
    require(pacer.wake(3.0, 0, 60.0) == normal, "a 60 Hz ceiling");
    /* A display that reports neither 60 nor 120 gets the step it fits: the
     * 120 step from 100 up, the 60 step for any lower rate, a mode of 47 Hz
     * (a size too large for a 60 Hz timing), 30 Hz or none included. */
    for (const double ceiling : {47.0, 31.0, 30.0, 1.0, 0.0})
        require(pacer.wake(4.0, 0, ceiling) == normal, "under 100 is the 60 step");
    for (const double ceiling : {100.0, 119.0, 120.0, 144.0})
        require(pacer.wake(5.0, 0, ceiling) == high, "100 and over is the 120 step");
}

void aSixtyHertzGuestSettlesAtSixtyAndKeepsNoFasterLink()
{
    Link link;
    link.setGuestRate(60.0);
    link.run(3.0);
    require(link.rate() == 60, "the link settled at 60");
    require(link.requests().size() == 2 && link.requests()[0] == high &&
                link.requests()[1] == normal,
        "one wake at 120 and one step down");
    require(link.lastRequestAt() < 1.3, "settled on the first second's evidence");
    /* Nearly every frame is presented (two that arrive between ticks are
     * one present), and none later than one tick of the settled rate after
     * it arrived (the first waits for the 120 Hz link). */
    require(link.presents() * 10U >= link.frames() * 9U, "almost every frame");
    require(link.longestWait() < 0.0185, "no frame waits more than a tick");
    /* After settling the link ticks at 60 a second, not 120. */
    const auto ticksBefore = link.ticks();
    link.run(2.0);
    const auto ticks = link.ticks() - ticksBefore;
    require(ticks >= 118 && ticks <= 122, "60 ticks a second at the 60 step");
}

void aOneHundredTwentyHertzGuestKeepsTheFastLink()
{
    Link link;
    link.setGuestRate(120.0);
    link.run(4.0);
    require(link.rate() == 120);
    require(link.requests().size() == 1 && link.requests()[0] == high,
        "no change of range at all");
    require(link.presents() * 10U >= link.frames() * 9U, "almost every frame");
    require(link.longestWait() < 0.0105, "a frame waits at most a tick");
}

void aGuestThatRendersFasterMakesTheLinkRiseAtOnce()
{
    Link link;
    link.setGuestRate(60.0);
    link.run(2.0);
    require(link.rate() == 60);
    const double at = link.now();
    link.setGuestRate(120.0);
    link.run(0.5);
    require(link.rate() == 120, "the link rose");
    require(link.lastRequestAt() - at < 0.25, "a doubled rate lifts the window at once");
    require(link.requests().back() == high);
}

void aGuestThatSlowsDownMakesTheLinkFallAfterASustainedStretch()
{
    Link link;
    link.setGuestRate(120.0);
    link.run(1.0);
    require(link.rate() == 120);
    const double at = link.now();
    link.setGuestRate(60.0);
    link.run(1.5);
    require(link.rate() == 120, "not before a sustained stretch below");
    link.run(1.0);
    require(link.rate() == 60, "fell to 60");
    require(link.requests().back() == normal);
    require(link.lastRequestAt() - at < 2.2, "within the fall window");
    /* A slower guest stays on the 60 step, which is the lowest. */
    link.setGuestRate(24.0);
    link.run(6.0);
    require(link.rate() == 60);
    require(link.requests().back() == normal);
}

/* Where a link starts: asleep, so that it wakes at the fastest step ("high"),
 * or settled on the 60 step by a guest of 45 frames a second ("mid"). */
enum class Start { high, mid };
constexpr Start allStarts[] = {Start::high, Start::mid};

void putOnStep(Link& link, Start start)
{
    if (start == Start::high)
        return;
    link.setGuestRate(45.0);
    link.run(4.0);
    require(link.rate() == 60, "the middle phase is on the 60 step");
}

/* The rule the steps follow. With the setting on, a guest that holds a rate
 * is shown at no fewer frames a second than with the setting off, which
 * shows every frame up to the 60 of its mode: a guest of 60 or less is on the
 * 60 step, which shows 60, and one at 63 or more has the 120 step. What a
 * step shows is checked for a guest well under it. */
void aSteadyGuestIsNeverShownFewerFramesThanTheSettingOffPathBeyondTwo()
{
    for (const Start start : allStarts) {
        for (const double guest : {20.0, 30.0, 31.0, 33.0, 35.0, 45.0, 55.0,
                 59.0, 60.0, 63.0, 65.0, 70.0, 80.0, 100.0, 119.0, 120.0}) {
            Link link;
            link.setJitter(0.0);
            putOnStep(link, start);
            link.setGuestRate(guest);
            link.run(6.0);
            if (guest <= 60.0)
                require(link.rate() == 60, "a guest of 60 or less is on the 60 step");
            if (guest >= 63.0)
                require(link.rate() == 120, "63 or more has the 120 step");
            /* A guest well under its step is shown whole. One near it can have
             * two frames in a tick and none in the next, whatever the step is,
             * so only its step is checked. */
            if (guest > 0.8 * static_cast<double>(link.rate()))
                continue;
            const auto frames = link.frames();
            const auto presents = link.presents();
            link.run(2.0);
            const double shown =
                static_cast<double>(link.presents() - presents) / 2.0;
            const double offered =
                static_cast<double>(link.frames() - frames) / 2.0;
            if (shown < offered - 2.0)
                std::fprintf(stderr, "guest %.0f from start %d is shown %.1f of %.1f\n",
                    guest, static_cast<int>(start), shown, offered);
            require(shown >= offered - 2.0, "an even guest is shown whole, less 2");
        }
    }
}

/* A guest that speeds up to 63 or more, from any step, has the 120 step
 * within 2 s. */
void aGuestThatSpeedsUpToSixtyThreeOrMoreReachesTheHighStepWithinTwoSeconds()
{
    for (const double before : {24.0, 45.0, 58.0}) {
        for (const double guest : {63.0, 64.0, 65.0, 70.0, 90.0, 119.0}) {
            Link link;
            link.setGuestRate(before);
            link.run(4.0);
            link.setGuestRate(guest);
            link.run(2.0);
            if (link.rate() != 120)
                std::fprintf(stderr, "guest %.0f after %.0f is on the %u step\n",
                    guest, before, link.rate());
            require(link.rate() == 120, "the 120 step within 2 s");
        }
    }
}

/* A steady speed-up changes the rate once: the rise measure supports the
 * step it has risen to, so the fall measure (which lags it) is not asked. */
void aSteadySpeedUpChangesTheRateOnce()
{
    struct Change {
        double before;
        double after;
        unsigned step;
    };
    for (const Change change : {Change {50.0, 70.0, 120}, Change {25.0, 63.0, 120},
             Change {30.0, 70.0, 120}, Change {55.0, 65.0, 120},
             Change {58.0, 63.0, 120}, Change {45.0, 64.0, 120},
             Change {60.0, 120.0, 120}, Change {20.0, 40.0, 60}}) {
        Link link;
        link.setGuestRate(change.before);
        link.run(6.0);
        const auto requests = link.requests().size();
        link.setGuestRate(change.after);
        link.run(8.0);
        const auto changes = link.requests().size() - requests;
        if (changes != (change.step == 120 ? 1U : 0U))
            std::fprintf(stderr, "%.0f -> %.0f made %zu changes\n", change.before,
                change.after, changes);
        require(changes == (change.step == 120 ? 1U : 0U),
            "one change for a speed-up over the threshold, none under it");
        require(link.rate() == change.step);
    }
}

/* A guest that alternates across the threshold changes the rate a bounded
 * number of times: at most once in each half of a cycle, and one more (the
 * step it settles on), whatever the pair, so that a fall measure and a rise
 * measure that disagree cannot take the link up and down on every tick. The
 * reviewers counted 169 changes in 12 s for 57 and 64 before the fall waited
 * for the rise measure. */
void aGuestThatAlternatesAcrossTheThresholdChangesTheRateAtMostOncePerHalfCycle()
{
    struct Pair {
        double slow;
        double fast;
    };
    for (const Pair pair : {Pair {57.0, 64.0}, Pair {58.0, 66.0}, Pair {50.0, 70.0},
             Pair {40.0, 70.0}, Pair {20.0, 40.0}, Pair {60.0, 80.0},
             Pair {62.0, 82.0}, Pair {60.0, 120.0}, Pair {30.0, 80.0}}) {
        for (const double half : {0.25, 0.5, 1.0, 1.5, 2.0}) {
            for (const Start start : allStarts) {
                Link link;
                putOnStep(link, start);
                const auto alternate = [&](double seconds) {
                    for (double done = 0.0; done < seconds; done += 2.0 * half) {
                        link.setGuestRate(pair.slow);
                        link.run(half);
                        link.setGuestRate(pair.fast);
                        link.run(half);
                    }
                };
                alternate(6.0);
                const auto requests = link.requests().size();
                alternate(12.0);
                const auto changes = link.requests().size() - requests;
                const auto bound = static_cast<std::size_t>(12.0 / half) + 1U;
                if (changes > bound)
                    std::fprintf(stderr,
                        "%.0f/%.0f half %.2f s from start %d: %zu changes in 12 s, bound %zu\n",
                        pair.slow, pair.fast, half, static_cast<int>(start), changes, bound);
                require(changes <= bound, "more than one change per half cycle");
            }
        }
    }
}

/* A guest that alternates across the threshold in halves of a second or
 * less, with its mean over it, stays on the 120 step and makes no change
 * once the link has settled. */
void aGuestThatAlternatesAboveTheThresholdOnAverageStaysOnTheHighStep()
{
    for (const auto [slow, fast] : {std::pair {60.0, 80.0}, std::pair {62.0, 82.0},
             std::pair {60.0, 120.0}, std::pair {70.0, 74.0}}) {
        for (const double half : {0.25, 0.5, 1.0}) {
            for (const Start start : allStarts) {
                Link link;
                putOnStep(link, start);
                const auto alternate = [&](double seconds) {
                    for (double done = 0.0; done < seconds; done += 2.0 * half) {
                        link.setGuestRate(slow);
                        link.run(half);
                        link.setGuestRate(fast);
                        link.run(half);
                    }
                };
                alternate(6.0);
                const auto requests = link.requests().size();
                alternate(8.0);
                if (link.requests().size() != requests || link.rate() != 120)
                    std::fprintf(stderr, "%.0f<->%.0f half %.2f s from start %d changed\n",
                        slow, fast, half, static_cast<int>(start));
                require(link.requests().size() == requests);
                require(link.rate() == 120, "the 120 step");
            }
        }
    }
}

/* A steady guest makes no request after the link has settled. */
void aSteadyGuestMakesNoLinkFlip()
{
    for (const Start start : allStarts) {
        for (const double guest : {20.0, 30.0, 45.0, 59.0, 60.0, 61.0, 62.0,
                 63.0, 66.0, 80.0, 120.0}) {
            Link link;
            putOnStep(link, start);
            link.setGuestRate(guest);
            link.run(6.0);
            const auto requests = link.requests().size();
            link.run(8.0);
            if (link.requests().size() != requests)
                std::fprintf(stderr, "guest %.0f from start %d changed the range\n",
                    guest, static_cast<int>(start));
            require(link.requests().size() == requests,
                "a steady guest changed the range");
        }
    }
}

/* A draw that finds no drawable shows nothing: with no later frame to ask
 * again, the link must try the same frame at its next ticks, and sleep only
 * when it is shown. */
void aDrawThatFindsNoDrawableIsRetriedUntilTheFrameIsShown()
{
    Link link;
    link.offer();
    link.failNextDraws(5);
    link.run(0.3);
    require(link.presents() == 1, "the frame was shown once");
    require(link.draws() == 6, "five failed draws and the one that showed it");
    require(!link.frameWaiting());
    link.run(0.3);
    require(link.asleep(), "the link sleeps after the frame is shown");
    require(link.draws() == 6, "no further draws");
}

/* A drawable that never comes costs a bounded number of draws, and the link
 * sleeps after them: an idle link does not tick for ever. */
void aDrawThatNeverFindsADrawableIsRetriedAtMostThirtyTimes()
{
    Link link;
    link.offer();
    link.failNextDraws(1000);
    link.run(1.0);
    require(link.draws() == 31, "the first draw and thirty retries");
    require(link.presents() == 0);
    require(link.asleep(), "the link slept once the retries were spent");
    /* A new frame has its own retries, and a drawable that is back shows it
     * at the next tick. */
    link.failNextDraws(2);
    link.offer();
    link.run(0.2);
    require(link.presents() == 1, "the next frame was shown");
    require(link.draws() == 31 + 3);
    link.run(0.3);
    require(link.asleep());
}

/* The pacer's side of it, with no link: a retry is a present that no frame
 * asked for, a shown draw ends the debt, and a frame renews it. */
void aRetryIsAPresentNoFrameAskedFor()
{
    PresentPacer pacer;
    (void)pacer.wake(0.0, 0, 120.0);
    require(pacer.tick(0.0083, 1, true, 120.0).present);
    pacer.drew(true);
    const auto retry = pacer.tick(0.0166, 1, false, 120.0);
    require(retry.present && !retry.sleep, "the retry presents");
    pacer.drew(false);
    const auto done = pacer.tick(0.0249, 1, false, 120.0);
    require(!done.present && !done.sleep, "a shown frame owes nothing");
    /* Never asleep while a retry is owed, though no frame has waited. */
    PresentPacer owing;
    (void)owing.wake(0.0, 0, 120.0);
    require(owing.tick(0.01, 1, true, 120.0).present);
    owing.drew(true);
    require(owing.tick(0.5, 1, false, 120.0).present, "owed after 0.5 s");
    owing.drew(false);
    require(owing.tick(0.51, 1, false, 120.0).sleep == false);
    require(owing.tick(0.7, 1, false, 120.0).sleep);
}

/* A link made again while the newest frame is owed another draw (its scene
 * disconnected and another activated before the next tick) goes on with that
 * frame: the debt is the display's, not the link's. */
void aLinkMadeAgainWhileAFrameIsOwedStillShowsIt()
{
    Link link;
    link.offer();
    link.failNextDraws(1);
    link.run(0.010);
    require(link.draws() == 1 && link.presents() == 0, "one draw, which failed");
    require(link.frameWaiting());
    link.rebind();
    link.run(0.3);
    require(link.presents() == 1, "the frame was shown by the new link");
    require(!link.frameWaiting());
    require(link.draws() == 2, "no more than the one retry");
    link.run(0.3);
    require(link.asleep());
}

/* The limit that is left goes with it: a link made again and again with no
 * new frame never owes more than the frame's own draws. */
void aLinkMadeAgainAndAgainDoesNotRenewTheRetries()
{
    Link link;
    link.offer();
    link.failNextDraws(1000);
    for (int round = 0; round < 40; ++round) {
        link.run(0.020);
        link.rebind();
    }
    link.run(1.0);
    require(link.draws() == 31, "the first draw and thirty retries");
    require(link.presents() == 0);
    require(link.asleep(), "the link slept once they were spent");
    /* Only a frame renews them. */
    link.rebind();
    link.run(0.3);
    require(link.draws() == 31, "a link made again owes nothing more");
    link.failNextDraws(0);
    link.offer();
    link.run(0.2);
    require(link.presents() == 1 && link.draws() == 32);
}

/* The display with no link retries after a delay it arranges: one delay at a
 * time, whatever frames arrive, and none when nothing is owed. */
void theDelayedRetryOfADisplayWithNoLinkIsOneAtATime()
{
    PresentPacer pacer;
    require(!pacer.arrangeRetry(), "nothing owed, nothing arranged");
    pacer.frameArrived();
    pacer.drew(true);
    require(pacer.retryOwed());
    require(pacer.arrangeRetry(), "the first is arranged");
    /* More frames, more failed draws: still the one. */
    for (int frame = 0; frame < 100; ++frame) {
        pacer.frameArrived();
        pacer.drew(true);
        require(!pacer.arrangeRetry(), "a delay is pending already");
    }
    pacer.retryDelayRan();
    require(pacer.arrangeRetry(), "the next after the first ran");
    pacer.retryDelayRan();
    pacer.drew(false);
    require(!pacer.arrangeRetry(), "a frame shown owes nothing");
    /* The limit of a frame is thirty, however many delays it takes, and
     * delays alone do not renew it. */
    PresentPacer spent;
    spent.frameArrived();
    unsigned draws = 0;
    for (int tries = 0; tries < 100; ++tries) {
        spent.drew(true);
        ++draws;
        if (!spent.retryOwed())
            break;
    }
    require(draws == 31, "the draw and thirty retries");
    spent.retryDelayRan();
    require(!spent.retryOwed() && !spent.arrangeRetry(), "nothing more is owed");
    spent.frameArrived();
    spent.drew(true);
    require(spent.retryOwed(), "a frame that arrives has its own");
}

void theLinkSleepsWhenNoFrameHasWaitedForTheIdleTime()
{
    Link link;
    link.setGuestRate(60.0);
    link.run(1.0);
    require(!link.asleep());
    link.setGuestRate(0.0);
    link.run(0.05);
    require(!link.asleep(), "not after 50 ms");
    link.run(0.2);
    require(link.asleep(), "after 250 ms");

    /* Exactly: the tick that finds the last frame presented 100 ms ago
     * sleeps; one a fraction of a millisecond earlier does not. */
    PresentPacer pacer;
    (void)pacer.wake(10.0, 0, 120.0);
    require(pacer.tick(10.0083, 1, true, 120.0).present);
    const auto before = pacer.tick(10.108, 1, false, 120.0);
    require(!before.sleep && !before.present, "99.7 ms after the present");
    const auto after = pacer.tick(10.1084, 1, false, 120.0);
    require(after.sleep && !after.present, "100.1 ms after the present");
}

void aFrameThatArrivesBeforeTheIdleTimeKeepsTheLinkAwake()
{
    Link link;
    /* One frame every 90 ms: the link never sleeps, though only one tick in
     * many has a frame. */
    link.setGuestRate(1.0 / 0.09);
    link.run(2.0);
    require(link.wakes() == 1, "one wake");
    require(!link.asleep());
    /* One frame every 150 ms: the link sleeps between them and wakes for
     * each. */
    Link sparse;
    sparse.setGuestRate(1.0 / 0.15);
    sparse.run(3.0);
    require(sparse.wakes() >= 15, "a wake for most frames");
    require(sparse.presents() >= sparse.frames() - 1U);
    /* The first frame after a wake waits for the first tick of the fastest
     * rate, which is at most 8.4 ms. */
    require(sparse.longestWait() < 0.0105);
}

void aTickPresentsOnlyWhenAFrameWaits()
{
    PresentPacer pacer;
    (void)pacer.wake(0.0, 0, 120.0);
    const auto empty = pacer.tick(0.0083, 0, false, 120.0);
    require(!empty.present && !empty.sleep);
    const auto waiting = pacer.tick(0.0166, 1, true, 120.0);
    require(waiting.present && !waiting.sleep);
}

void theCeilingCapsTheLinkAndItRisesAgainWhenTheCeilingRises()
{
    Link link;
    link.setGuestRate(120.0);
    link.run(1.0);
    require(link.rate() == 120);
    link.setCeiling(60.0);
    link.run(0.05);
    require(link.rate() == 60, "capped at the next tick");
    require(link.requests().back() == normal);
    /* The guest still renders 120: the link stays at the cap. */
    link.run(2.0);
    require(link.rate() == 60);
    link.setCeiling(120.0);
    link.run(0.4);
    require(link.rate() == 120, "rose when the cap lifted");
    /* A link that wakes under a cap asks for the cap. */
    Link capped {60.0};
    capped.setGuestRate(120.0);
    capped.run(1.0);
    require(capped.requests().front() == normal);
    require(capped.rate() == 60);
}

void aWakeForgetsTheEvidenceOfTheLastBurst()
{
    Link link;
    link.setGuestRate(45.0);
    link.run(1.5);
    require(link.rate() == 60);
    link.setGuestRate(0.0);
    link.run(0.5);
    require(link.asleep());
    /* The next burst is at 120, and the first request is the fastest step
     * again, not the 60 the last burst ended on. */
    const auto before = link.requests().size();
    link.setGuestRate(120.0);
    link.run(0.2);
    require(link.requests().size() == before + 1);
    require(link.requests().back() == high);
    require(link.rate() == 120);
}

} // namespace

int main(int argc, char** argv)
{
    /* With a row's name as the argument, that row alone. */
    struct Row {
        const char* name;
        void (*run)();
    };
    const Row rows[] = {
        {"aLinkThatWakesAsksForTheFastestRateTheCeilingAllows",
            aLinkThatWakesAsksForTheFastestRateTheCeilingAllows},
        {"aSixtyHertzGuestSettlesAtSixtyAndKeepsNoFasterLink",
            aSixtyHertzGuestSettlesAtSixtyAndKeepsNoFasterLink},
        {"aOneHundredTwentyHertzGuestKeepsTheFastLink",
            aOneHundredTwentyHertzGuestKeepsTheFastLink},
        {"aGuestThatRendersFasterMakesTheLinkRiseAtOnce",
            aGuestThatRendersFasterMakesTheLinkRiseAtOnce},
        {"aGuestThatSlowsDownMakesTheLinkFallAfterASustainedStretch",
            aGuestThatSlowsDownMakesTheLinkFallAfterASustainedStretch},
        {"aSteadyGuestIsNeverShownFewerFramesThanTheSettingOffPathBeyondTwo",
            aSteadyGuestIsNeverShownFewerFramesThanTheSettingOffPathBeyondTwo},
        {"aGuestThatSpeedsUpToSixtyThreeOrMoreReachesTheHighStepWithinTwoSeconds",
            aGuestThatSpeedsUpToSixtyThreeOrMoreReachesTheHighStepWithinTwoSeconds},
        {"aSteadySpeedUpChangesTheRateOnce", aSteadySpeedUpChangesTheRateOnce},
        {"aGuestThatAlternatesAcrossTheThresholdChangesTheRateAtMostOncePerHalfCycle",
            aGuestThatAlternatesAcrossTheThresholdChangesTheRateAtMostOncePerHalfCycle},
        {"aGuestThatAlternatesAboveTheThresholdOnAverageStaysOnTheHighStep",
            aGuestThatAlternatesAboveTheThresholdOnAverageStaysOnTheHighStep},
        {"aSteadyGuestMakesNoLinkFlip", aSteadyGuestMakesNoLinkFlip},
        {"aDrawThatFindsNoDrawableIsRetriedUntilTheFrameIsShown",
            aDrawThatFindsNoDrawableIsRetriedUntilTheFrameIsShown},
        {"aDrawThatNeverFindsADrawableIsRetriedAtMostThirtyTimes",
            aDrawThatNeverFindsADrawableIsRetriedAtMostThirtyTimes},
        {"aRetryIsAPresentNoFrameAskedFor", aRetryIsAPresentNoFrameAskedFor},
        {"aLinkMadeAgainWhileAFrameIsOwedStillShowsIt",
            aLinkMadeAgainWhileAFrameIsOwedStillShowsIt},
        {"aLinkMadeAgainAndAgainDoesNotRenewTheRetries",
            aLinkMadeAgainAndAgainDoesNotRenewTheRetries},
        {"theDelayedRetryOfADisplayWithNoLinkIsOneAtATime",
            theDelayedRetryOfADisplayWithNoLinkIsOneAtATime},
        {"theLinkSleepsWhenNoFrameHasWaitedForTheIdleTime",
            theLinkSleepsWhenNoFrameHasWaitedForTheIdleTime},
        {"aFrameThatArrivesBeforeTheIdleTimeKeepsTheLinkAwake",
            aFrameThatArrivesBeforeTheIdleTimeKeepsTheLinkAwake},
        {"aTickPresentsOnlyWhenAFrameWaits", aTickPresentsOnlyWhenAFrameWaits},
        {"theCeilingCapsTheLinkAndItRisesAgainWhenTheCeilingRises",
            theCeilingCapsTheLinkAndItRisesAgainWhenTheCeilingRises},
        {"aWakeForgetsTheEvidenceOfTheLastBurst",
            aWakeForgetsTheEvidenceOfTheLastBurst},
    };
    try {
        for (const Row& row : rows) {
            if (argc > 1 && std::string_view(argv[1]) != row.name)
                continue;
            row.run();
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "present pacer test failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
