// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>

namespace uml::audio {

/* An engine that is stale, or whose output node reports no format, is not
 * started as it is. */
[[nodiscard]] constexpr bool mustReplaceEngine(bool stale, bool routeUp) noexcept
{
    return stale || !routeUp;
}

/* Whether the host output's engine may be started as it is. An AVAudioEngine
 * is bound to the output route it was made on. While the app is in the
 * background with its session inactive, iOS takes that route away: the
 * engine's output node then reports no format, starting it fails for the rest
 * of the run, and nothing done to that engine (reconnecting it, preparing it
 * again, taking the session again) brings it back. Only an engine made after
 * the route is there again works, so an engine that was caught by the loss is
 * replaced, never started.
 *
 * Releasing a stream and taking the session back is routine in the
 * foreground, and does not harm the engine, so it does not replace it; the
 * evidence does: the output node has no format, an engine configuration change
 * arrived while the sink held no session, or a start failed. */
class EngineHealth final {
public:
    /* The engine is replaced before it is started. `routeUp` is whether its
     * output node reports a format. */
    [[nodiscard]] bool mustReplace(bool routeUp) const noexcept
    {
        return mustReplaceEngine(stale_, routeUp);
    }

    [[nodiscard]] bool stale() const noexcept { return stale_; }

    void invalidate() noexcept { stale_ = true; }

    /* A fresh engine took the place of the old one. */
    void replaced() noexcept { stale_ = false; }

    /* The engine, or the audio route under it, changed. `fromEngine` is true
     * for the engine's own configuration-change notification and false for a
     * route change of the session; `sessionHeld` is whether the sink held the
     * session when it arrived. An engine's notification without the session
     * means the route went with it, and an output node without a format
     * means the route is gone, whoever said so. */
    void configurationChanged(
        bool fromEngine, bool sessionHeld, bool routeUp) noexcept
    {
        if (!routeUp || (fromEngine && !sessionHeld))
            stale_ = true;
    }

private:
    bool stale_ {};
};

/* How long a sink whose output has no route yet keeps asking for it. The
 * route comes back some seconds after the app returns, and nothing announces
 * it, so a sink that starts into a missing route plays muted (the stream
 * keeps its clock) and starts the output again with a fresh engine once a
 * second, on the poll that already asks whether another app let go of the
 * session. A wait ends when the output runs, when the stream stops, is
 * released or is configured again, or when it has lasted `limit`: the route
 * is then reported as a failure of the output, like any other, and only a
 * later route change or a new start asks again. */
class RouteWait final {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr auto limit = std::chrono::seconds(15);

    /* Starts waiting at `now`, unless a wait is under way: asking again does
     * not extend it. */
    void begin(Clock::time_point now) noexcept
    {
        if (!waiting_) {
            waiting_ = true;
            since_ = now;
        }
    }

    void end() noexcept { waiting_ = false; }

    [[nodiscard]] bool waiting() const noexcept { return waiting_; }

    [[nodiscard]] bool expired(Clock::time_point now) const noexcept
    {
        return waiting_ && now - since_ >= limit;
    }

private:
    bool waiting_ {};
    Clock::time_point since_ {};
};

} // namespace uml::audio
