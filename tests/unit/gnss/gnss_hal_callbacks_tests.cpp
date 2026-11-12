// SPDX-License-Identifier: GPL-2.0-or-later
//
// How the guest GNSS HAL calls the framework (the overlay's gnss_hal.hpp,
// gnss_engine.hpp and gnss_reporter.hpp). The framework answers a callback
// while it holds locks, and its threads hold those locks while they call into
// the HAL, so the HAL must never wait for a callback inside an entry point or
// under a lock an entry point takes. Each case here blocks the framework's
// answer for as long as it likes, bounded by a timeout so that a regression
// fails instead of hanging the suite, and requires every entry point of the HAL
// to return meanwhile; others check that the calls still come in order, that
// nothing queued is delivered to a replaced or closed callback, and that the
// reporter bounds what a slow framework makes it hold. This test is
// GPL-2.0-or-later and includes the overlay's Apache-2.0 headers; the binary is
// built and run by the test suite only and is never distributed.

#include "gnss_engine.hpp"
#include "gnss_hal.hpp"
#include "gnss_hal_test_world.hpp"
#include "gnss_reporter.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <csignal>
#include <cstdlib>
#include <functional>
#include <optional>
#include <random>

namespace {

using andarium::gnss::Engine;
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

/* The world and the gate of one case. A callback still in flight when a case
 * ends finishes on the reporter's own thread afterwards, so the sinks made here
 * keep both alive until the last of them is gone, as the HAL's sink keeps the
 * framework's callback. */
struct Scene {
    std::shared_ptr<World> keep = std::make_shared<World>();
    std::shared_ptr<Gate> gateKeep = std::make_shared<Gate>();
    World& world = *keep;
    Gate& gate = *gateKeep;

    /* The world's sink, with the first `blocking` status callbacks held at the
     * gate after the world has recorded them. */
    Sink sink(int blocking = 0) const
    {
        Sink result = world.sink();
        auto remaining = std::make_shared<std::atomic<int>>(blocking);
        auto recorded = result.status;
        result.status = [recorded, remaining, world = keep, gate = gateKeep](Status status) {
            recorded(status);
            if (remaining->fetch_sub(1) > 0)
                gate->block();
        };
        return result;
    }
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

/* The statuses and locations the framework heard form sessions: engine_on then
 * session_begin, locations and sentences, then session_end and engine_off, with
 * a restart (engine_on again) allowed to end a session without them. Nothing is
 * heard outside a session. Returns an empty string when the log is well formed. */
std::string wellFormed(const std::vector<std::string>& log)
{
    enum { idle, begun, inSession, ended } state = idle;
    for (std::size_t index = 0; index < log.size(); ++index) {
        const std::string& call = log[index];
        const std::string where = " at call " + std::to_string(index) + " (" + call + ")";
        if (call == "ready") {
            if (index != 0)
                return "ready is not first" + where;
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
            state = inSession;
            break;
        case inSession:
            if (call == "on")
                state = begun;
            else if (call == "end")
                state = ended;
            else if (call != "location" && call != "nmea" && call != "sv")
                return "unexpected call in a session" + where;
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
    const std::string problem = wellFormed(logOf(world));
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
    Engine engine(world.env(), scene.sink(1));
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

/* The same through the entry points of the HAL, which also take its own lock:
 * every one of them, on a thread of its own, while the framework does not
 * answer. This is the cycle of the field report: a framework thread that holds
 * its status-listener lock calls startSvStatus while another is inside start(). */
void everyHalEntryPointReturnsWhileTheFrameworkDoesNotAnswer()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    Hal hal(world.env());
    hal.set_callback(scene.sink(1));

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

/* The reader thread ends the session by itself when the receiver goes away, and
 * says so; with the framework not answering, it must still end, the wakelock
 * must go, and the framework's stop must return. */
void aDeadReceiverEndsTheSessionWhileTheFrameworkDoesNotAnswer()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    Engine engine(world.env(), scene.sink(1));
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
    Engine engine(world.env(), scene.sink(1));
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
    Engine engine(world.env(), scene.sink(1));
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

/* Several threads start and stop the HAL at random while a receiver writes
 * epochs: whatever the interleaving, the framework hears sessions in order. */
void randomStartsAndStopsKeepTheCallsInOrder()
{
    Scene scene;
    World& world = scene.world;
    world.boottimeStep = 2 * second;
    Hal hal(world.env());
    hal.set_nmea(true);
    hal.set_sv_status(true);
    hal.set_callback(scene.sink());
    std::atomic<bool> writing{true};
    std::thread writer([&] {
        for (int index = 0; writing; ++index) {
            const auto text = epoch(preciseFix(baseNs + index * second));
            {
                std::lock_guard lock(world.mutex);
                if (world.feed >= 0 && ::write(world.feed, text.data(), text.size()) < 0 && errno != EPIPE &&
                    errno != EAGAIN)
                    require(false, "the receiver pipe failed");
            }
            std::this_thread::sleep_for(200us);
        }
    });
    {
        Callers threads;
        for (int thread = 0; thread < 4; ++thread) {
            threads.call([&, thread] {
                std::minstd_rand random(1234 + thread);
                for (int round = 0; round < 150; ++round) {
                    switch (random() % 5) {
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
                    default:
                        hal.last_fix();
                        break;
                    }
                    if (random() % 4 == 0)
                        std::this_thread::yield();
                }
            });
        }
        require(threads.allReturned(60s), "the starts and stops did not finish");
    }
    writing = false;
    writer.join();
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
    const auto log = logOf(world);
    require((log == std::vector<std::string>{"ready", "on", "begin", "end", "off"}));
}

/* Replacing the callback or closing the HAL drops what is queued for the old
 * callback: after the call that is in flight returns, the old one hears nothing
 * more, and the new one hears only its own session. */
void aReplacedOrClosedCallbackHearsNothingQueued()
{
    Scene scene;
    World& first = scene.world;
    Gate& gate = scene.gate;
    Scene next;
    World& second_world = next.world;
    Hal hal(first.env());
    hal.set_callback(scene.sink(1));
    Callers starter;
    starter.call([&] { hal.start(); });
    require(gate.waitEntered());
    // Behind the withheld engine_on are session_begin, and now session_end and
    // engine_off.
    mustReturn("stop waits for the framework's answer", [&] { hal.stop(); });
    mustReturn("setCallback waits for the framework's answer", [&] { hal.set_callback(next.sink()); });
    mustReturn("start waits for the framework's answer",
               [&] { require(hal.start() == Hal::StartResult::started); });
    require(second_world.wait([&] { return second_world.statuses.size() == 2; }));
    gate.release();
    require(starter.allReturned());
    hal.close();
    // The first callback heard its announcement and the one call in flight.
    require((logOf(first) == std::vector<std::string>{"ready", "on"}));
    require((logOf(second_world) == std::vector<std::string>{"ready", "on", "begin"}));
    require(!gate.wasTimedOut());
}

void aClosedHalHearsNothingQueued()
{
    Scene scene;
    World& world = scene.world;
    Gate& gate = scene.gate;
    Hal hal(world.env());
    hal.set_callback(scene.sink(1));
    Callers starter;
    starter.call([&] { hal.start(); });
    require(gate.waitEntered());
    mustReturn("stop waits for the framework's answer", [&] { hal.stop(); });
    mustReturn("close waits for the framework's answer", [&] { hal.close(); });
    heldWakelocks(world, 0);
    require(world.receiverClosed());
    gate.release();
    require(starter.allReturned());
    // A call after close() is refused, not queued.
    require(hal.start() == Hal::StartResult::no_callback);
    require((logOf(world) == std::vector<std::string>{"ready", "on"}));
}

/* The reporter: delivers in order, never waits for a delivery, and bounds what
 * a framework that does not answer makes it keep. */
void theReporterDeliversInOrder()
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<int> delivered;
    Reporter reporter;
    for (int index = 0; index < 50; ++index) {
        reporter.post(index % 3 == 0 ? Reporter::Kind::essential : Reporter::Kind::report, [&, index] {
            std::lock_guard lock(mutex);
            delivered.push_back(index);
            changed.notify_all();
        });
    }
    std::unique_lock lock(mutex);
    require(changed.wait_for(lock, 10s, [&] { return delivered.size() == 50; }));
    for (int index = 0; index < 50; ++index)
        require(delivered[static_cast<std::size_t>(index)] == index);
}

void theReporterBoundsWhatAStalledFrameworkMakesItKeep()
{
    Gate gate;
    std::mutex mutex;
    std::vector<int> delivered;
    std::vector<int> essentials;
    {
        Reporter reporter;
        reporter.post(Reporter::Kind::essential, [&] { gate.block(); });
        require(gate.waitEntered());
        for (int index = 0; index < 1000; ++index) {
            reporter.post(Reporter::Kind::report, [&, index] {
                std::lock_guard lock(mutex);
                delivered.push_back(index);
            });
            if (index % 100 == 0) {
                reporter.post(Reporter::Kind::essential, [&, index] {
                    std::lock_guard lock(mutex);
                    essentials.push_back(index);
                });
            }
        }
        gate.release();
        // The last essential was posted after the last report but one, so when
        // it has been delivered everything that was kept has been.
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            {
                std::lock_guard lock(mutex);
                if (!essentials.empty() && essentials.back() == 900 && !delivered.empty() &&
                    delivered.back() == 999)
                    break;
            }
            require(std::chrono::steady_clock::now() < deadline, "the reporter did not deliver what it kept");
            std::this_thread::sleep_for(1ms);
        }
    }
    std::lock_guard lock(mutex);
    require(delivered.size() <= Reporter::kMaxReports, "the reports kept are bounded");
    // The newest are the ones kept, in order.
    require(delivered.back() == 999);
    for (std::size_t index = 1; index < delivered.size(); ++index)
        require(delivered[index] > delivered[index - 1]);
    // A status is never dropped.
    require(essentials.size() == 10);
    for (std::size_t index = 0; index < essentials.size(); ++index)
        require(essentials[index] == static_cast<int>(index) * 100);
}

void theReporterNeverWaitsForACallbackInFlight()
{
    Gate gate;
    std::atomic<bool> returned{false};
    std::atomic<int> after{0};
    auto reporter = std::make_unique<Reporter>();
    reporter->post(Reporter::Kind::essential, [&] {
        gate.block();
        returned = true;
    });
    require(gate.waitEntered());
    reporter->post(Reporter::Kind::essential, [&] { ++after; });
    // Closing and destroying return while the callback is still in flight.
    Callers callers;
    callers.call([&] { reporter->close(); });
    callers.call([&] { reporter->post(Reporter::Kind::essential, [&] { ++after; }); });
    require(callers.allReturned(), "close waits for the callback in flight");
    Callers destroying;
    destroying.call([&] { reporter.reset(); });
    require(destroying.allReturned(), "destroying the reporter waits for the callback in flight");
    require(!returned);
    gate.release();
    // The callback in flight finishes on its own; nothing behind it runs.
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!returned && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    require(returned);
    require(after == 0, "a call queued behind the one in flight was made after close");
}

void aClosedReporterTakesNothingMore()
{
    std::atomic<int> calls{0};
    Reporter reporter;
    reporter.close();
    reporter.post(Reporter::Kind::essential, [&] { ++calls; });
    reporter.post(Reporter::Kind::report, [&] { ++calls; });
    std::this_thread::sleep_for(50ms);
    require(calls == 0);
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
        {"a-dead-receiver-ends-the-session-while-the-framework-does-not-answer",
         aDeadReceiverEndsTheSessionWhileTheFrameworkDoesNotAnswer},
        {"statuses-of-queued-sessions-come-in-order", statusesOfQueuedSessionsComeInOrder},
        {"a-restart-is-silent-about-the-old-session", aRestartIsSilentAboutTheOldSession},
        {"random-starts-and-stops-keep-the-calls-in-order", randomStartsAndStopsKeepTheCallsInOrder},
        {"a-callback-may-call-back-into-the-hal", aCallbackMayCallBackIntoTheHal},
        {"the-announcement-comes-first", theAnnouncementComesFirst},
        {"a-replaced-callback-hears-nothing-queued", aReplacedOrClosedCallbackHearsNothingQueued},
        {"a-closed-hal-hears-nothing-queued", aClosedHalHearsNothingQueued},
        {"the-reporter-delivers-in-order", theReporterDeliversInOrder},
        {"the-reporter-bounds-what-a-stalled-framework-makes-it-keep",
         theReporterBoundsWhatAStalledFrameworkMakesItKeep},
        {"the-reporter-never-waits-for-a-callback-in-flight", theReporterNeverWaitsForACallbackInFlight},
        {"a-closed-reporter-takes-nothing-more", aClosedReporterTakesNothingMore},
    };
    return uml::testing::runNamedTests(tests);
}
