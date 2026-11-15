// SPDX-License-Identifier: GPL-2.0-or-later

#include "playback_output_recovery.hpp"
#include "require.hpp"

#include <chrono>

namespace {

using uml::audio::EngineHealth;
using uml::audio::mustReplaceEngine;
using uml::audio::RouteWait;
using uml::testing::require;
using namespace std::chrono;

const RouteWait::Clock::time_point origin {seconds(1000)};

// An engine is started as it is only when it is trusted and its output node
// reports a route.
void anEngineWithoutARouteIsNeverStarted()
{
    require(!mustReplaceEngine(false, true), "a trusted engine with a route runs");
    require(mustReplaceEngine(false, false),
        "an output node with no format is replaced, not started");
    require(mustReplaceEngine(true, true), "a stale engine is replaced");
    require(mustReplaceEngine(true, false), "and so is one with no route too");

    EngineHealth health;
    require(!health.mustReplace(true) && !health.stale(),
        "a new engine is fit to start");
    require(health.mustReplace(false), "unless its route is gone");
}

// Releasing a stream does not touch the engine's health; evidence does.
void evidenceMakesAnEngineStale()
{
    EngineHealth health;
    health.configurationChanged(true, true, true);
    require(!health.stale(),
        "the engine's own change with the session held and a route is routine");
    health.configurationChanged(false, false, true);
    require(!health.stale(),
        "a route change of the session says nothing about the engine while "
        "its output still has a route");

    health.configurationChanged(true, false, true);
    require(health.stale() && health.mustReplace(true),
        "the engine's own change with no session held means the route went "
        "with it");
    health.replaced();
    require(!health.stale() && !health.mustReplace(true),
        "a fresh engine is fit to start");

    health.configurationChanged(false, true, false);
    require(health.stale(),
        "a change that leaves the output node without a route stales the "
        "engine, whoever reported it");
    health.replaced();

    health.invalidate();
    require(health.mustReplace(true), "a failed start invalidates the engine");
}

// A stale mark stays until the engine is replaced: a later routine change does
// not clear it.
void aStaleMarkOutlivesRoutineChanges()
{
    EngineHealth health;
    health.invalidate();
    health.configurationChanged(true, true, true);
    require(health.stale(), "a routine change does not make an engine trusted");
    health.replaced();
    require(!health.stale(), "only a replacement does");
}

// The wait for the route is bounded, asking again does not extend it, and it
// can be begun again once it has ended.
void theWaitForTheRouteIsBounded()
{
    RouteWait wait;
    require(!wait.waiting() && !wait.expired(origin + hours(1)),
        "a sink that is not waiting never expires");

    wait.begin(origin);
    require(wait.waiting(), "the wait begins");
    require(!wait.expired(origin + RouteWait::limit - nanoseconds(1)),
        "it has not expired a nanosecond before its limit");
    require(wait.expired(origin + RouteWait::limit),
        "it expires at its limit");

    wait.begin(origin + RouteWait::limit - seconds(1));
    require(wait.expired(origin + RouteWait::limit),
        "asking again does not extend a wait that is under way");

    wait.end();
    require(!wait.waiting() && !wait.expired(origin + hours(1)),
        "a wait that has ended does not expire");

    const auto later = origin + minutes(5);
    wait.begin(later);
    require(!wait.expired(later + RouteWait::limit - nanoseconds(1)) &&
            wait.expired(later + RouteWait::limit),
        "a new wait has a full limit of its own");
}

} // namespace

int main()
{
    anEngineWithoutARouteIsNeverStarted();
    evidenceMakesAnEngineStale();
    aStaleMarkOutlivesRoutineChanges();
    theWaitForTheRouteIsBounded();
    return 0;
}
