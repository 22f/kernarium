// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/descriptor.hpp"
#include "protocol.hpp"
#include "vhost_user_device.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>

namespace uml::input::virtio {

enum class DeviceKind {
    touchscreen,
    /* The side keys: Power, the volume keys, Back and Home. */
    keyboard,
    /* The hardware keyboard, mouse and pointer exist from boot and are
     * plugged and unplugged by the host (EventQueue::Attachment::pluggable). */
    hardwareKeyboard,
    mouse,
    pointer,
};

/* Whether the host plugs and unplugs a device of this kind. */
[[nodiscard]] constexpr bool pluggable(DeviceKind kind) noexcept
{
    return kind == DeviceKind::hardwareKeyboard ||
        kind == DeviceKind::mouse || kind == DeviceKind::pointer;
}

/* What the guest tells the host on the status queue, as the host sees it. */
struct StatusEvent final {
    enum class Kind : std::uint8_t {
        /* The guest wrote an LED: `led` (protocol::ledCapsLock and the
         * like), lit when `on`. */
        led,
        /* The guest's driver registered (`on`) or unregistered its input
         * device (kernel/common/input/virtio_input_uml.h). `lostEvents`:
         * events that waited for the registration were dropped, so the
         * host's idea of what it sent is out of date. */
        registration,
    };

    Kind kind {Kind::led};
    std::uint16_t led {};
    bool on {};
    bool lostEvents {};
};

/* The host's side of one device: the events it sends the guest, whether the
 * device is plugged in, and what the guest has told it. Shared between the
 * host's thread and the device's transport thread.
 *
 * A fixed device is always plugged in and its events go straight to the
 * guest. A pluggable device starts unplugged:
 *  - unplugged, the device reports no events and the queue drops what it is
 *    sent;
 *  - plugged, a configuration change is pending (once per plug or unplug) and
 *    the events wait, at most `heldEventLimit` of them, until the guest's
 *    driver has registered its input device (the registration notice, see
 *    StatusEvent) and then go to it in order. A guest that never registers,
 *    as one without the kernel's hot-plug support, gets nothing: when the
 *    wait outgrows the limit everything held is dropped, and so is what comes
 *    after, until the registration, which then reports `lostEvents`. */
class EventQueue final {
public:
    enum class Attachment : std::uint8_t {
        fixed,
        pluggable,
    };
    using StatusSink = std::function<void(const StatusEvent&)>;

    static constexpr std::size_t heldEventLimit = 256;

    explicit EventQueue(Attachment attachment = Attachment::fixed);

    EventQueue(const EventQueue&) = delete;
    EventQueue& operator=(const EventQueue&) = delete;

    /* Host side. */
    void enqueue(std::span<const protocol::InputEvent> events) noexcept;
    [[nodiscard]] bool connected() const noexcept;
    /* Whether the guest has told the host its input device exists. A fixed
     * device is always registered. */
    [[nodiscard]] bool registered() const noexcept;
    /* Plugs or unplugs a pluggable device and raises a configuration change;
     * nothing happens, and false is returned, when it is in that state
     * already or is fixed. Unplugging drops what waits for a registration. */
    bool setConnected(bool connected) noexcept;
    /* Sends `finalEvents` and unplugs as one step, so nothing sent by another
     * thread comes between: the releases a host owes the guest before the
     * device goes. */
    void disconnect(
        std::span<const protocol::InputEvent> finalEvents) noexcept;
    /* Receives what the guest writes to the status queue, on the transport
     * thread; it must return promptly. Replaces any earlier sink, and a null
     * sink clears it; once this returns no call to the old one is running. */
    void setStatusSink(StatusSink sink);

    /* Device side. */
    [[nodiscard]] bool take(protocol::InputEvent& event) noexcept;
    [[nodiscard]] int descriptor() const noexcept;
    void drain();
    /* A status event from the guest. */
    void guestStatus(const protocol::InputEvent& event) noexcept;
    /* A configuration change is waiting to be sent, is taken for sending and
     * is acknowledged, as the transport asks a device to. */
    [[nodiscard]] bool configChangePending() const noexcept;
    [[nodiscard]] bool claimConfigChange() noexcept;
    void clearConfigChange() noexcept;
    /* The guest reset the device: it has no events queued and has registered
     * nothing. */
    void reset() noexcept;
    void clear() noexcept;

private:
    void wake() noexcept;
    void queueLocked(std::span<const protocol::InputEvent> events) noexcept;
    void holdLocked(std::span<const protocol::InputEvent> events) noexcept;
    bool setConnectedLocked(bool connected) noexcept;

    uml::platform::WakeSocket wake_;
    mutable std::mutex mutex_;
    std::deque<protocol::InputEvent> events_;
    std::deque<protocol::InputEvent> held_;
    bool reportedOverflow_ {};
    bool reportedHoldOverflow_ {};
    const bool pluggable_;
    bool connected_;
    bool registered_;
    bool heldLost_ {};
    std::uint64_t configGeneration_ {};
    std::uint64_t configClaimed_ {};
    std::uint64_t configDelivered_ {};
    /* The sink runs with sinkMutex_ held, which is what makes replacing it
     * wait for a call in progress. */
    std::mutex sinkMutex_;
    StatusSink sink_;
};

class VirtioInputDevice final : public uml::virtio::VhostUserDevice {
public:
    VirtioInputDevice(DeviceKind kind, std::shared_ptr<EventQueue> events);

    [[nodiscard]] const char* name() const noexcept override;
    [[nodiscard]] std::uint64_t features() const noexcept override;
    [[nodiscard]] std::size_t queueCount() const noexcept override;
    [[nodiscard]] std::vector<std::uint8_t> configuration() const override;
    void writeConfiguration(
        std::uint32_t offset,
        std::uint32_t flags,
        std::span<const std::uint8_t> bytes) override;
    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override;
    void reset() noexcept override;
    [[nodiscard]] int eventDescriptor() const noexcept override;
    void drainEventSource() override;
    [[nodiscard]] bool configChangePending() const noexcept override;
    [[nodiscard]] bool claimConfigChange() noexcept override;
    void clearConfigChange() noexcept override;

private:
    [[nodiscard]] protocol::InputConfig selectedConfiguration() const;

    DeviceKind kind_;
    std::shared_ptr<EventQueue> events_;
    std::uint8_t select_ {};
    std::uint8_t subselect_ {};
    bool reportedDelivery_ {};
};

} // namespace uml::input::virtio
