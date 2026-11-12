// SPDX-License-Identifier: GPL-2.0-or-later
//
// How the guest GNSS HAL calls the framework (the overlay's gnss_hal.hpp,
// gnss_engine.hpp and gnss_reporter.hpp). The framework answers a callback
// while it holds locks, and its threads hold those locks while they call into
// the HAL, so the HAL must never wait for a callback inside an entry point or
// under a lock an entry point takes. Each case here withholds the framework's
// answer for as long as it likes, bounded by a timeout so that a regression
// fails instead of hanging the suite, and requires every entry point of the HAL
// to return meanwhile. Others check the delivery contract of gnss_reporter.hpp:
// the statuses and the announcement of a callback are never dropped and come in
// the order posted across callbacks, the locations of a session that ended are
// dropped, what a slow framework can make the HAL hold is bounded, and the
// settings an entry point saves are the ones the engine runs. This test is
// GPL-2.0-or-later and includes the overlay's Apache-2.0 headers; the binary is
// built and run by the test suite only and is never distributed.

#include "gnss_engine.hpp"
#include "gnss_hal.hpp"
#include "gnss_hal_test_world.hpp"
#include "gnss_reporter.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <optional>
#include <random>

namespace {

using andarium::gnss::Engine;
using andarium::gnss::Env;
using andarium::gnss::Hal;
using andarium::gnss::Reporter;
using gnsshaltest::Fix;
using gnsshaltest::Sink;
using gnsshaltest::Status;
using gnsshaltest::World;
using gnsshaltest::baseNs;
using gnsshaltest::epoch;
using gnsshaltest::heldWakelocks;
using gnsshaltest::preciseFix;
using gnsshaltest::second;
using uml::testing::require;
using namespace std::chrono_literals;

// How long a case waits for the HAL's entry points to return while the framework
// does not answer. A HAL that waits for the answer takes `blockedFor`, so it
// is the shorter of the two that makes a regression fail.
constexpr auto entryPointsWithin = 5s;
constexpr auto blockedFor = 20s;

/* The framework's answer, withheld: a callback that enters waits until the test
 * opens the gate. */
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    int entered = 0;
    bool open = false;
    bool timedOut = false;

    void block()
    {
        std::unique_lock lock(mutex);
        ++entered;
        changed.notify_all();
        if (!changed.wait_for(lock, blockedFor, [&] { return open; }))
            timedOut = true;
    }

    bool waitEntered(int count = 1)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, entryPointsWithin, [&] { return entered >= count; });
    }

    void release()
    {
        std::lock_guard lock(mutex);
        open = true;
        changed.notify_all();
    }

    bool wasTimedOut()
    {
        std::lock_guard lock(mutex);
        return timedOut;
    }
};

/* Which of a sink's first calls are held at the gate, after the world has
 * recorded them. */
struct Hold {
    int statuses = 0;
    int readies = 0;
    int locations = 0;
    int sentences = 0;
};

/* The world and the gate of one case. A callback still in flight when a case
 * ends finishes on the reporter's own thread afterwards, so the sinks made here
 * keep the gate alive until the last of them is gone, as the HAL's sink keeps
 * the framework's callback; the world itself ignores a call that comes after it
 * is gone. */
struct Scene {
    World world;
    std::shared_ptr<Gate> gateKeep = std::make_shared<Gate>();
    Gate& gate = *gateKeep;

    Sink sink(Hold hold = {})
    {
        Sink result = world.sink();
        const auto holding = [gate = gateKeep](auto recorded, int count) {
            auto remaining = std::make_shared<std::atomic<int>>(count);
            return [gate, recorded, remaining](auto&&... arguments) {
                recorded(arguments...);
                if (remaining->fetch_sub(1) > 0)
                    gate->block();
            };
        };
        result.status = holding(result.status, hold.statuses);
        result.ready = holding(result.ready, hold.readies);
        result.location = holding(result.location, hold.locations);
        result.nmea = holding(result.nmea, hold.sentences);
        return result;
    }

    Scene() = default;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
};

/* Polls `done` until it holds, at most `limit`: for a condition no one
 * notifies about. */
template<typename Predicate>
bool eventually(Predicate done)
{
    const auto deadline = std::chrono::steady_clock::now() + gnsshaltest::limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

/* Calls on threads of their own, the way the framework's binder threads make
 * them. */
class Callers {
public:
    void call(std::function<void()> function)
    {
        {
            std::lock_guard lock(mutex_);
            ++started_;
        }
        threads_.emplace_back([this, function = std::move(function)] {
            function();
            std::lock_guard lock(mutex_);
            ++returned_;
            changed_.notify_all();
        });
    }

    bool allReturned(std::chrono::seconds within = std::chrono::duration_cast<std::chrono::seconds>(entryPointsWithin))
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, within, [&] { return returned_ == started_; });
    }

    ~Callers()
    {
        for (auto& thread : threads_)
            thread.join();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int started_ = 0;
    int returned_ = 0;
    std::vector<std::thread> threads_;
};

/* A call that must return, on a thread of its own so that a call that does not
 * fails the case instead of hanging it. */
void mustReturn(const char* what, std::function<void()> call)
{
    Callers callers;
    callers.call(std::move(call));
    require(callers.allReturned(), what);
}

std::vector<std::string> logOf(World& world)
{
    std::lock_guard lock(world.mutex);
    return world.log;
}

/* The log without the reports: the announcements and the statuses. */
std::vector<std::string> callsOf(World& world)
{
    std::vector<std::string> result;
    for (const auto& call : logOf(world)) {
        if (call != "location" && call != "nmea" && call != "sv")
            result.push_back(call);
    }
    return result;
}

using Calls = std::vector<std::string>;

/* The session a location belongs to, written by the epoch writer as its
 * latitude, a hundredth of a degree for each session. */
constexpr double sessionLatitude(int session)
{
    return 10.0 + session * 0.01;
}

int sessionOf(const andarium::gnss::Location& location)
{
    return static_cast<int>(std::lround((location.latitude - 10.0) * 100));
}

/* Writes an epoch to whatever the engine has open, each with the latitude of
 * the session whose pipe it goes into, so that a location says which session it
 * came from. The pipe belongs to the session, so this holds whichever thread
 * reads. */
class EpochWriter {
public:
    explicit EpochWriter(World& world) : world_(world), thread_([this] { run(); }) {}

    ~EpochWriter()
    {
        stop_ = true;
        thread_.join();
    }

private:
    void run()
    {
        std::int64_t utc = baseNs;
        while (!stop_) {
            {
                std::lock_guard lock(world_.mutex);
                if (world_.feed >= 0) {
                    utc += second;
                    const auto text = epoch(preciseFix(utc, sessionLatitude(world_.opens)));
                    // A full or a closed pipe loses the epoch, which is what a
                    // receiver nobody reads does.
                    if (::write(world_.feed, text.data(), text.size()) < 0 && errno != EPIPE && errno != EAGAIN)
                        require(false, "the receiver pipe failed");
                }
            }
            std::this_thread::sleep_for(200us);
        }
    }

    World& world_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

/* What the framework heard must be sessions in order: engine_on, session_begin,
 * then locations, then session_end and engine_off, or the session replaced
 * (engine_on again, or a new announcement) without an end. And every location
 * is of the session the last session_begin began, and not after its end. Returns
 * an empty string when the log is well formed. */
std::string wellFormed(World& world)
{
    std::lock_guard lock(world.mutex);
    enum { idle, begun, inSession, ended } state = idle;
    int begins = 0;
    std::size_t locations = 0;
    for (std::size_t index = 0; index < world.log.size(); ++index) {
        const std::string& call = world.log[index];
        const std::string where = " at call " + std::to_string(index) + " (" + call + ")";
        if (call == "ready") {
            if (state == begun || state == ended)
                return "an announcement inside a status pair" + where;
            state = idle;
            continue;
        }
        switch (state) {
        case idle:
            if (call != "on")
                return "a call outside a session" + where;
            state = begun;
            break;
        case begun:
            if (call != "begin")
                return "engine_on is not followed by session_begin" + where;
            ++begins;
            state = inSession;
            break;
        case inSession:
            if (call == "on") {
                state = begun;
            } else if (call == "end") {
                state = ended;
            } else if (call == "location") {
                const int session = sessionOf(world.locations.at(locations++));
                if (session != begins)
                    return "a location of session " + std::to_string(session) + " in session " +
                           std::to_string(begins) + where;
            } else if (call != "nmea" && call != "sv") {
                return "unexpected call in a session" + where;
            }
            break;
        case ended:
            if (call != "off")
                return "session_end is not followed by engine_off" + where;
            state = idle;
            break;
        }
    }
    return "";
}

void requireWellFormed(World& world)
{
    const std::string problem = wellFormed(world);
    require(problem.empty(), problem);
}

/* The bug, at the engine: the framework's answer to the status callback of a
 * session's start is withheld, and everything the HAL can be asked meanwhile
 * must still return. A HAL that makes the callback inside start() with its
 * control lock held cannot return from running() or stop(). */
void everyEngineCallReturnsWhileTheFrameworkDoesNotAnswer()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    Engine engine(world.env(), scene.sink({.statuses = 1}));
    Callers callers;
    callers.call([&] { engine.start(); });
    require(gate.waitEntered(), "the status callback of the start never ran");

    Callers others;
    others.call([&] { engine.running(); });
    others.call([&] { engine.last_fix(); });
    others.call([&] { engine.set_min_interval_ms(2000); });
    others.call([&] { engine.set_nmea(true); });
    others.call([&] { engine.set_sv_status(true); });
    others.call([&] { engine.set_nmea(false); });
    others.call([&] { engine.set_sv_status(false); });
    others.call([&] { engine.stop(); });
    others.call([&] { engine.start(); });
    require(others.allReturned(), "an entry point of the engine waits for the framework's answer");
    gate.release();
    require(callers.allReturned());
    engine.stop();
    require(!gate.wasTimedOut());
}

/* The same through the entry points of the HAL, which also take its own locks:
 * every one of them, on a thread of its own, while the framework does not
 * answer. This is the cycle of the field report: a framework thread that holds
 * its status-listener lock calls startSvStatus while another is inside start(). */
void everyHalEntryPointReturnsWhileTheFrameworkDoesNotAnswer()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    Hal hal(world.env());
    hal.set_callback(scene.sink({.statuses = 1}));

    Callers starter;
    starter.call([&] { hal.start(); });
    require(gate.waitEntered(), "the status callback of the start never ran");

    Callers calls;
    calls.call([&] { hal.start(); });
    calls.call([&] { hal.stop(); });
    calls.call([&] { hal.set_position_mode(2000); });
    calls.call([&] { hal.set_sv_status(true); });
    calls.call([&] { hal.set_sv_status(false); });
    calls.call([&] { hal.set_nmea(true); });
    calls.call([&] { hal.set_nmea(false); });
    calls.call([&] { hal.last_fix(); });
    require(calls.allReturned(), "an entry point of the HAL waits for the framework's answer");

    // The calls that replace or drop the callback, while it is still not answered.
    Scene other;
    Callers last;
    last.call([&] { hal.set_callback(other.sink()); });
    require(last.allReturned(), "setCallback waits for the framework's answer");
    Callers closing;
    closing.call([&] { hal.close(); });
    require(closing.allReturned(), "close waits for the framework's answer");

    gate.release();
    require(starter.allReturned());
    require(!gate.wasTimedOut());
}

/* The announcement of a callback is made by the reporter too: with the
 * framework's answer to it withheld, every entry point must still return,
 * including a second setCallback and close. */
void everyEntryPointReturnsWhileTheAnnouncementIsNotAnswered()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    Hal hal(world.env());
    mustReturn("setCallback waits for the announcement", [&] { hal.set_callback(scene.sink({.readies = 1})); });
    require(gate.waitEntered(), "the announcement never ran");

    Callers calls;
    calls.call([&] { hal.start(); });
    calls.call([&] { hal.stop(); });
    calls.call([&] { hal.set_position_mode(2000); });
    calls.call([&] { hal.set_sv_status(true); });
    calls.call([&] { hal.set_nmea(true); });
    calls.call([&] { hal.last_fix(); });
    require(calls.allReturned(), "an entry point of the HAL waits for the announcement");

    Scene other;
    mustReturn("a second setCallback waits for the announcement", [&] { hal.set_callback(other.sink()); });
    mustReturn("close waits for the announcement", [&] { hal.close(); });
    gate.release();
    // Both announcements, and the statuses queued behind the first, are heard.
    require(other.world.wait([&] { return other.world.readies == 1; }));
    require(world.wait([&] { return world.readies == 1; }));
    require(!gate.wasTimedOut());
}

/* The reader thread ends the session by itself when the receiver goes away, and
 * says so; with the framework not answering, it must still end, the wakelock
 * must go, and the framework's stop must return. */
void aDeadReceiverEndsTheSessionWhileTheFrameworkDoesNotAnswer()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    Engine engine(world.env(), scene.sink({.statuses = 1}));
    mustReturn("start waits for the framework's answer", [&] { require(engine.start()); });
    require(gate.waitEntered());
    world.hangUp();
    require(world.wait([&] { return world.wakelocksHeld == 0; }), "the session did not end");
    require(world.wait([&] { return !world.logs.empty(); }));
    Callers calls;
    calls.call([&] { engine.stop(); });
    calls.call([&] { engine.running(); });
    require(calls.allReturned(), "stop waits for the framework's answer");
    gate.release();
    require(world.wait([&] { return world.statuses.size() == 4; }));
    requireWellFormed(world);
}

/* The framework's answer arrives late, and the framework meanwhile stops and
 * starts the session again: the calls still come in order, one session's end
 * before the next one's begin, none of an old session after a new one's begin. */
void statusesOfQueuedSessionsComeInOrder()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    Engine engine(world.env(), scene.sink({.statuses = 1}));
    Callers starter;
    starter.call([&] { engine.start(); });
    require(gate.waitEntered());
    // While the first engine_on is withheld: stop, start, stop, start, stop.
    Callers more;
    more.call([&] {
        engine.stop();
        require(engine.start());
        engine.stop();
        require(engine.start());
        engine.stop();
    });
    require(more.allReturned(), "an entry point waits for the framework's answer");
    gate.release();
    const std::vector<Status> expected{
        Status::engine_on, Status::session_begin, Status::session_end, Status::engine_off,
        Status::engine_on, Status::session_begin, Status::session_end, Status::engine_off,
        Status::engine_on, Status::session_begin, Status::session_end, Status::engine_off};
    require(world.wait([&] { return world.statuses == expected; }));
    requireWellFormed(world);
}

/* A restart without a stop ends the old session silently: the framework hears
 * the second engine_on and session_begin, and no end of the first. */
void aRestartIsSilentAboutTheOldSession()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    Engine engine(world.env(), scene.sink({.statuses = 1}));
    Callers starter;
    starter.call([&] { engine.start(); });
    require(gate.waitEntered());
    Callers more;
    more.call([&] { require(engine.start()); });
    require(more.allReturned(), "an entry point waits for the framework's answer");
    gate.release();
    require(world.wait([&] {
        return world.statuses ==
               std::vector<Status>{Status::engine_on, Status::session_begin, Status::engine_on, Status::session_begin};
    }));
    engine.stop();
    requireWellFormed(world);
}

/* A restart drops the locations the old session still has queued: the old
 * session could not have reported one after the restart returned, when every
 * call was made inside the entry point. The one being delivered completes. */
void aRestartDropsTheOldSessionsQueuedLocations()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    world.boottimeStep = 2 * second;
    Hal hal(world.env());
    hal.set_callback(scene.sink({.locations = 1}));
    require(hal.start() == Hal::StartResult::started);
    for (int index = 0; index < 4; ++index)
        world.write(epoch(preciseFix(baseNs + index * second, sessionLatitude(1))));
    require(gate.waitEntered(), "the first location was not delivered");
    require(world.drained());
    mustReturn("a restart waits for the framework's answer", [&] { require(hal.start() == Hal::StartResult::started); });
    world.write(epoch(preciseFix(baseNs + 10 * second, sessionLatitude(2))));
    require(world.drained());
    gate.release();
    // The session's end is the barrier: everything posted before it is heard.
    mustReturn("stop waits for the framework's answer", [&] { hal.stop(); });
    require(world.wait([&] { return world.log.back() == "off"; }));
    require((callsOf(world) == Calls{"ready", "on", "begin", "on", "begin", "end", "off"}));
    std::lock_guard lock(world.mutex);
    require(world.locations.size() == 2, "a location of the old session was heard after the restart");
    require(sessionOf(world.locations[0]) == 1);
    require(sessionOf(world.locations[1]) == 2);
}

/* Replacing the callback with the same callback object, with the old session's
 * location held: the old location in flight completes, nothing else of the old
 * session is heard, and the new announcement and session follow. One log shows
 * the order across the callbacks. */
void aReplacementDropsTheOldSessionsQueuedLocations()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    world.boottimeStep = 2 * second;
    Hal hal(world.env());
    hal.set_callback(scene.sink({.locations = 1}));
    require(hal.start() == Hal::StartResult::started);
    for (int index = 0; index < 4; ++index)
        world.write(epoch(preciseFix(baseNs + index * second, sessionLatitude(1))));
    require(gate.waitEntered());
    require(world.drained());
    mustReturn("setCallback waits for the framework's answer", [&] { hal.set_callback(scene.sink()); });
    mustReturn("start waits for the framework's answer", [&] { require(hal.start() == Hal::StartResult::started); });
    world.write(epoch(preciseFix(baseNs + 10 * second, sessionLatitude(2))));
    require(world.drained());
    gate.release();
    mustReturn("stop waits for the framework's answer", [&] { hal.stop(); });
    require(world.wait([&] { return world.log.back() == "off"; }));
    require((callsOf(world) == Calls{"ready", "on", "begin", "ready", "on", "begin", "end", "off"}));
    std::lock_guard lock(world.mutex);
    require(world.locations.size() == 2, "a location of the old session was heard after the replacement");
    require(sessionOf(world.locations[0]) == 1);
    require(sessionOf(world.locations[1]) == 2);
}

/* The old callback's statuses are all heard, in order, before the new
 * callback's announcement and session, though the framework's answer to the
 * old session's first status is withheld across the replacement and the new
 * start. */
void aReplacementKeepsTheOrderAcrossCallbacks()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    Hal hal(world.env());
    hal.set_callback(scene.sink({.statuses = 1}));
    mustReturn("start waits for the framework's answer", [&] { require(hal.start() == Hal::StartResult::started); });
    require(gate.waitEntered());
    mustReturn("stop waits for the framework's answer", [&] { hal.stop(); });
    mustReturn("setCallback waits for the framework's answer", [&] { hal.set_callback(scene.sink()); });
    mustReturn("start waits for the framework's answer", [&] { require(hal.start() == Hal::StartResult::started); });
    gate.release();
    require(world.wait([&] { return world.statuses.size() == 6; }));
    require((callsOf(world) == Calls{"ready", "on", "begin", "end", "off", "ready", "on", "begin"}));
    require(!gate.wasTimedOut());
}

/* A replacement waits for the old engine's reader (held here in the release of
 * its wakelock), and so does a start that comes after it: the start belongs to
 * the new callback, after the old session's statuses, never to the old one. */
void aStartDuringAReplacementBelongsToTheNewCallback()
{
    Scene scene;
    World& world = scene.world;
    Scene hold;
    Env env = world.env();
    env.hold_wakelock = [inner = env.hold_wakelock, gate = hold.gateKeep]() -> std::shared_ptr<void> {
        auto held = inner();
        if (!held)
            return nullptr;
        return std::shared_ptr<void>(new char(0), [held, gate](void* token) {
            gate->block();
            delete static_cast<char*>(token);
        });
    };
    Hal hal(env);
    hal.set_callback(scene.sink());
    require(hal.start() == Hal::StartResult::started);
    require(world.wait([&] { return world.statuses.size() == 2; }));
    Callers replacing;
    replacing.call([&] { hal.set_callback(scene.sink()); });
    require(hold.gate.waitEntered(), "the replacement did not reach the old session's wakelock");
    Callers starting;
    starting.call([&] { require(hal.start() == Hal::StartResult::started); });
    hold.gate.release();
    require(replacing.allReturned());
    require(starting.allReturned());
    require(world.wait([&] { return world.statuses.size() == 4; }));
    require((callsOf(world) == Calls{"ready", "on", "begin", "ready", "on", "begin"}));
    hal.stop();
}

/* The framework closes the HAL right after a stop on one of its own paths: the
 * session's end and off are still heard. */
void aStopThenACloseStillDeliversTheEnd()
{
    for (int round = 0; round < 200; ++round) {
        Scene scene;
        World& world = scene.world;
        Hal hal(world.env());
        hal.set_callback(scene.sink());
        require(hal.start() == Hal::StartResult::started);
        require(world.wait([&] { return world.statuses.size() == 2; }));
        hal.stop();
        hal.close();
        require(world.wait([&] { return world.statuses.size() == 4; }), "the end of the session was dropped");
        require((callsOf(world) == Calls{"ready", "on", "begin", "end", "off"}));
    }
}

/* The framework's init and cleanup at boot are a setCallback and a close: the
 * announcement is still heard. */
void aSetCallbackThenACloseStillDeliversTheAnnouncement()
{
    for (int round = 0; round < 200; ++round) {
        Scene scene;
        World& world = scene.world;
        Hal hal(world.env());
        hal.set_callback(scene.sink());
        hal.close();
        require(world.wait([&] { return world.readies == 1; }), "the announcement was dropped");
    }
}

/* The statuses of a closed HAL are heard though the framework's answer to the
 * first was withheld across stop and close. A start after the close is
 * refused. */
void aClosedHalStillDeliversItsStatuses()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    Hal hal(world.env());
    hal.set_callback(scene.sink({.statuses = 1}));
    Callers starter;
    starter.call([&] { hal.start(); });
    require(gate.waitEntered());
    mustReturn("stop waits for the framework's answer", [&] { hal.stop(); });
    mustReturn("close waits for the framework's answer", [&] { hal.close(); });
    heldWakelocks(world, 0);
    require(world.receiverClosed());
    gate.release();
    require(starter.allReturned());
    require(hal.start() == Hal::StartResult::no_callback);
    require(world.wait([&] { return world.statuses.size() == 4; }));
    require((callsOf(world) == Calls{"ready", "on", "begin", "end", "off"}));
}

/* The announcement is delivered whole: close while its first call is held does
 * not cancel the second. */
void theAnnouncementIsDeliveredWholeAfterAClose()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    Scene next;
    Hal hal(world.env());
    Sink sink = scene.sink();
    sink.ready = [&world, keep = scene.gateKeep] {
        {
            std::lock_guard lock(world.mutex);
            world.log.push_back("capabilities");
        }
        keep->block();
        std::lock_guard lock(world.mutex);
        world.log.push_back("system info");
        world.changed.notify_all();
    };
    // `world` outlives the case's last call: the scene's gate stays until the
    // sink is gone, and the world is the first member of the scene.
    hal.set_callback(sink);
    require(gate.waitEntered());
    mustReturn("close waits for the announcement", [&] { hal.close(); });
    gate.release();
    // A new callback's announcement is the barrier behind the old one.
    hal.set_callback(next.sink());
    require(next.world.wait([&] { return next.world.readies == 1; }));
    require((logOf(world) == Calls{"capabilities", "system info"}));
}

/* A closed HAL stops an epoch's sentences between two of them: the sentence in
 * flight completes and the rest of the epoch is not heard. */
void aCloseStopsAnEpochBetweenItsSentences()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    Scene next;
    world.boottimeStep = 2 * second;
    Hal hal(world.env());
    hal.set_nmea(true);
    hal.set_callback(scene.sink({.sentences = 1}));
    require(hal.start() == Hal::StartResult::started);
    world.write(epoch(preciseFix(baseNs, sessionLatitude(1))));
    require(gate.waitEntered(), "no sentence was delivered");
    mustReturn("close waits for the framework's answer", [&] { hal.close(); });
    gate.release();
    // The next callback's announcement is the barrier behind the epoch.
    hal.set_callback(next.sink());
    require(next.world.wait([&] { return next.world.readies == 1; }));
    std::lock_guard lock(world.mutex);
    require(world.sentences.size() == 1, "the rest of the epoch was heard after the close");
}

void aReplacementStopsAnEpochBetweenItsSentences()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    Scene next;
    world.boottimeStep = 2 * second;
    Hal hal(world.env());
    hal.set_nmea(true);
    hal.set_callback(scene.sink({.sentences = 1}));
    require(hal.start() == Hal::StartResult::started);
    world.write(epoch(preciseFix(baseNs, sessionLatitude(1))));
    require(gate.waitEntered(), "no sentence was delivered");
    mustReturn("setCallback waits for the framework's answer", [&] { hal.set_callback(next.sink()); });
    gate.release();
    require(next.world.wait([&] { return next.world.readies == 1; }));
    // The announcement was heard behind the epoch's one sentence, which is all
    // the old callback heard of it.
    std::lock_guard lock(world.mutex);
    require(world.sentences.size() == 1, "the rest of the epoch was heard after the replacement");
}

/* A framework that does not answer cannot make the HAL hold more than 64
 * sessions' statuses: a start is refused, before anything changes, when its
 * four do not fit, and a stop never is. Nothing is dropped: when the framework
 * answers, every admitted session's statuses arrive in order. */
void aStalledFrameworkBoundsTheStatusesByRefusingStarts()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    Hal hal(world.env());
    hal.set_callback(scene.sink({.statuses = 1}));
    int admitted = 0;
    int refused = 0;
    for (int round = 0; round < 200; ++round) {
        if (hal.start() == Hal::StartResult::started) {
            ++admitted;
            hal.stop();
        } else {
            ++refused;
        }
        if (round == 0)
            require(gate.waitEntered());
    }
    require(admitted >= 60 && admitted <= 65, "the number of sessions admitted is the bound's");
    require(refused == 200 - admitted, "a start was refused and a later one admitted");
    {
        std::lock_guard lock(world.mutex);
        require(world.opens == admitted, "a refused start opened the receiver");
        int lines = 0;
        for (const auto& line : world.logs)
            lines += line.find("cannot start") != std::string::npos ? 1 : 0;
        require(lines >= 1 && lines <= refused / 64 + 1, "the refusal is not logged once in a while");
    }
    heldWakelocks(world, 0);
    gate.release();
    std::vector<Status> expected;
    for (int session = 0; session < admitted; ++session) {
        for (const auto status :
             {Status::engine_on, Status::session_begin, Status::session_end, Status::engine_off})
            expected.push_back(status);
    }
    require(world.wait([&] { return world.statuses == expected; }), "an admitted session's statuses were lost");
    require(!gate.wasTimedOut());
}

/* A refused start leaves the session as it was: the one running keeps its
 * receiver and its wakelock, and its stop, which is never refused, still ends
 * it. */
void aRefusedStartLeavesTheRunningSessionAlone()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    world.boottimeStep = 2 * second;
    Hal hal(world.env());
    hal.set_callback(scene.sink({.statuses = 1}));
    require(hal.start() == Hal::StartResult::started);
    int admitted = 1;
    while (admitted < 400 && hal.start() == Hal::StartResult::started)
        ++admitted;
    require(admitted < 400, "restarts were never refused");
    heldWakelocks(world, 1);
    require(!world.receiverClosed(), "a refused start closed the session's receiver");
    {
        std::lock_guard lock(world.mutex);
        require(world.opens == admitted);
    }
    gate.release();
    // The session still reads: its epoch is heard.
    world.write(epoch(preciseFix(baseNs, sessionLatitude(admitted))));
    require(world.wait([&] { return world.locations.size() == 1; }));
    hal.stop();
    std::vector<Status> expected;
    for (int session = 0; session < admitted; ++session) {
        expected.push_back(Status::engine_on);
        expected.push_back(Status::session_begin);
    }
    expected.push_back(Status::session_end);
    expected.push_back(Status::engine_off);
    require(world.wait([&] { return world.statuses == expected; }));
    heldWakelocks(world, 0);
}

/* One thread starts, stops, restarts and replaces at random while a writer
 * feeds the receiver: the framework hears exactly the sequence the entry
 * points ran, with no end or off lost to a restart, and every location is of
 * the session that is running. */
void aDriverHearsExactlyTheSessionsItRan()
{
    Scene scene;
    World& world = scene.world;
    world.boottimeStep = 2 * second;
    Hal hal(world.env());
    hal.set_nmea(true);
    hal.set_sv_status(true);
    Calls expected{"ready"};
    hal.set_callback(scene.sink());
    bool running = false;
    {
        EpochWriter writer(world);
        std::minstd_rand random(4321);
        for (int round = 0; round < 400; ++round) {
            switch (random() % 10) {
            case 0:
            case 1:
            case 2:
            case 3:
                if (hal.start() == Hal::StartResult::started) {
                    expected.push_back("on");
                    expected.push_back("begin");
                    running = true;
                }
                break;
            case 4:
            case 5:
            case 6:
                hal.stop();
                if (running) {
                    expected.push_back("end");
                    expected.push_back("off");
                }
                running = false;
                break;
            case 7:
                // A replacement ends the session silently.
                hal.set_callback(scene.sink());
                expected.push_back("ready");
                running = false;
                break;
            case 8:
                hal.set_sv_status(random() % 2 == 0);
                break;
            default:
                std::this_thread::sleep_for(100us);
                break;
            }
        }
        hal.stop();
        if (running) {
            expected.push_back("end");
            expected.push_back("off");
        }
    }
    require(eventually([&] { return callsOf(world).size() >= expected.size(); }), "calls were lost");
    require(callsOf(world) == expected, "the framework did not hear the sessions that ran");
    requireWellFormed(world);
}

/* Several threads at once: whatever the interleaving, the log is well formed
 * and every location belongs to the running session. */
void randomStartsAndStopsKeepTheCallsInOrder()
{
    Scene scene;
    World& world = scene.world;
    world.boottimeStep = 2 * second;
    Hal hal(world.env());
    hal.set_nmea(true);
    hal.set_sv_status(true);
    hal.set_callback(scene.sink());
    {
        EpochWriter writer(world);
        Callers threads;
        for (int thread = 0; thread < 4; ++thread) {
            threads.call([&, thread] {
                std::minstd_rand random(1234 + thread);
                for (int round = 0; round < 150; ++round) {
                    switch (random() % 6) {
                    case 0:
                    case 1:
                        hal.start();
                        break;
                    case 2:
                        hal.stop();
                        break;
                    case 3:
                        hal.set_sv_status(random() % 2 == 0);
                        break;
                    case 4:
                        hal.last_fix();
                        break;
                    default:
                        if (random() % 8 == 0)
                            hal.set_callback(scene.sink());
                        break;
                    }
                    if (random() % 4 == 0)
                        std::this_thread::yield();
                }
            });
        }
        require(threads.allReturned(60s), "the starts and stops did not finish");
    }
    // End on a whole session, so that the log ends at rest.
    require(hal.start() == Hal::StartResult::started);
    hal.stop();
    require(world.wait([&] { return !world.log.empty() && world.log.back() == "off"; }));
    requireWellFormed(world);
    hal.close();
}

/* A callback that calls back into the HAL, the way framework code does while
 * it handles a status, does not wait for anything. */
void aCallbackMayCallBackIntoTheHal()
{
    Scene scene;
    World& world = scene.world;
    Hal hal(world.env());
    Sink sink = scene.sink();
    auto recorded = sink.status;
    auto nested = std::make_shared<std::atomic<int>>(0);
    sink.status = [&hal, recorded, nested](Status status) {
        recorded(status);
        if (status == Status::session_begin) {
            hal.set_sv_status(true);
            hal.set_nmea(true);
            hal.last_fix();
            hal.set_position_mode(1500);
            ++*nested;
        }
        if (status == Status::engine_off) {
            hal.stop();
            ++*nested;
        }
    };
    hal.set_callback(sink);
    mustReturn("a callback that calls the HAL deadlocks it",
               [&] { require(hal.start() == Hal::StartResult::started); });
    require(eventually([&] { return *nested == 1; }));
    mustReturn("a callback that calls the HAL deadlocks it", [&] { hal.stop(); });
    require(eventually([&] { return *nested == 2; }));
    requireWellFormed(world);
}

/* The first call a callback hears is the announcement of what the receiver
 * is, then the session. */
void theAnnouncementComesFirst()
{
    Scene scene;
    World& world = scene.world;
    Hal hal(world.env());
    hal.set_callback(scene.sink());
    require(hal.start() == Hal::StartResult::started);
    hal.stop();
    require(world.wait([&] { return world.statuses.size() == 4; }));
    require((callsOf(world) == Calls{"ready", "on", "begin", "end", "off"}));
}

/* The reporter: delivers in order, never waits for a delivery, drops reports
 * and never essentials, and bounds the reports it keeps. */
void theReporterDeliversInOrder()
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<int> delivered;
    const auto token = Reporter::make_token();
    Reporter reporter;
    for (int index = 0; index < 50; ++index) {
        const auto deliver = [&, index] {
            std::lock_guard lock(mutex);
            delivered.push_back(index);
            changed.notify_all();
        };
        if (index % 3 == 0)
            reporter.post_essential(deliver, false);
        else
            reporter.post_report(token, deliver);
    }
    std::unique_lock lock(mutex);
    require(changed.wait_for(lock, 10s, [&] { return delivered.size() == 50; }));
    for (int index = 0; index < 50; ++index)
        require(delivered[static_cast<std::size_t>(index)] == index);
}

void theReporterKeepsTheNewestReportsAndEveryEssential()
{
    Gate gate;
    std::mutex mutex;
    std::vector<int> delivered;
    std::vector<int> essentials;
    const auto token = Reporter::make_token();
    Reporter reporter;
    reporter.post_essential([&] { gate.block(); }, false);
    require(gate.waitEntered());
    for (int index = 0; index < 1000; ++index) {
        reporter.post_report(token, [&, index] {
            std::lock_guard lock(mutex);
            delivered.push_back(index);
        });
        if (index % 100 == 0) {
            reporter.post_essential(
                [&, index] {
                    std::lock_guard lock(mutex);
                    essentials.push_back(index);
                },
                false);
        }
    }
    gate.release();
    require(eventually([&] {
        std::lock_guard lock(mutex);
        return essentials.size() == 10 && delivered.size() == Reporter::kMaxReports;
    }), "the reporter did not deliver what it kept");
    std::lock_guard lock(mutex);
    // The newest are the ones kept, in order, and no status was dropped.
    require(delivered.back() == 999 && delivered.front() == 1000 - static_cast<int>(Reporter::kMaxReports));
    for (std::size_t index = 1; index < delivered.size(); ++index)
        require(delivered[index] > delivered[index - 1]);
    for (std::size_t index = 0; index < essentials.size(); ++index)
        require(essentials[index] == static_cast<int>(index) * 100);
}

void theReporterRefusesWhatDoesNotFitAndGivesRoomBack()
{
    Reporter reporter;
    require(reporter.reserve(Reporter::kMaxStatuses));
    require(!reporter.reserve(1));
    reporter.release(4);
    require(reporter.reserve(4));
    require(!reporter.reserve(1));
    reporter.release(Reporter::kMaxStatuses);
    require(reporter.reserve(Reporter::kMaxStatuses));
    // Giving back more than was taken leaves nothing taken.
    reporter.release(Reporter::kMaxStatuses + 10);
    require(reporter.reserve(Reporter::kMaxStatuses));
}

void retiringASessionDropsItsReportsOnly()
{
    Gate gate;
    std::mutex mutex;
    std::vector<int> delivered;
    const auto old = Reporter::make_token();
    const auto current = Reporter::make_token();
    {
        Reporter reporter;
        reporter.post_essential([&] { gate.block(); }, false);
        require(gate.waitEntered());
        for (int index = 0; index < 5; ++index) {
            reporter.post_report(old, [&, index] {
                std::lock_guard lock(mutex);
                delivered.push_back(index);
            });
        }
        reporter.post_report(current, [&] {
            std::lock_guard lock(mutex);
            delivered.push_back(100);
        });
        reporter.retire(old);
        // A report of a retired session is not queued.
        reporter.post_report(old, [&] {
            std::lock_guard lock(mutex);
            delivered.push_back(200);
        });
        gate.release();
        // Destroying the idle reporter waits for nothing in flight, and what it
        // held is delivered.
        require(eventually([&] {
            std::lock_guard lock(mutex);
            return !delivered.empty();
        }));
    }
    std::lock_guard lock(mutex);
    require((delivered == std::vector<int>{100}), "a retired session's report was delivered");
}

/* Destroying the reporter does not wait for a call in flight, never drops an
 * essential item, and drops the reports. */
void theReporterNeverWaitsForACallbackInFlightAndKeepsItsEssentials()
{
    Gate gate;
    std::atomic<bool> returned{false};
    std::mutex mutex;
    std::vector<int> delivered;
    const auto token = Reporter::make_token();
    auto reporter = std::make_unique<Reporter>();
    reporter->post_essential(
        [&] {
            gate.block();
            returned = true;
        },
        false);
    require(gate.waitEntered());
    for (int index = 1; index <= 3; ++index) {
        reporter->post_essential(
            [&, index] {
                std::lock_guard lock(mutex);
                delivered.push_back(index);
            },
            false);
    }
    reporter->post_report(token, [&] {
        std::lock_guard lock(mutex);
        delivered.push_back(99);
    });
    Callers destroying;
    destroying.call([&] { reporter.reset(); });
    require(destroying.allReturned(), "destroying the reporter waits for the callback in flight");
    require(!returned);
    gate.release();
    require(eventually([&] {
        std::lock_guard lock(mutex);
        return delivered.size() == 3;
    }), "an essential item was dropped");
    // Then the thread ends: wait for the in-flight call to have returned.
    require(eventually([&] { return returned.load(); }));
    std::lock_guard lock(mutex);
    require((delivered == std::vector<int>{1, 2, 3}), "a report was heard after the reporter was destroyed");
}

} // namespace

int main()
{
    // The receiver pipe's reader closes while the test may still write.
    signal(SIGPIPE, SIG_IGN);
    static constexpr uml::testing::NamedTest tests[] = {
        {"every-engine-call-returns-while-the-framework-does-not-answer",
         everyEngineCallReturnsWhileTheFrameworkDoesNotAnswer},
        {"every-hal-entry-point-returns-while-the-framework-does-not-answer",
         everyHalEntryPointReturnsWhileTheFrameworkDoesNotAnswer},
        {"every-entry-point-returns-while-the-announcement-is-not-answered",
         everyEntryPointReturnsWhileTheAnnouncementIsNotAnswered},
        {"a-dead-receiver-ends-the-session-while-the-framework-does-not-answer",
         aDeadReceiverEndsTheSessionWhileTheFrameworkDoesNotAnswer},
        {"statuses-of-queued-sessions-come-in-order", statusesOfQueuedSessionsComeInOrder},
        {"a-restart-is-silent-about-the-old-session", aRestartIsSilentAboutTheOldSession},
        {"a-restart-drops-the-old-sessions-queued-locations", aRestartDropsTheOldSessionsQueuedLocations},
        {"a-replacement-drops-the-old-sessions-queued-locations", aReplacementDropsTheOldSessionsQueuedLocations},
        {"a-replacement-keeps-the-order-across-callbacks", aReplacementKeepsTheOrderAcrossCallbacks},
        {"a-start-during-a-replacement-belongs-to-the-new-callback", aStartDuringAReplacementBelongsToTheNewCallback},
        {"a-stop-then-a-close-still-delivers-the-end", aStopThenACloseStillDeliversTheEnd},
        {"a-set-callback-then-a-close-still-delivers-the-announcement",
         aSetCallbackThenACloseStillDeliversTheAnnouncement},
        {"a-closed-hal-still-delivers-its-statuses", aClosedHalStillDeliversItsStatuses},
        {"the-announcement-is-delivered-whole-after-a-close", theAnnouncementIsDeliveredWholeAfterAClose},
        {"a-close-stops-an-epoch-between-its-sentences", aCloseStopsAnEpochBetweenItsSentences},
        {"a-replacement-stops-an-epoch-between-its-sentences", aReplacementStopsAnEpochBetweenItsSentences},
        {"a-stalled-framework-bounds-the-statuses-by-refusing-starts", aStalledFrameworkBoundsTheStatusesByRefusingStarts},
        {"a-refused-start-leaves-the-running-session-alone", aRefusedStartLeavesTheRunningSessionAlone},
        {"a-driver-hears-exactly-the-sessions-it-ran", aDriverHearsExactlyTheSessionsItRan},
        {"random-starts-and-stops-keep-the-calls-in-order", randomStartsAndStopsKeepTheCallsInOrder},
        {"a-callback-may-call-back-into-the-hal", aCallbackMayCallBackIntoTheHal},
        {"the-announcement-comes-first", theAnnouncementComesFirst},
        {"the-reporter-delivers-in-order", theReporterDeliversInOrder},
        {"the-reporter-keeps-the-newest-reports-and-every-essential", theReporterKeepsTheNewestReportsAndEveryEssential},
        {"the-reporter-refuses-what-does-not-fit-and-gives-room-back", theReporterRefusesWhatDoesNotFitAndGivesRoomBack},
        {"retiring-a-session-drops-its-reports-only", retiringASessionDropsItsReportsOnly},
        {"the-reporter-never-waits-for-a-callback-in-flight-and-keeps-its-essentials",
         theReporterNeverWaitsForACallbackInFlightAndKeepsItsEssentials},
    };
    return uml::testing::runNamedTests(tests);
}
