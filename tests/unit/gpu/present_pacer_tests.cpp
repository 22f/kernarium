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
#include <vector>

namespace {

using uml::gpu::LinkFrameRate;
using uml::gpu::PresentPacer;
using uml::testing::require;

constexpr LinkFrameRate high {80.0F, 120.0F, 120.0F};
constexpr LinkFrameRate normal {60.0F, 60.0F, 60.0F};
constexpr LinkFrameRate low {30.0F, 30.0F, 30.0F};

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
    [[nodiscard]] std::uint64_t presents() const { return presents_; }
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
            if (!waiting_)
                waitingSince_ = now_;
            waiting_ = true;
            wake();
            seed_ = seed_ * 1103515245U + 12345U;
            const double jitter =
                (static_cast<double>((seed_ >> 16U) % 1000U) / 1000.0 - 0.5) *
                0.001;
            nextFrame_ += 1.0 / guestRate_ + jitter;
        }
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
        const auto decision = pacer_.tick(now_, frames_, waiting_, ceiling_);
        if (decision.range)
            request(*decision.range);
        if (decision.present) {
            ++presents_;
            longestWait_ = std::max(longestWait_, now_ - waitingSince_);
            waiting_ = false;
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
    std::uint32_t seed_ {1};
    std::uint64_t frames_ {};
    std::uint64_t presents_ {};
    std::uint64_t ticks_ {};
    std::uint64_t wakes_ {};
    bool waiting_ {};
    bool asleep_ {true};
    std::vector<LinkFrameRate> requests_;
};

void aLinkThatWakesAsksForTheFastestRateTheCeilingAllows()
{
    PresentPacer pacer;
    require(pacer.wake(1.0, 0, 120.0) == high, "a 120 Hz ceiling");
    require(pacer.rate() == 120);
    require(pacer.wake(2.0, 0, 90.0) == normal, "a 90 Hz ceiling is a 60 step");
    require(pacer.wake(3.0, 0, 60.0) == normal, "a 60 Hz ceiling");
    require(pacer.wake(4.0, 0, 30.0) == low, "a 30 Hz ceiling");
    require(pacer.wake(5.0, 0, 0.0) == low, "no ceiling is the slowest step");
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
    require(link.lastRequestAt() < 0.6, "settled within the fall window");
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
    link.run(1.5);
    require(link.rate() == 60);
    const double at = link.now();
    link.setGuestRate(120.0);
    link.run(0.5);
    require(link.rate() == 120, "the link rose");
    require(link.lastRequestAt() - at < 0.25, "within the rise window");
    require(link.requests().back() == high);
}

void aGuestThatSlowsDownMakesTheLinkFallInSteps()
{
    Link link;
    link.setGuestRate(120.0);
    link.run(1.0);
    require(link.rate() == 120);
    double at = link.now();
    link.setGuestRate(60.0);
    link.run(1.0);
    require(link.rate() == 60, "fell to 60");
    require(link.lastRequestAt() - at < 0.6, "within the fall window");
    at = link.now();
    link.setGuestRate(30.0);
    link.run(1.2);
    require(link.rate() == 30, "fell to 30");
    require(link.requests().back() == low);
    require(link.lastRequestAt() - at < 0.8);
    /* A guest that goes straight from 120 to 30 passes 60 without stopping,
     * or at most for a tick. */
    Link straight;
    straight.setGuestRate(120.0);
    straight.run(1.0);
    straight.setGuestRate(30.0);
    straight.run(1.2);
    require(straight.rate() == 30);
}

void aGuestThatSpeedsUpFromThirtyRisesToSixtyAndToOneHundredTwenty()
{
    Link link;
    link.setGuestRate(30.0);
    link.run(1.5);
    require(link.rate() == 30);
    link.setGuestRate(60.0);
    link.run(0.6);
    require(link.rate() == 60);
    link.setGuestRate(120.0);
    link.run(0.6);
    require(link.rate() == 120);
}

void aGuestNearAStepDoesNotMakeTheLinkFlip()
{
    /* 80 frames a second is between the fall from 120 (72) and the rise to
     * 120 (92): the link stays on whichever step it is on. */
    Link onHigh;
    onHigh.setGuestRate(80.0);
    onHigh.run(6.0);
    require(onHigh.rate() == 120);
    require(onHigh.requests().size() == 1, "no step down at 80");

    Link onNormal;
    onNormal.setGuestRate(60.0);
    onNormal.run(1.5);
    require(onNormal.rate() == 60);
    const auto requests = onNormal.requests().size();
    onNormal.setGuestRate(80.0);
    onNormal.run(6.0);
    require(onNormal.rate() == 60, "no step up at 80");
    require(onNormal.requests().size() == requests);

    /* Likewise a guest at 40 on the 60 step and the 30 step. */
    Link onLow;
    onLow.setGuestRate(30.0);
    onLow.run(1.5);
    require(onLow.rate() == 30);
    onLow.setGuestRate(40.0);
    onLow.run(3.0);
    require(onLow.rate() == 30, "no step up at 40");
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
    link.setGuestRate(30.0);
    link.run(1.5);
    require(link.rate() == 30);
    link.setGuestRate(0.0);
    link.run(0.5);
    require(link.asleep());
    /* The next burst is at 120, and the first request is the fastest step
     * again, not the 30 the last burst ended on. */
    const auto before = link.requests().size();
    link.setGuestRate(120.0);
    link.run(0.2);
    require(link.requests().size() == before + 1);
    require(link.requests().back() == high);
    require(link.rate() == 120);
}

} // namespace

int main()
{
    try {
        aLinkThatWakesAsksForTheFastestRateTheCeilingAllows();
        aSixtyHertzGuestSettlesAtSixtyAndKeepsNoFasterLink();
        aOneHundredTwentyHertzGuestKeepsTheFastLink();
        aGuestThatRendersFasterMakesTheLinkRiseAtOnce();
        aGuestThatSlowsDownMakesTheLinkFallInSteps();
        aGuestThatSpeedsUpFromThirtyRisesToSixtyAndToOneHundredTwenty();
        aGuestNearAStepDoesNotMakeTheLinkFlip();
        theLinkSleepsWhenNoFrameHasWaitedForTheIdleTime();
        aFrameThatArrivesBeforeTheIdleTimeKeepsTheLinkAwake();
        aTickPresentsOnlyWhenAFrameWaits();
        theCeilingCapsTheLinkAndItRisesAgainWhenTheCeilingRises();
        aWakeForgetsTheEvidenceOfTheLastBurst();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "present pacer test failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
