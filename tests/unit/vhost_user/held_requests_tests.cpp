// SPDX-License-Identifier: GPL-2.0-or-later
// A device that keeps its deferred requests across a guest's suspend, behind
// the real transport. The guest suspends by disabling its rings one after
// another and resumes by enabling them in the same order, sending nothing
// else but kicks: none of it may reset such a device, what the device
// completes or raises meanwhile reaches the guest at the ring's enable, and
// the messages that change what the requests refer to still retire them.
// The front end is the GPU tests', so their support header and protocol
// header are on the path.
#include "gpu_test_support.hpp"
#include "vhost_user_device.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace uml::gpu::testing;
using uml::gpu::VhostUserServer;
using uml::testing::require;
using uml::virtio::VhostUserDevice;
using uml::virtio::VhostUserQueueAction;
using uml::virtio::VhostUserQueueCompletion;
using uml::virtio::VhostUserQueueResult;
namespace protocol = uml::gpu::protocol;

constexpr std::uint32_t requestQueue = 0;
constexpr std::uint32_t eventQueue = 1;
constexpr std::uint16_t queueDepth = 8;
constexpr std::uint64_t negotiated = protocol::protocolFeatureReplyAck |
    protocol::protocolFeatureConfig;
constexpr std::uint64_t negotiatedInband = negotiated |
    protocol::protocolFeatureInbandNotifications |
    protocol::protocolFeatureSlaveRequest;

/* The device's tag for a request is the one byte the guest sends; its
 * response to the request is that byte and the next. */
[[nodiscard]] std::vector<std::uint8_t> responseTo(std::uint8_t tag)
{
    return {tag, static_cast<std::uint8_t>(tag + 1U)};
}

/* A device with a request queue and an event queue, like a codec. The request
 * queue defers every request until the test finishes it; the event queue
 * answers a buffer with the oldest event the test raised, and keeps the buffer
 * while there is none. */
class HoldingDevice final : public VhostUserDevice {
public:
    struct Options {
        bool keepsRequests {};
        bool answersKicksFirst {};
    };

    explicit HoldingDevice(Options options) : options_(options)
    {
        wake_ = makePipe();
        require(::fcntl(wake_.readEnd.get(), F_SETFL, O_NONBLOCK) == 0);
    }

    [[nodiscard]] const char* name() const noexcept override { return "holding"; }
    [[nodiscard]] std::uint64_t features() const noexcept override { return 0; }
    [[nodiscard]] std::size_t queueCount() const noexcept override { return 2; }
    [[nodiscard]] std::vector<std::uint8_t> configuration() const override
    {
        return {0, 0, 0, 0};
    }
    void writeConfiguration(
        std::uint32_t, std::uint32_t, std::span<const std::uint8_t>) override {}
    [[nodiscard]] bool keepsDeferredRequestsWhileDisabled() const noexcept override
    {
        return options_.keepsRequests;
    }
    [[nodiscard]] bool keepsRequestsOnDisabledRing() const noexcept override
    {
        return options_.keepsRequests;
    }
    [[nodiscard]] bool answersKicksBeforeRunning() const noexcept override
    {
        return options_.answersKicksFirst;
    }

    [[nodiscard]] VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t) override
    {
        const std::lock_guard lock {mutex_};
        ++runs_[index];
        if (index == requestQueue) {
            held_.push_back({requestId, request.empty() ? std::uint8_t {} : request[0]});
            return {VhostUserQueueAction::defer, {}};
        }
        if (events_.empty())
            return {VhostUserQueueAction::retry, {}};
        const auto event = events_.front();
        events_.pop_front();
        return {VhostUserQueueAction::complete, uml::virtio::toBytes(event)};
    }
    [[nodiscard]] bool takeQueueCompletion(VhostUserQueueCompletion& completion) override
    {
        const std::lock_guard lock {mutex_};
        if (finished_.empty())
            return false;
        completion = std::move(finished_.front());
        finished_.pop_front();
        ++taken_;
        return true;
    }
    void reset() noexcept override
    {
        const std::lock_guard lock {mutex_};
        ++resets_;
        held_.clear();
        finished_.clear();
        events_.clear();
    }
    [[nodiscard]] int eventDescriptor() const noexcept override
    {
        return wake_.readEnd.get();
    }
    void drainEventSource() override
    {
        std::array<char, 16> bytes {};
        while (::read(wake_.readEnd.get(), bytes.data(), bytes.size()) > 0) {}
        const std::lock_guard lock {mutex_};
        ++drained_;
    }

    /* The device finishes the request the guest tagged `tag`. */
    void finish(std::uint8_t tag)
    {
        {
            const std::lock_guard lock {mutex_};
            const auto held = std::find_if(
                held_.begin(), held_.end(), [tag](const Held& request) {
                    return request.tag == tag;
                });
            require(held != held_.end(), "the device holds no such request");
            finished_.push_back({held->requestId, responseTo(tag)});
            held_.erase(held);
        }
        wake();
    }
    /* The device raises an event for the event queue's next buffer. */
    void raise(std::uint64_t event)
    {
        {
            const std::lock_guard lock {mutex_};
            events_.push_back(event);
        }
        wake();
    }

    [[nodiscard]] unsigned runs(std::uint32_t queue) const
    {
        const std::lock_guard lock {mutex_};
        return runs_[queue];
    }
    [[nodiscard]] unsigned resets() const
    {
        const std::lock_guard lock {mutex_};
        return resets_;
    }
    [[nodiscard]] std::size_t held() const
    {
        const std::lock_guard lock {mutex_};
        return held_.size();
    }
    [[nodiscard]] unsigned taken() const
    {
        const std::lock_guard lock {mutex_};
        return taken_;
    }
    [[nodiscard]] unsigned drained() const
    {
        const std::lock_guard lock {mutex_};
        return drained_;
    }

private:
    struct Held {
        std::uint64_t requestId {};
        std::uint8_t tag {};
    };

    void wake() const
    {
        require(::write(wake_.writeEnd.get(), "x", 1) == 1);
    }

    Options options_;
    Pipe wake_;
    mutable std::mutex mutex_;
    std::array<unsigned, 2> runs_ {};
    unsigned resets_ {};
    unsigned taken_ {};
    unsigned drained_ {};
    std::deque<Held> held_;
    std::deque<VhostUserQueueCompletion> finished_;
    std::deque<std::uint64_t> events_;
};

/* Where a queue's rings lie in the ring's one page: SplitRing places a single
 * queue, so the second takes the upper half. The buffers follow. */
struct Layout {
    std::size_t descriptors;
    std::size_t available;
    std::size_t used;
};
constexpr Layout eventLayout {0x000, 0x100, 0x200};
constexpr Layout requestLayout {0x400, 0x500, 0x600};
constexpr std::size_t eventBufferOffset = 0x300;
constexpr std::size_t requestBufferOffset = 0x700;
constexpr std::size_t responseBufferOffset = 0x780;
constexpr std::size_t bufferStride = 0x10;
constexpr std::uint32_t responseCapacity = 8;

/* One queue of the device in `ring`'s page, driven the way a front end does:
 * placed and enabled, given a call pipe and, when asked, a kick pipe, and fed
 * buffers by the test. */
class TestQueue {
public:
    TestQueue(const SplitRing& ring, std::uint32_t index, Layout layout)
        : ring_(ring), index_(index), layout_(layout) {}

    void start(int socket, bool enable, bool withKickPipe)
    {
        const protocol::VringState size {.index = index_, .value = queueDepth};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringNum, bytesOf(size));
        const protocol::VringAddress address {
            .index = index_,
            .flags = 0,
            .descriptor = SplitRing::userBase + layout_.descriptors,
            .used = SplitRing::userBase + layout_.used,
            .available = SplitRing::userBase + layout_.available,
            .log = 0,
        };
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringAddr, bytesOf(address));
        if (enable)
            this->enable(socket, true);
        attachChannels(socket, withKickPipe);
    }
    /* A new call pipe, and a kick pipe when asked. The pipes the server had
     * stay open here: closing the write end of its kick pipe would end the
     * connection before the new one reached it. */
    void attachChannels(int socket, bool withKickPipe)
    {
        retired_.push_back(std::move(call_));
        retired_.push_back(std::move(kick_));
        call_ = makePipe();
        const std::uint64_t queue = index_;
        const std::array callDescriptors {call_.writeEnd.get()};
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setVringCall,
            bytesOf(queue),
            callDescriptors);
        if (!withKickPipe)
            return;
        kick_ = makePipe();
        const std::array kickDescriptors {kick_.readEnd.get()};
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setVringKick,
            bytesOf(queue),
            kickDescriptors);
    }
    void enable(int socket, bool on) const
    {
        const protocol::VringState state {.index = index_, .value = on ? 1U : 0U};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringEnable, bytesOf(state));
    }
    void writeKickPipe() const { writeKick(kick_); }
    /* An in-band kick. Whatever the kick asks for, it returns once the server
     * has handled it, which a configuration read, answered after it, shows;
     * a reply the kick should not have got would come before that one. */
    void kick(int socket, bool needReply) const
    {
        const protocol::VringState state {.index = index_, .value = 0};
        sendRequest(
            socket, protocol::VhostRequest::vringKick, bytesOf(state), {}, needReply);
        if (needReply)
            readAcknowledgement(socket, protocol::VhostRequest::vringKick);
        (void)readConfig(socket, 0, 1);
    }

    /* The guest queues the request it tags `tag`: one byte for the device and
     * room for its answer. */
    void postRequest(std::uint8_t tag)
    {
        const auto slot = posted_;
        require(slot < 4, "the buffer area holds four requests");
        const auto requestOffset = requestBufferOffset + slot * bufferStride;
        const auto responseOffset = responseBufferOffset + slot * bufferStride;
        ring_.memory()[requestOffset] = tag;
        const auto head = static_cast<std::uint16_t>(2U * slot);
        const auto chain = requestChain(
            requestOffset, 1, responseOffset, responseCapacity, head);
        publish(chain, head);
    }
    /* The guest queues a buffer for the device's next event. */
    void postEventBuffer()
    {
        const std::array<protocol::VirtqueueDescriptor, 1> buffer {{
            {.address = SplitRing::guestBase + eventBufferOffset + posted_ * bufferStride,
             .length = responseCapacity,
             .flags = protocol::descriptorWrite,
             .next = 0}}};
        publish(buffer, static_cast<std::uint16_t>(posted_));
    }

    [[nodiscard]] std::uint16_t usedIndex() const
    {
        return std::atomic_ref<std::uint16_t>(
                   *reinterpret_cast<std::uint16_t*>(
                       ring_.memory().data() + layout_.used + sizeof(std::uint16_t)))
            .load(std::memory_order_acquire);
    }
    [[nodiscard]] protocol::VirtqueueUsedElement used(std::size_t index) const
    {
        protocol::VirtqueueUsedElement element {};
        std::memcpy(
            &element,
            ring_.memory().data() + layout_.used + protocol::splitRingHeaderSize +
                index * sizeof(element),
            sizeof(element));
        return element;
    }
    /* The response the device wrote for request number `slot` of this queue
     * (counting from zero). */
    [[nodiscard]] std::vector<std::uint8_t> responseAt(std::size_t slot, std::size_t size) const
    {
        const auto* bytes = ring_.memory().data() + responseBufferOffset + slot * bufferStride;
        return {bytes, bytes + size};
    }
    [[nodiscard]] std::uint64_t eventAt(std::size_t slot) const
    {
        std::uint64_t event {};
        std::memcpy(
            &event, ring_.memory().data() + eventBufferOffset + slot * bufferStride, sizeof(event));
        return event;
    }

    /* Whether the guest has been called since it last was. */
    [[nodiscard]] bool called(int timeoutMilliseconds = 100) const
    {
        pollfd call {.fd = call_.readEnd.get(), .events = POLLIN};
        if (::poll(&call, 1, timeoutMilliseconds) != 1)
            return false;
        std::uint64_t count {};
        readExact(call_.readEnd.get(), &count, sizeof(count));
        return true;
    }
    void awaitCall() const
    {
        uml::gpu::testing::awaitCall(call_.readEnd.get());
    }

private:
    void publish(
        std::span<const protocol::VirtqueueDescriptor> chain, std::uint16_t head)
    {
        auto* memory = ring_.memory().data();
        std::memcpy(
            memory + layout_.descriptors + head * sizeof(protocol::VirtqueueDescriptor),
            chain.data(),
            chain.size_bytes());
        std::memcpy(
            memory + layout_.available + protocol::splitRingHeaderSize +
                (posted_ % queueDepth) * sizeof(std::uint16_t),
            &head,
            sizeof(head));
        ++posted_;
        std::atomic_ref<std::uint16_t>(
            *reinterpret_cast<std::uint16_t*>(
                memory + layout_.available + sizeof(std::uint16_t)))
            .store(static_cast<std::uint16_t>(posted_), std::memory_order_release);
    }

    const SplitRing& ring_;
    std::uint32_t index_;
    Layout layout_;
    std::size_t posted_ {};
    Pipe call_;
    Pipe kick_;
    std::vector<Pipe> retired_;
};

/* The state of the guest's two queues once the device holds requests: both
 * started and enabled, the request queue holding `requests` requests the
 * device deferred and the event queue holding one buffer. */
struct HeldSession {
    HoldingDevice& device;
    SplitRing& ring;
    TestQueue& requests;
    TestQueue& events;
    int socket;
};

/* Runs `body` on a connection whose guest has posted `postedRequests`
 * requests (tags 1, 2, ...) and one event buffer, all of which the device
 * holds, and then suspended: both rings disabled. */
void withSuspendedGuest(
    HoldingDevice::Options options,
    unsigned postedRequests,
    const std::function<void(const HeldSession&)>& body)
{
    auto device = std::make_unique<HoldingDevice>(options);
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};

    withServerConnection(server, [&](int socket) {
        negotiateProtocolFeatures(socket, negotiated);
        configureMemory(socket, ring.descriptor());
        requests.start(socket, true, true);
        events.start(socket, true, true);
        for (unsigned tag = 1; tag <= postedRequests; ++tag)
            requests.postRequest(static_cast<std::uint8_t>(tag));
        events.postEventBuffer();
        requests.writeKickPipe();
        events.writeKickPipe();
        awaitCondition([&] {
            return recorder->runs(requestQueue) >= postedRequests &&
                recorder->runs(eventQueue) >= 1;
        });
        require(recorder->held() == postedRequests);
        requests.enable(socket, false);
        events.enable(socket, false);
        body({*recorder, ring, requests, events, socket});
    });
}

/* A request the device completes while the guest is suspended, and an event
 * it raises, are written to the rings at each ring's enable, in whichever
 * order the rings are enabled, and the suspend resets nothing. The request
 * completions keep the order the device finished them in. */
void completionsAndEventsWaitForTheEnable(bool enableEventsFirst)
{
    withSuspendedGuest({true, false}, 2, [&](const HeldSession& session) {
        auto& device = session.device;
        auto& requests = session.requests;
        auto& events = session.events;
        const int socket = session.socket;
        const auto resets = device.resets();
        const auto taken = device.taken();
        const auto drained = device.drained();
        device.finish(2);
        device.finish(1);
        device.raise(0x1234);
        awaitCondition([&] {
            return device.taken() == taken + 2 && device.drained() > drained;
        });
        // The server has handled the wake once a message after it is answered.
        (void)readConfig(socket, 0, 1);
        require(requests.usedIndex() == 0 && events.usedIndex() == 0,
                "a used ring was written while its ring was disabled");
        require(!requests.called() && !events.called(),
                "the guest was called while its rings were disabled");

        const auto resumeRequests = [&] {
            requests.enable(socket, true);
            require(requests.usedIndex() == 2, "the held completions were not written");
            // Request 2 was posted second (slot 1, head 2) and finished first.
            require(requests.used(0).id == 2 && requests.used(0).length == 2);
            require(requests.used(1).id == 0 && requests.used(1).length == 2);
            require(requests.responseAt(1, 2) == responseTo(2));
            require(requests.responseAt(0, 2) == responseTo(1));
            requests.awaitCall();
            require(!requests.called(), "the guest was called more than once");
        };
        const auto resumeEvents = [&] {
            events.enable(socket, true);
            require(events.usedIndex() == 1, "the event did not reach the buffer");
            require(events.used(0).id == 0 && events.used(0).length == sizeof(std::uint64_t));
            require(events.eventAt(0) == 0x1234);
            events.awaitCall();
            require(!events.called(), "the guest was called more than once");
        };
        if (enableEventsFirst) {
            resumeEvents();
            require(requests.usedIndex() == 0, "the request ring was written before its enable");
            resumeRequests();
        } else {
            resumeRequests();
            require(events.usedIndex() == 0, "the event ring was written before its enable");
            resumeEvents();
        }
        require(device.resets() == resets, "the suspend and resume reset the device");
        require(device.held() == 0);
    });
}

/* A completion that arrives while only one of the rings is disabled is
 * written at once to a ring that is enabled. */
void completionOnAnEnabledRingIsWrittenAtOnce()
{
    withSuspendedGuest({true, false}, 1, [&](const HeldSession& session) {
        auto& device = session.device;
        auto& requests = session.requests;
        auto& events = session.events;
        const int socket = session.socket;
        requests.enable(socket, true);
        const auto resets = device.resets();
        device.finish(1);
        awaitCondition([&] { return requests.usedIndex() == 1; });
        requests.awaitCall();
        require(requests.responseAt(0, 2) == responseTo(1));
        require(events.usedIndex() == 0);
        require(device.resets() == resets);
    });
}

/* A kick across the suspend, with and without the acknowledgement the front
 * end may ask for. A kick for a ring whose enable has not arrived is accepted
 * and does nothing; a kick for an enabled ring runs it while a later ring is
 * still disabled; neither resets the device, and the request a disabled
 * ring's kick announced runs at the ring's enable. */
void kicksAcrossASuspend(bool needReply, bool answersKicksFirst)
{
    auto device = std::make_unique<HoldingDevice>(
        HoldingDevice::Options {true, answersKicksFirst});
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};

    withServerConnection(server, [&](int socket) {
        auto backend = openBackendChannel(socket, negotiatedInband);
        configureMemory(socket, ring.descriptor());
        requests.start(socket, true, false);
        events.start(socket, true, false);
        requests.postRequest(1);
        events.postEventBuffer();
        events.kick(socket, needReply);
        requests.kick(socket, needReply);
        require(recorder->runs(requestQueue) == 1 && recorder->runs(eventQueue) == 1);
        require(recorder->held() == 1);
        const auto resets = recorder->resets();

        // The guest suspends: its rings are disabled, in order.
        requests.enable(socket, false);
        events.enable(socket, false);

        // The guest resumes: its flag clears and it queues and kicks before
        // the host has handled its enable.
        requests.postRequest(2);
        requests.kick(socket, needReply);
        events.kick(socket, needReply);
        require(recorder->runs(requestQueue) == 1 && recorder->runs(eventQueue) == 1,
                "a kick for a disabled ring ran the device");
        require(recorder->held() == 1 && recorder->resets() == resets);
        require(requests.usedIndex() == 0, "a disabled ring's request was consumed and dropped");

        // The request ring is enabled, and runs the request kicked before.
        requests.enable(socket, true);
        require(recorder->runs(requestQueue) == 2 && recorder->held() == 2);

        // A kick for the enabled request ring, with the event ring disabled.
        requests.postRequest(3);
        requests.kick(socket, needReply);
        require(recorder->runs(requestQueue) == 3 && recorder->held() == 3,
                "a kick for an enabled ring was not run beside a disabled one");
        events.kick(socket, needReply);
        require(recorder->runs(eventQueue) == 1);
        require(recorder->resets() == resets, "a kick reset the device");

        // The event ring is enabled and takes the event raised meanwhile.
        const auto drained = recorder->drained();
        recorder->raise(0x77);
        awaitCondition([&] { return recorder->drained() > drained; });
        (void)readConfig(socket, 0, 1);
        require(events.usedIndex() == 0);
        events.enable(socket, true);
        require(events.usedIndex() == 1 && events.eventAt(0) == 0x77);
        events.awaitCall();

        // Every request is answered, each to its own buffer.
        recorder->finish(3);
        recorder->finish(1);
        recorder->finish(2);
        awaitCondition([&] { return requests.usedIndex() == 3; });
        require(requests.used(0).id == 4 && requests.used(1).id == 0 && requests.used(2).id == 2);
        require(recorder->resets() == resets && recorder->held() == 0);
    });
}

/* The messages that change what a held request refers to, or end the
 * driver, reset the device and drop its held requests and completions;
 * the connection goes on. */
void stateMessagesRetireHeldRequests()
{
    struct Message {
        const char* name;
        std::function<void(const HeldSession&)> send;
    };
    const std::vector<Message> messages {
        {"GET_FEATURES", [](const HeldSession& session) { (void)getFeatures(session.socket); }},
        {"SET_FEATURES",
         [](const HeldSession& session) {
             const std::uint64_t features = protocol::vhostFeatureProtocol;
             sendRequestAndReadAcknowledgement(
                 session.socket, protocol::VhostRequest::setFeatures, bytesOf(features));
         }},
        {"SET_PROTOCOL_FEATURES",
         [](const HeldSession& session) {
             const std::uint64_t features = negotiated;
             sendRequestAndReadAcknowledgement(
                 session.socket, protocol::VhostRequest::setProtocolFeatures, bytesOf(features));
         }},
        {"SET_MEM_TABLE",
         [](const HeldSession& session) {
             configureMemory(session.socket, session.ring.descriptor());
         }},
        {"SET_VRING_NUM",
         [](const HeldSession& session) {
             const protocol::VringState size {.index = requestQueue, .value = queueDepth};
             sendRequestAndReadAcknowledgement(
                 session.socket, protocol::VhostRequest::setVringNum, bytesOf(size));
         }},
        {"SET_VRING_ADDR",
         [](const HeldSession& session) {
             const protocol::VringAddress address {
                 .index = requestQueue,
                 .flags = 0,
                 .descriptor = SplitRing::userBase + requestLayout.descriptors,
                 .used = SplitRing::userBase + requestLayout.used,
                 .available = SplitRing::userBase + requestLayout.available,
                 .log = 0,
             };
             sendRequestAndReadAcknowledgement(
                 session.socket, protocol::VhostRequest::setVringAddr, bytesOf(address));
         }},
        {"SET_VRING_BASE",
         [](const HeldSession& session) {
             const protocol::VringState base {.index = requestQueue, .value = 0};
             sendRequestAndReadAcknowledgement(
                 session.socket, protocol::VhostRequest::setVringBase, bytesOf(base));
         }},
        {"GET_VRING_BASE",
         [](const HeldSession& session) {
             const protocol::VringState base {.index = requestQueue, .value = 0};
             sendRequest(session.socket, protocol::VhostRequest::getVringBase, bytesOf(base), {}, false);
             (void)readValueReply<protocol::VringState>(
                 session.socket, protocol::VhostRequest::getVringBase);
         }},
        {"RESET_OWNER",
         [](const HeldSession& session) {
             sendRequestAndReadAcknowledgement(
                 session.socket, protocol::VhostRequest::resetOwner, {});
         }},
    };
    for (const auto& message : messages) {
        withSuspendedGuest({true, false}, 2, [&](const HeldSession& session) {
            const auto& device = session.device;
            // A completion held for the enable goes with the requests.
            const auto taken = device.taken();
            session.device.finish(1);
            awaitCondition([&] { return device.taken() == taken + 1; });
            (void)readConfig(session.socket, 0, 1);
            const auto resets = device.resets();
            message.send(session);
            const std::string name = message.name;
            require(device.resets() == resets + 1, name + " did not reset the device");
            require(device.held() == 0, name + " left a request held");
            (void)getFeatures(session.socket);
        });
    }
}

/* The messages a suspend can race with, or that touch neither the requests
 * nor the rings' layout, leave the device alone, and the held request is
 * still answered at the enable. */
void otherMessagesLeaveHeldRequestsAlone()
{
    withSuspendedGuest({true, false}, 1, [&](const HeldSession& session) {
        auto& device = session.device;
        auto& requests = session.requests;
        auto& events = session.events;
        const int socket = session.socket;
        const auto resets = device.resets();
        const auto unchanged = [&](const char* name) {
            require(device.resets() == resets, std::string {name} + " reset the device");
            require(device.held() == 1, std::string {name} + " dropped a request");
        };

        (void)readConfig(socket, 0, 4);
        unchanged("GET_CONFIG");
        const std::array<std::uint8_t, 1> contents {0};
        const protocol::ConfigHeader write {.offset = 0, .size = 1, .flags = 0};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setConfig, configPayload(write, contents));
        unchanged("SET_CONFIG");
        sendRequest(socket, protocol::VhostRequest::getProtocolFeatures, {}, {}, false);
        (void)readValueReply<std::uint64_t>(socket, protocol::VhostRequest::getProtocolFeatures);
        unchanged("GET_PROTOCOL_FEATURES");
        sendRequest(socket, protocol::VhostRequest::getQueueNum, {}, {}, false);
        (void)readValueReply<std::uint64_t>(socket, protocol::VhostRequest::getQueueNum);
        unchanged("GET_QUEUE_NUM");
        sendRequestAndReadAcknowledgement(socket, protocol::VhostRequest::setOwner, {});
        unchanged("SET_OWNER");
        requests.attachChannels(socket, true);
        events.attachChannels(socket, true);
        unchanged("SET_VRING_CALL and SET_VRING_KICK");

        requests.enable(socket, true);
        device.finish(1);
        awaitCondition([&] { return requests.usedIndex() == 1; });
        require(requests.responseAt(0, 2) == responseTo(1));
        require(device.resets() == resets);
    });
}

/* A driver's teardown disables its rings in reverse order, asks for the
 * features, and gives its rings up. The request is still held through the
 * disables; the features request ends it, and the calls and kicks the driver
 * then withdraws are accepted. */
void teardownRetiresHeldRequestsAtTheFeaturesRequest()
{
    withSuspendedGuest({true, false}, 1, [&](const HeldSession& session) {
        auto& device = session.device;
        auto& requests = session.requests;
        auto& events = session.events;
        const int socket = session.socket;
        // The suspend already disabled both rings; a teardown disables them
        // again, in the other order.
        events.enable(socket, false);
        requests.enable(socket, false);
        require(device.held() == 1);
        const auto resets = device.resets();
        (void)getFeatures(socket);
        require(device.resets() == resets + 1 && device.held() == 0,
                "the features request after the disables did not retire the request");
        for (const std::uint64_t queue : {std::uint64_t {eventQueue}, std::uint64_t {requestQueue}}) {
            const std::uint64_t withdrawn = queue | protocol::vhostVringNoDescriptor;
            sendRequestAndReadAcknowledgement(
                socket, protocol::VhostRequest::setVringCall, bytesOf(withdrawn));
            sendRequestAndReadAcknowledgement(
                socket, protocol::VhostRequest::setVringKick, bytesOf(withdrawn));
        }
        require(device.resets() == resets + 1);
        (void)readConfig(socket, 0, 1);
    });
}

/* A reset drops the completion a ring was holding for its enable: nothing is
 * written when the ring is enabled afterwards. */
void resetDropsAHeldCompletion()
{
    withSuspendedGuest({true, false}, 1, [&](const HeldSession& session) {
        auto& device = session.device;
        auto& requests = session.requests;
        auto& events = session.events;
        const int socket = session.socket;
        const auto taken = device.taken();
        device.finish(1);
        awaitCondition([&] { return device.taken() == taken + 1; });
        (void)readConfig(socket, 0, 1);
        (void)getFeatures(socket);
        requests.enable(socket, true);
        events.enable(socket, true);
        (void)readConfig(socket, 0, 1);
        require(requests.usedIndex() == 0, "a completion of a reset device was written");
        require(!requests.called(), "the guest was called for a completion that was dropped");
    });
}

/* A device that does not ask to keep its requests is reset by the first
 * disable of a ring while one is held, as before: the request and its answer
 * are gone, and a request posted to the disabled ring is consumed and dropped
 * rather than kept. */
void deviceThatDoesNotOptInIsResetAtTheDisable()
{
    auto device = std::make_unique<HoldingDevice>(HoldingDevice::Options {false, false});
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};

    withServerConnection(server, [&](int socket) {
        negotiateProtocolFeatures(socket, negotiated);
        configureMemory(socket, ring.descriptor());
        requests.start(socket, true, true);
        events.start(socket, true, true);
        requests.postRequest(1);
        requests.writeKickPipe();
        awaitCondition([&] { return recorder->runs(requestQueue) == 1; });
        require(recorder->held() == 1);
        const auto resets = recorder->resets();

        requests.enable(socket, false);
        require(recorder->resets() == resets + 1, "the disable did not reset a device that holds a request");
        require(recorder->held() == 0);

        requests.postRequest(2);
        requests.writeKickPipe();
        awaitCondition([&] { return requests.usedIndex() == 1; });
        require(requests.used(0).id == 2 && requests.used(0).length == 0,
                "a request on a disabled ring was kept");
        require(recorder->runs(requestQueue) == 1);

        // With nothing held, a disable resets nothing further.
        events.enable(socket, false);
        require(recorder->resets() == resets + 1);
    });
}

} // namespace

/* With a case's name as its argument, runs that case alone. */
int main(int argc, char** argv)
{
    struct Case {
        const char* name;
        std::function<void()> run;
    };
    std::vector<Case> cases;
    for (const bool eventsFirst : {false, true}) {
        cases.push_back({eventsFirst ? "completions-events-first" : "completions-requests-first",
                         [=] { completionsAndEventsWaitForTheEnable(eventsFirst); }});
    }
    cases.push_back({"completion-on-enabled-ring", completionOnAnEnabledRingIsWrittenAtOnce});
    for (const bool needReply : {false, true}) {
        for (const bool answersFirst : {false, true}) {
            cases.push_back({needReply ? (answersFirst ? "kicks-reply-answered-first" : "kicks-reply")
                                       : (answersFirst ? "kicks-answered-first" : "kicks"),
                             [=] { kicksAcrossASuspend(needReply, answersFirst); }});
        }
    }
    cases.push_back({"state-messages", stateMessagesRetireHeldRequests});
    cases.push_back({"other-messages", otherMessagesLeaveHeldRequestsAlone});
    cases.push_back({"teardown", teardownRetiresHeldRequestsAtTheFeaturesRequest});
    cases.push_back({"reset-drops-completion", resetDropsAHeldCompletion});
    cases.push_back({"no-opt-in", deviceThatDoesNotOptInIsResetAtTheDisable});
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
    std::puts("vhost-user held requests ok");
    return 0;
}
