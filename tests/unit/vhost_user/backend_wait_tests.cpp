// SPDX-License-Identifier: GPL-2.0-or-later
// The server's wait for the front-end's acknowledgement of a backend request
// (a shared-memory map or unmap, an in-band completion, a configuration
// change), behind the real transport and with the clock a test moves.
//
// The wait ends with the acknowledgement. A deadline runs only while a reply
// the front-end waits for depends on it, and then fails the wait and closes the
// backend channel; a wait nothing is behind, such as a notification a
// suspended guest has not read, has none. The end of the main socket aborts
// the operation before anything the acknowledgement would release is written,
// and a frame that stays incomplete is bound the same way. A service stop ends
// every wait as it always has.
#include "held_request_harness.hpp"
#include "vhost_user_server_testing.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace uml::virtio::held_request_harness;
using uml::gpu::backendAcknowledgementBound;
using uml::gpu::VhostUserServer;
using std::chrono::milliseconds;

constexpr auto bound = std::chrono::duration_cast<milliseconds>(backendAcknowledgementBound);
constexpr auto map = protocol::VhostSlaveRequest::sharedMemoryMap;
constexpr auto unmap = protocol::VhostSlaveRequest::sharedMemoryUnmap;

/* The clock of every wait the server makes while it lives. It starts where
 * steady_clock is and moves only when the test says, so a bound passes by
 * advance(), not by waiting. The server looks at the clock at least every stop
 * interval while a deadline or a frame runs, so awaitReads() shows that it has
 * judged the time as it now is. */
class FakeClock {
public:
    FakeClock()
    {
        base().store(std::chrono::steady_clock::now().time_since_epoch().count());
        offset().store(0);
        reads().store(0);
        uml::gpu::testing::setBackendWaitClock(&FakeClock::now);
    }
    ~FakeClock() { uml::gpu::testing::setBackendWaitClock(nullptr); }
    FakeClock(const FakeClock&) = delete;
    FakeClock& operator=(const FakeClock&) = delete;

    void advance(milliseconds by) { offset().fetch_add(std::chrono::nanoseconds {by}.count()); }
    /* The server's next read of the clock does not return until release(), so
     * that the test can change what it will find, with the server stopped at a
     * known point of its loop. awaitHeld() returns once it is stopped there. */
    void holdNextRead()
    {
        const std::lock_guard lock {gate().mutex};
        gate().hold = true;
    }
    void awaitHeld() const
    {
        std::unique_lock lock {gate().mutex};
        require(gate().changed.wait_for(
                    lock, std::chrono::seconds {20}, [] { return gate().blocked; }),
                "the server did not read the clock");
    }
    void release()
    {
        {
            const std::lock_guard lock {gate().mutex};
            gate().hold = false;
        }
        gate().changed.notify_all();
    }
    [[nodiscard]] std::uint64_t readCount() const { return reads().load(); }
    /* The server reads the clock `count` more times. */
    void awaitReads(std::uint64_t count) const
    {
        const auto target = readCount() + count;
        require(uml::testing::waitUntil(
                    [&] { return readCount() >= target; }, std::chrono::seconds {20}),
                "the server did not look at the clock");
    }
    /* The server has read the clock at all since `before`, which a wait does
     * as its deadline begins. */
    void awaitFirstRead(std::uint64_t before) const
    {
        require(uml::testing::waitUntil(
                    [&] { return readCount() > before; }, std::chrono::seconds {20}),
                "the server did not start a deadline");
    }

private:
    static std::atomic<std::int64_t>& base() { static std::atomic<std::int64_t> value; return value; }
    static std::atomic<std::int64_t>& offset() { static std::atomic<std::int64_t> value; return value; }
    static std::atomic<std::uint64_t>& reads() { static std::atomic<std::uint64_t> value; return value; }
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool hold {};
        bool blocked {};
    };
    static Gate& gate() { static Gate value; return value; }
    static std::chrono::steady_clock::time_point now() noexcept
    {
        {
            std::unique_lock lock {gate().mutex};
            if (gate().hold) {
                gate().blocked = true;
                gate().changed.notify_all();
                gate().changed.wait(lock, [] { return !gate().hold; });
                gate().blocked = false;
            }
        }
        const std::chrono::steady_clock::time_point time {
            std::chrono::steady_clock::duration {base().load() + offset().load()}};
        // Counted once the time is read, so a count shows a time before any
        // advance that follows it.
        reads().fetch_add(1);
        return time;
    }
};

/* How many bytes the server sent on `socket` before it closed it, or -1 if it
 * has not within `timeoutMilliseconds`. */
[[nodiscard]] long bytesBeforeEnd(int socket, int timeoutMilliseconds = 2000)
{
    long bytes = 0;
    const auto deadline = std::chrono::steady_clock::now() + milliseconds {timeoutMilliseconds};
    for (;;) {
        const auto left = std::chrono::duration_cast<milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0)
            return -1;
        pollfd ready {.fd = socket, .events = POLLIN};
        if (::poll(&ready, 1, static_cast<int>(left)) != 1)
            return -1;
        std::array<char, 64> chunk {};
        const auto count = ::read(socket, chunk.data(), chunk.size());
        if (count == 0)
            return bytes;
        if (count < 0)
            return -1;
        bytes += count;
    }
}

/* Whether the socket has nothing to read and has not ended. */
[[nodiscard]] bool connectionOpen(int socket)
{
    pollfd ready {.fd = socket, .events = POLLIN};
    return ::poll(&ready, 1, 0) == 0;
}

/* A server on its own thread, whose connection the test can end by closing the
 * main socket and whose stop flag it holds, so that it shows when and why the
 * thread ended. */
class ServerThread {
public:
    explicit ServerThread(VhostUserServer& server)
    {
        auto sockets = makeSocketPair();
        socket_ = std::move(sockets.descriptors[0]);
        worker_ = std::thread {
            [this, &server, connection = sockets.descriptors[1].release()] {
                server.runConnected(connection, stop_);
                exited_.store(true);
            }};
    }
    ~ServerThread()
    {
        stop_.store(true);
        if (worker_.joinable())
            worker_.join();
    }
    ServerThread(const ServerThread&) = delete;
    ServerThread& operator=(const ServerThread&) = delete;

    [[nodiscard]] int socket() const { return socket_.get(); }
    /* The guest lets the device go. */
    void closeSocket()
    {
        (void)::shutdown(socket_.get(), SHUT_RDWR);
        socket_.reset();
    }
    void stop() { stop_.store(true); }
    [[nodiscard]] bool exited() const { return exited_.load(); }
    [[nodiscard]] bool waitForExit(milliseconds timeout = milliseconds {2000}) const
    {
        return uml::testing::waitUntil([&] { return exited_.load(); }, timeout);
    }

private:
    uml::gpu::UniqueDescriptor socket_;
    std::atomic_bool stop_ {};
    std::atomic_bool exited_ {};
    std::thread worker_;
};

/* A connection whose guest has suspended with one request held and mapped, as
 * in the held-request tests, then resumed its request ring and kicked a second
 * request, whose map the front end has not acknowledged. The server is waiting
 * for it, with nothing owed to the front-end: only the event ring is disabled
 * still. The test's copy of the backend channel is closed, so that the
 * server's closing it shows. */
void withPendingMap(const std::function<void(const HeldSession&)>& body)
{
    withSuspendedGuest(
        {true, false, true},
        1,
        [&](const HeldSession& session) {
            auto& backend = *session.backend;
            backend.tolerateClosedChannel();
            session.requests.enable(session.socket, true);
            require(backend.count(map) == 1);
            backend.hold(true);
            session.requests.postRequest(2);
            session.requests.writeKickPipe();
            awaitCondition([&] { return backend.count(map) == 2; });
            body(session);
        },
        {},
        true);
}

/* The front-end's teardown asks for the features while the map is unanswered.
 * The query waits behind it, and a reply the front-end waits for now depends
 * on the wait: the server has begun to count. A request that the wait answers
 * and that follows the query shows that the server has read it. */
void askForTheFeaturesInsideTheWait(int socket)
{
    sendRequest(socket, protocol::VhostRequest::getFeatures, {}, {}, false);
    (void)readConfig(socket, 0, 1);
}

/* The wait ended with a failure, as the probe of a missing acknowledgement
 * showed it never did: the reply the query waited for was not made up, the
 * connection ended, the backend channel is closed, the device was reset, and
 * the host index holds nothing the guest does not. */
void requireWaitFailed(const HeldSession& session, unsigned resetsBefore)
{
    auto& device = session.device;
    awaitCondition([&] { return device.resets() == resetsBefore + 1; });
    require(device.held() == 0 && device.mapped() == 0, "the failed wait left the device state");
    require(session.backend->channelEnded(), "the backend channel was left open");
    require(!session.regions->contains(0, 0, 1) &&
                !session.regions->contains(0, HoldingDevice::rangeLength, 1),
            "a range stayed in the host index");
    require(session.requests.usedIndex() == 0, "an answer was written by a failed wait");
}

/* A missing acknowledgement no longer strands the teardown: once the query has
 * waited for the bound, the wait fails with a report, the query is not
 * answered, and the connection and the backend channel end. An acknowledgement
 * that arrives afterwards completes nothing. */
void aMissingAcknowledgementEndsTheWait()
{
    FakeClock clock;
    withPendingMap([&](const HeldSession& session) {
        auto& device = session.device;
        auto& backend = *session.backend;
        const int socket = session.socket;
        const auto resets = device.resets();

        askForTheFeaturesInsideTheWait(socket);
        require(connectionOpen(socket), "the wait ended before the bound");

        long sent = 0;
        const auto said = captureStderr([&] {
            clock.advance(3 * bound);
            sent = bytesBeforeEnd(socket);
        });
        require(sent == 0, "the wait did not fail at the bound, or a reply was made up");
        require(said == "holding vhost-user connection failed: vhost-user shared-memory "
                        "acknowledgement missing for 10 s\n",
                "the failure was not reported as expected: " + said);
        requireWaitFailed(session, resets);

        const auto answered = backend.answered();
        backend.hold(false);
        awaitCondition([&] { return backend.answered() == answered + 1; });
        require(session.requests.usedIndex() == 0, "a late acknowledgement completed a request");
        require(device.resets() == resets + 1);
    });
}

/* The bound is exact: a millisecond short of it the server is still waiting,
 * and at it the wait fails. */
void theBoundIsExact()
{
    FakeClock clock;
    withPendingMap([&](const HeldSession& session) {
        const int socket = session.socket;
        const auto resets = session.device.resets();
        askForTheFeaturesInsideTheWait(socket);

        clock.advance(bound - milliseconds {1});
        clock.awaitReads(4);
        require(connectionOpen(socket) && !session.backend->channelEnded(100),
                "the wait failed a millisecond before the bound");

        long sent = 0;
        (void)captureStderr([&] {
            clock.advance(milliseconds {1});
            sent = bytesBeforeEnd(socket);
        });
        require(sent == 0, "the wait did not fail at the bound");
        requireWaitFailed(session, resets);
    });
}

/* An acknowledgement that arrives a millisecond before the bound ends the wait
 * as usual: the query is answered after it, behind the reset it asks for. */
void anAcknowledgementJustBeforeTheBoundIsAccepted()
{
    FakeClock clock;
    withPendingMap([&](const HeldSession& session) {
        auto& device = session.device;
        auto& backend = *session.backend;
        const int socket = session.socket;
        const auto resets = device.resets();
        askForTheFeaturesInsideTheWait(socket);

        clock.advance(bound - milliseconds {1});
        clock.awaitReads(4);
        require(connectionOpen(socket));
        const auto said = captureStderr([&] {
            backend.hold(false);
            (void)readValueReply<std::uint64_t>(socket, protocol::VhostRequest::getFeatures);
        });
        require(said.empty(), "the wait reported a failure: " + said);
        require(device.resets() == resets + 1 && device.held() == 0 && device.mapped() == 0,
                "the features request did not reset the device once the wait was over");
        require(!backend.channelEnded(100), "the backend channel was closed");
        (void)readConfig(socket, 0, 1);
    });
}

/* An acknowledgement that is already waiting on the channel when the bound
 * passes is read, whatever the time: only a poll that finds nothing after the
 * bound fails the wait. The server is stopped just before that poll while the
 * clock moves past the bound and the acknowledgement arrives. */
void anAcknowledgementAlreadyWaitingAtTheBoundIsAccepted()
{
    FakeClock clock;
    withPendingMap([&](const HeldSession& session) {
        auto& device = session.device;
        auto& backend = *session.backend;
        const int socket = session.socket;
        const auto resets = device.resets();
        askForTheFeaturesInsideTheWait(socket);

        clock.holdNextRead();
        clock.awaitHeld();
        clock.advance(2 * bound);
        const auto answered = backend.answered();
        backend.hold(false);
        awaitCondition([&] { return backend.answered() == answered + 1; });
        bool replied = false;
        const auto said = captureStderr([&] {
            clock.release();
            pollfd ready {.fd = socket, .events = POLLIN};
            replied = ::poll(&ready, 1, 5000) == 1;
        });
        require(said.empty(), "the wait reported a failure: " + said);
        require(replied, "the features request was not answered");
        (void)readValueReply<std::uint64_t>(socket, protocol::VhostRequest::getFeatures);
        require(device.resets() == resets + 1 && device.held() == 0 && device.mapped() == 0,
                "the features request did not reset the device once the wait was over");
        require(!backend.channelEnded(100), "the backend channel was closed");
        (void)readConfig(socket, 0, 1);
    });
}

/* A request that asks for an acknowledgement is a reply the front-end waits
 * for, as a query is: the enable of a ring that starts a map begins the count
 * as the wait does, and no query is needed. */
void aRequestThatAsksForAReplyStartsTheCount()
{
    FakeClock clock;
    withSuspendedGuest(
        {true, false, true},
        1,
        [&](const HeldSession& session) {
            auto& backend = *session.backend;
            backend.tolerateClosedChannel();
            const int socket = session.socket;
            const auto resets = session.device.resets();
            backend.hold(true);
            session.requests.postRequest(2);
            session.requests.sendEnable(socket, true);
            awaitCondition([&] { return backend.count(map) == 2; });
            // Answered inside the wait the enable began, so the count has begun.
            (void)readConfig(socket, 0, 1);
            require(connectionOpen(socket));

            long sent = 0;
            (void)captureStderr([&] {
                clock.advance(bound);
                sent = bytesBeforeEnd(socket);
            });
            // The server releases the sender of the request it could not finish.
            require(sent >= 0, "the wait did not fail at the bound");
            requireWaitFailed(session, resets);
        },
        {},
        true);
}

/* The reviewer's second probe: the main socket closes while the map is
 * unanswered, and the request the map came from completes at once. Nothing
 * waits for the clock: the server leaves the wait, writes no answer when the
 * acknowledgement comes late, and ends. */
void theEndOfTheMainSocketAbortsTheWait()
{
    FakeClock clock;
    auto device = std::make_unique<HoldingDevice>(HoldingDevice::Options {true, false, true, true});
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    auto regions = std::make_shared<uml::virtio::SharedMemoryRegions>();
    server.setSharedMemoryRegions(regions);
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};
    ServerThread connection {server};
    const int socket = connection.socket();

    auto backendChannel = openBackendChannel(socket, negotiatedSharedMemory);
    backendChannel.descriptors[1].reset();
    BackendAnswerer backend {backendChannel.descriptors[0].get()};
    backend.tolerateClosedChannel();
    configureMemory(socket, ring.descriptor());
    requests.start(socket, true, true);
    events.start(socket, true, true);
    backend.hold(true);
    requests.postRequest(1);
    requests.writeKickPipe();
    awaitCondition([&] { return backend.count(map) == 1; });
    const auto resets = recorder->resets();
    const auto reads = clock.readCount();

    bool exited = false;
    const auto said = captureStderr([&] {
        connection.closeSocket();
        exited = connection.waitForExit();
    });
    require(exited, "the end of the main socket left the wait running");
    require(said.empty(), "the end of the connection was reported as a failure: " + said);
    require(uml::testing::waitUntil([&] { return recorder->resets() == resets + 1; },
                                    std::chrono::seconds {2}),
            "the device was not reset");
    require(backend.channelEnded(), "the backend channel was left open");

    const auto answered = backend.answered();
    backend.hold(false);
    awaitCondition([&] { return backend.answered() == answered + 1; });
    require(requests.usedIndex() == 0, "an answer was written after the main socket closed");
    require(ring.memory()[responseBufferOffset] == 0 && ring.memory()[responseBufferOffset + 1] == 0,
            "response bytes were written after the main socket closed");
    require(recorder->resets() == resets + 1 && recorder->held() == 0 && recorder->mapped() == 0);
    require(!regions->contains(0, 0, 1), "a range stayed in the host index");
    require(clock.readCount() == reads, "the abort waited for a deadline");
}

/* A guest that suspends with a backend notification unread leaves the server
 * waiting for it, and nothing is behind that wait: no time, however long the
 * guest is held, fails it. The guest's queries are answered meanwhile, and the
 * acknowledgement that comes when it wakes ends the wait and the work goes on. */
void aNotificationPendingAcrossASuspendIsNotAFailure()
{
    FakeClock clock;
    auto device = std::make_unique<HoldingDevice>(HoldingDevice::Options {true, false, false, true});
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};

    withServerConnection(server, [&](int socket) {
        auto backendChannel = openBackendChannel(socket, negotiatedInband);
        backendChannel.descriptors[1].reset();
        BackendAnswerer backend {backendChannel.descriptors[0].get()};
        configureMemory(socket, ring.descriptor());
        // The request queue has no call descriptor: the server calls in-band.
        requests.start(socket, true, true, false);
        events.start(socket, true, true);
        backend.hold(true);
        requests.postRequest(1);
        requests.writeKickPipe();
        constexpr auto call = protocol::VhostSlaveRequest::vringCall;
        awaitCondition([&] { return backend.count(call) == 1; });
        require(requests.usedIndex() == 1, "the request was not completed before its notification");
        const auto resets = recorder->resets();

        // The guest is suspended: a year passes, and its queries are answered.
        clock.advance(milliseconds {365LL * 24 * 3600 * 1000});
        (void)readConfig(socket, 0, 1);
        bool stillWaiting = false;
        // A deadline would fail the wait within a stop interval of the advance.
        const auto said = captureStderr([&] {
            stillWaiting = connectionOpen(socket) && !backend.channelEnded(500);
        });
        require(stillWaiting, "a pending notification was failed while the guest was suspended");
        require(said.empty(), "the wait reported a failure: " + said);

        // It wakes and acknowledges.
        const auto answered = backend.answered();
        backend.hold(false);
        awaitCondition([&] { return backend.answered() == answered + 1; });
        (void)getFeatures(socket);
        require(connectionOpen(socket) && recorder->resets() == resets);
        requests.postRequest(2);
        requests.writeKickPipe();
        awaitCondition([&] { return requests.usedIndex() == 2; });
        (void)readConfig(socket, 0, 1);
    });
}

/* The same guest, suspending as the maintained front-end does: it disables its
 * rings and waits for each reply while it serves the backend channel, so its
 * acknowledgement comes within the bound, and an ordinary suspend is not
 * turned into a failure by the count the replies it waits for start. */
void aSuspendWithANotificationPendingAnswersInTime()
{
    FakeClock clock;
    auto device = std::make_unique<HoldingDevice>(HoldingDevice::Options {true, false, false, true});
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};

    withServerConnection(server, [&](int socket) {
        auto backendChannel = openBackendChannel(socket, negotiatedInband);
        backendChannel.descriptors[1].reset();
        BackendAnswerer backend {backendChannel.descriptors[0].get()};
        configureMemory(socket, ring.descriptor());
        requests.start(socket, true, true, false);
        events.start(socket, true, true);
        backend.hold(true);
        requests.postRequest(1);
        requests.writeKickPipe();
        awaitCondition([&] { return backend.count(protocol::VhostSlaveRequest::vringCall) == 1; });
        const auto resets = recorder->resets();

        events.sendEnable(socket, false);
        requests.sendEnable(socket, false);
        (void)readConfig(socket, 0, 1);
        clock.advance(bound - milliseconds {1});
        clock.awaitReads(4);
        require(connectionOpen(socket) && !backend.channelEnded(100));

        backend.hold(false);
        readAcknowledgement(socket, protocol::VhostRequest::setVringEnable);
        readAcknowledgement(socket, protocol::VhostRequest::setVringEnable);
        require(recorder->resets() == resets, "the suspend reset the device");
        require(connectionOpen(socket) && !backend.channelEnded(100), "the suspend was failed");
        (void)readConfig(socket, 0, 1);
    });
}

/* A partial acknowledgement that never completes: the first bytes of the frame
 * arrive and the rest does not, and nothing is owed to the front-end. The wait
 * fails the bound after the frame began, and not before. */
void aPartialAcknowledgementThatNeverCompletesFails()
{
    FakeClock clock;
    withPendingMap([&](const HeldSession& session) {
        auto& backend = *session.backend;
        const int socket = session.socket;
        const auto resets = session.device.resets();

        const auto reads = clock.readCount();
        backend.splitNextReplyAt(5);
        backend.hold(false);
        awaitCondition([&] { return backend.answered() == 2; });
        clock.awaitFirstRead(reads);
        clock.awaitReads(2);

        clock.advance(bound - milliseconds {1});
        clock.awaitReads(4);
        require(connectionOpen(socket), "the frame was failed a millisecond before the bound");
        long sent = 0;
        const auto said = captureStderr([&] {
            clock.advance(milliseconds {1});
            sent = bytesBeforeEnd(socket);
        });
        require(sent == 0, "the incomplete frame did not fail the wait");
        require(said == "holding vhost-user connection failed: vhost-user frame incomplete: "
                        "its remaining bytes did not arrive within 10000 ms of its first byte\n",
                "the failure was not reported as expected: " + said);
        requireWaitFailed(session, resets);
    });
}

/* A frame that completes before the bound is accepted however long its first
 * bytes were alone: the bound counts from the first byte, and a late peer is
 * not a silent one. */
void aFrameThatCompletesInTimeIsAccepted()
{
    FakeClock clock;
    withPendingMap([&](const HeldSession& session) {
        auto& backend = *session.backend;
        const int socket = session.socket;
        const auto resets = session.device.resets();

        const auto reads = clock.readCount();
        backend.splitNextReplyAt(5);
        backend.hold(false);
        awaitCondition([&] { return backend.answered() == 2; });
        clock.awaitFirstRead(reads);
        clock.awaitReads(2);

        clock.advance(bound - milliseconds {1});
        clock.awaitReads(4);
        require(connectionOpen(socket));
        const auto said = captureStderr([&] {
            backend.finishSplitReply();
            // The map was acknowledged: the request ring's request is held, and
            // the connection goes on.
            awaitCondition([&] { return session.device.mapsCompleted() == 2; });
            (void)readConfig(socket, 0, 1);
        });
        require(said.empty(), "the late frame was reported as a failure: " + said);
        require(session.device.resets() == resets);
    });
}

/* A partial request from the front-end, in the wait, is bound the same way. */
void aPartialRequestFromTheFrontEndFails()
{
    FakeClock clock;
    withPendingMap([&](const HeldSession& session) {
        const int socket = session.socket;
        const auto resets = session.device.resets();

        const auto reads = clock.readCount();
        const std::array<std::uint8_t, 5> beginning {};
        require(::write(socket, beginning.data(), beginning.size()) ==
                static_cast<ssize_t>(beginning.size()));
        clock.awaitFirstRead(reads);
        clock.awaitReads(2);

        clock.advance(bound);
        long sent = 0;
        const auto said = captureStderr([&] { sent = bytesBeforeEnd(socket); });
        require(sent == 0, "the incomplete request did not fail the wait");
        require(said == "holding vhost-user connection failed: vhost-user frame incomplete: "
                        "its remaining bytes did not arrive within 10000 ms of its first byte\n",
                "the failure was not reported as expected: " + said);
        requireWaitFailed(session, resets);
    });
}

/* A reset's unmaps are a wait the guest no longer has a connection behind:
 * once the main socket is gone the teardown has only the acknowledgements to
 * wait for, and fails them at the bound, with the device reset and every range
 * dropped from the host index. */
void aTeardownUnmapThatIsNeverAcknowledgedFails()
{
    FakeClock clock;
    auto device = std::make_unique<HoldingDevice>(HoldingDevice::Options {true, false, true});
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    auto regions = std::make_shared<uml::virtio::SharedMemoryRegions>();
    server.setSharedMemoryRegions(regions);
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};
    ServerThread connection {server};
    const int socket = connection.socket();

    auto backendChannel = openBackendChannel(socket, negotiatedSharedMemory);
    backendChannel.descriptors[1].reset();
    BackendAnswerer backend {backendChannel.descriptors[0].get()};
    backend.tolerateClosedChannel();
    configureMemory(socket, ring.descriptor());
    requests.start(socket, true, true);
    events.start(socket, true, true);
    requests.postRequest(1);
    requests.writeKickPipe();
    awaitCondition([&] { return recorder->mapsCompleted() == 1; });
    require(regions->contains(0, 0, 1));

    // The guest goes without a word to the device, and the unmap is not answered.
    backend.hold(true);
    const auto resets = recorder->resets();
    const auto reads = clock.readCount();
    connection.closeSocket();
    awaitCondition([&] { return backend.count(unmap) == 1; });
    clock.awaitFirstRead(reads);
    require(!connection.exited(), "the teardown did not wait for the unmap");

    clock.advance(bound - milliseconds {1});
    clock.awaitReads(4);
    require(!connection.exited(), "the teardown gave up a millisecond before the bound");
    bool exited = false;
    const auto said = captureStderr([&] {
        clock.advance(milliseconds {1});
        exited = connection.waitForExit();
    });
    require(exited, "the teardown went on waiting for the unmap");
    require(said == "holding vhost-user reset: vhost-user shared-memory acknowledgement missing for "
                    "10 s\n",
            "the failure was not reported as expected: " + said);
    require(recorder->resets() == resets + 1,
            "the device was reset " + std::to_string(recorder->resets() - resets) + " times");
    require(recorder->held() == 0 && recorder->mapped() == 0, "the reset left state");
    require(!regions->contains(0, 0, 1), "a range stayed in the host index");
    require(backend.channelEnded(), "the backend channel was left open");
}

/* A service stop still ends the wait, with or without a reply owed behind it,
 * at once and without a report. */
void aStopEndsTheWait(bool replyOwed)
{
    FakeClock clock;
    auto device = std::make_unique<HoldingDevice>(HoldingDevice::Options {true, false, true});
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};
    ServerThread connection {server};
    const int socket = connection.socket();

    auto backendChannel = openBackendChannel(socket, negotiatedSharedMemory);
    BackendAnswerer backend {backendChannel.descriptors[0].get()};
    configureMemory(socket, ring.descriptor());
    requests.start(socket, true, true);
    events.start(socket, true, true);
    requests.postRequest(1);
    requests.writeKickPipe();
    awaitCondition([&] { return recorder->mapsCompleted() == 1; });
    backend.hold(true);
    requests.postRequest(2);
    requests.writeKickPipe();
    awaitCondition([&] { return backend.count(map) == 2; });
    if (replyOwed) {
        // A ring disable that asks for an acknowledgement waits behind the map.
        events.sendEnable(socket, false);
        (void)readConfig(socket, 0, 1);
    }
    require(!connection.exited());
    const auto resets = recorder->resets();

    bool exited = false;
    const auto said = captureStderr([&] {
        connection.stop();
        exited = connection.waitForExit();
    });
    require(exited, "a stop did not end the wait");
    require(said.empty(), "a stop was reported as a failure: " + said);
    require(uml::testing::waitUntil([&] { return recorder->resets() == resets + 1; },
                                    std::chrono::seconds {2}),
            "the stop did not reset the device");
    require(recorder->held() == 0 && recorder->mapped() == 0);
}

} // namespace

/* With a case's name as its argument, runs that case alone. */
int main(int argc, char** argv)
{
    // A reply to a channel the server has closed is dropped, not fatal.
    std::signal(SIGPIPE, SIG_IGN);
    struct Case {
        std::string name;
        std::function<void()> run;
    };
    const std::vector<Case> cases {
        {"missing-acknowledgement", aMissingAcknowledgementEndsTheWait},
        {"bound-is-exact", theBoundIsExact},
        {"acknowledgement-before-the-bound", anAcknowledgementJustBeforeTheBoundIsAccepted},
        {"acknowledgement-ready-at-the-bound", anAcknowledgementAlreadyWaitingAtTheBoundIsAccepted},
        {"reply-request-starts-the-count", aRequestThatAsksForAReplyStartsTheCount},
        {"main-socket-closed", theEndOfTheMainSocketAbortsTheWait},
        {"notification-across-suspend", aNotificationPendingAcrossASuspendIsNotAFailure},
        {"suspend-with-notification", aSuspendWithANotificationPendingAnswersInTime},
        {"partial-acknowledgement", aPartialAcknowledgementThatNeverCompletesFails},
        {"partial-frame-completes", aFrameThatCompletesInTimeIsAccepted},
        {"partial-request", aPartialRequestFromTheFrontEndFails},
        {"teardown-unmap", aTeardownUnmapThatIsNeverAcknowledgedFails},
        {"stop", [] { aStopEndsTheWait(false); }},
        {"stop-with-a-reply-owed", [] { aStopEndsTheWait(true); }},
    };
    try {
        for (const auto& test : cases) {
            if (argc > 1 && std::string {argv[1]} != test.name)
                continue;
            test.run();
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("vhost-user backend wait ok");
    return 0;
}
