// SPDX-License-Identifier: GPL-2.0-or-later

#include "hardware_input.hpp"
#include "hardware_reports.hpp"
#include "hid_keyboard.hpp"
#include "require.hpp"
#include "wire_bytes.hpp"

#include <array>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <vector>

namespace {

using uml::input::virtio::EventQueue;
using uml::input::virtio::HardwareInput;
using uml::input::virtio::HardwareKeyboard;
using uml::input::virtio::HardwareReport;
using uml::input::virtio::HardwareStatus;
using uml::input::virtio::Mouse;
using uml::input::virtio::Pointer;
using uml::input::virtio::VirtioInputDevice;
namespace protocol = uml::input::virtio::protocol;
using uml::input::virtio::DeviceKind;
using uml::testing::require;

using Event = protocol::InputEvent;
using Events = std::vector<Event>;

constexpr Event event(
    std::uint16_t type,
    std::uint16_t code,
    std::int32_t value) noexcept
{
    return {type, code, value};
}

constexpr Event sync() noexcept
{
    return event(protocol::eventSynchronize, protocol::synchronizeReport, 0);
}

bool sameEvents(const Events& actual, std::initializer_list<Event> expected)
{
    if (actual.size() != expected.size())
        return false;
    std::size_t index = 0;
    for (const auto& one : expected) {
        const auto& got = actual[index++];
        if (got.type != one.type || got.code != one.code ||
            got.value != one.value)
            return false;
    }
    return true;
}

bool sameEvents(const HardwareReport& report, std::initializer_list<Event> expected)
{
    return sameEvents(report.events, expected);
}

// ---- The report builders.

constexpr auto key = protocol::eventKey;

void aKeyGoesDownOnceAndUpOnce()
{
    HardwareKeyboard keyboard;
    const auto down = keyboard.key(30, true);
    require(sameEvents(down, {event(key, 30, 1), sync()}));
    require(down.wakes);
    require(keyboard.holds());
    // A second press of a key that is down is not a repeat.
    require(keyboard.key(30, true).empty());
    const auto up = keyboard.key(30, false);
    require(sameEvents(up, {event(key, 30, 0), sync()}));
    require(!up.wakes);
    require(!keyboard.holds());
    // A release of a key that is not down is nothing.
    require(keyboard.key(30, false).empty());
    require(keyboard.key(48, false).empty());
}

void aKeyTheTableCannotProduceIsDropped()
{
    HardwareKeyboard keyboard;
    for (const std::uint16_t code : {0, 240, 195, 255, 256, 0x110, 0xffff}) {
        require(keyboard.key(code, true).empty());
        require(!keyboard.holds());
    }
}

void releaseAllLetsGoOfEveryKeyInOneReport()
{
    HardwareKeyboard keyboard;
    require(keyboard.releaseAll().empty());
    (void)keyboard.key(125, true); // Meta
    (void)keyboard.key(30, true);
    (void)keyboard.key(194, true);
    (void)keyboard.key(30, true);
    const auto release = keyboard.releaseAll();
    require(sameEvents(release, {
        event(key, 30, 0), event(key, 125, 0), event(key, 194, 0), sync()}));
    require(!release.wakes && !keyboard.holds());
    require(keyboard.releaseAll().empty());
    // What was released can go down again.
    require(keyboard.key(30, true).wakes);

    keyboard.reset();
    require(!keyboard.holds() && keyboard.key(30, false).empty());
}

void aMouseMovesAndScrollsAndPressesButtons()
{
    Mouse mouse;
    require(mouse.update(0, 0, 0, 0, 0).empty());
    const auto moved = mouse.update(5, -3, 0, 0, 0);
    require(sameEvents(moved, {
        event(protocol::eventRelative, protocol::relativeX, 5),
        event(protocol::eventRelative, protocol::relativeY, -3), sync()}));
    require(!moved.wakes);
    // One axis moves alone.
    require(sameEvents(mouse.update(0, 7, 0, 0, 0), {
        event(protocol::eventRelative, protocol::relativeY, 7), sync()}));

    const auto press = mouse.update(0, 0, 1U, 0, 0);
    require(sameEvents(press, {event(key, protocol::buttonLeft, 1), sync()}));
    require(press.wakes && mouse.holds());
    // The held button is not sent again, and a second one is an event.
    require(mouse.update(0, 0, 1U, 0, 0).empty());
    require(sameEvents(mouse.update(1, 0, 0b10011, 0, 0), {
        event(protocol::eventRelative, protocol::relativeX, 1),
        event(key, protocol::buttonRight, 1),
        event(key, protocol::buttonExtra, 1), sync()}));
    // A bit past the fifth button is no button.
    require(mouse.update(0, 0, 0b10011 | 0b100000, 0, 0).empty());
    const auto release = mouse.update(0, 0, 0b10, 0, 0);
    require(sameEvents(release, {
        event(key, protocol::buttonLeft, 0),
        event(key, protocol::buttonExtra, 0), sync()}));
    require(!release.wakes);
    require(sameEvents(mouse.releaseAll(), {
        event(key, protocol::buttonRight, 0), sync()}));
    require(!mouse.holds() && mouse.releaseAll().empty());
}

void theWheelsReportHighResolutionUnitsAndDeriveTheSteps()
{
    constexpr auto high = protocol::relativeWheelHighResolution;
    constexpr auto low = protocol::relativeWheel;
    constexpr auto rel = protocol::eventRelative;
    Mouse mouse;
    // A whole detent: the high-resolution event, then the step.
    const auto detent = mouse.update(0, 0, 0, 120, 0);
    require(sameEvents(detent, {
        event(rel, high, 120), event(rel, low, 1), sync()}));
    require(detent.wakes);
    // Half a detent is no step, and the other half completes it.
    const auto half = mouse.update(0, 0, 0, 60, 0);
    require(sameEvents(half, {event(rel, high, 60), sync()}) && !half.wakes);
    const auto rest = mouse.update(0, 0, 0, 60, 0);
    require(sameEvents(rest, {
        event(rel, high, 60), event(rel, low, 1), sync()}) && rest.wakes);
    // Down is negative, and several detents at once are one step event.
    require(sameEvents(mouse.update(0, 0, 0, -360, 0), {
        event(rel, high, -360), event(rel, low, -3), sync()}));
    // A turn the other way cancels the remainder before it steps.
    require(sameEvents(mouse.update(0, 0, 0, 100, 0), {
        event(rel, high, 100), sync()}));
    require(sameEvents(mouse.update(0, 0, 0, -150, 0), {
        event(rel, high, -150), sync()}));
    require(sameEvents(mouse.update(0, 0, 0, -70, 0), {
        event(rel, high, -70), event(rel, low, -1), sync()}));

    // The horizontal wheel has its own codes and its own remainder.
    Mouse sideways;
    require(sameEvents(sideways.update(0, 0, 0, 60, 90), {
        event(rel, high, 60),
        event(rel, protocol::relativeHorizontalWheelHighResolution, 90),
        sync()}));
    require(sameEvents(sideways.update(0, 0, 0, 0, 30), {
        event(rel, protocol::relativeHorizontalWheelHighResolution, 30),
        event(rel, protocol::relativeHorizontalWheel, 1), sync()}));
    // A report with motion, wheels and a button keeps that order.
    Mouse all;
    require(sameEvents(all.update(2, 3, 1U, 120, 120), {
        event(rel, protocol::relativeX, 2), event(rel, protocol::relativeY, 3),
        event(rel, high, 120), event(rel, low, 1),
        event(rel, protocol::relativeHorizontalWheelHighResolution, 120),
        event(rel, protocol::relativeHorizontalWheel, 1),
        event(key, protocol::buttonLeft, 1), sync()}));
    // A reset forgets the remainder.
    all.reset();
    require(sameEvents(all.update(0, 0, 0, 60, 0), {event(rel, high, 60), sync()}));
    all.reset();
    require(all.update(0, 0, 0, 60, 0).events.size() == 2);
}

constexpr Event slot0() noexcept
{
    return event(protocol::eventAbsolute, protocol::absoluteMtSlot, 0);
}
constexpr Event positionX(std::int32_t x) noexcept
{
    return event(protocol::eventAbsolute, protocol::absoluteMtPositionX, x);
}
constexpr Event positionY(std::int32_t y) noexcept
{
    return event(protocol::eventAbsolute, protocol::absoluteMtPositionY, y);
}
constexpr Event trackingId(std::int32_t id) noexcept
{
    return event(protocol::eventAbsolute, protocol::absoluteMtTrackingId, id);
}

void thePointerEntersMovesAndLeaves()
{
    Pointer pointer;
    // Out of range with nothing in range is nothing.
    require(pointer.update(10, 20, 0, false).empty());
    require(pointer.leave().empty() && !pointer.inRange());

    // The tool, then the contact, as Android's own absolute mouse test sends
    // them: a hover.
    const auto enter = pointer.update(100, 200, 0, true);
    require(sameEvents(enter, {
        event(key, protocol::buttonToolMouse, 1),
        event(key, protocol::buttonTouch, 1), slot0(), trackingId(0),
        positionX(100), positionY(200), sync()}));
    require(!enter.wakes && pointer.inRange());

    // A move states the slot and the position, and no tracking id.
    require(sameEvents(pointer.update(101, 200, 0, true), {
        slot0(), positionX(101), positionY(200), sync()}));
    require(pointer.update(101, 200, 0, true).empty());

    // A press with the pointer still wakes and states no position.
    const auto press = pointer.update(101, 200, 0b1, true);
    require(sameEvents(press, {event(key, protocol::buttonLeft, 1), sync()}));
    require(press.wakes);
    // Moving with the button down, then pressing a second.
    require(sameEvents(pointer.update(150, 250, 0b11, true), {
        slot0(), positionX(150), positionY(250),
        event(key, protocol::buttonRight, 1), sync()}));
    require(sameEvents(pointer.update(150, 250, 0b10, true), {
        event(key, protocol::buttonLeft, 0), sync()}));

    // Leaving releases what is held, ends the contact and the tool, and
    // keeps the position out of it.
    const auto leave = pointer.update(150, 250, 0b10, false);
    require(sameEvents(leave, {
        event(key, protocol::buttonRight, 0), slot0(), trackingId(-1),
        event(key, protocol::buttonTouch, 0),
        event(key, protocol::buttonToolMouse, 0), sync()}));
    require(!leave.wakes && !pointer.inRange());
    require(pointer.leave().empty());

    // The next entry is a new contact, which may have a button down already.
    const auto again = pointer.update(5, 6, 0b100, true);
    require(sameEvents(again, {
        event(key, protocol::buttonToolMouse, 1),
        event(key, protocol::buttonTouch, 1), slot0(), trackingId(1),
        positionX(5), positionY(6),
        event(key, protocol::buttonMiddle, 1), sync()}));
    require(again.wakes);
    // A coordinate past the range is held at its edge.
    require(sameEvents(pointer.update(40000, 70000, 0b100, true), {
        slot0(), positionX(32767), positionY(32767), sync()}));
    // A button without a contact is nothing.
    pointer.reset();
    require(!pointer.inRange() && pointer.update(0, 0, 0b1, false).empty());
}

void thePointersTrackingIdsStayInTheRangeItAdvertises()
{
    Pointer pointer;
    std::int32_t last = -1;
    for (int round = 0; round < 70000; ++round) {
        const auto enter = pointer.update(1, 1, 0, true);
        std::int32_t id = -2;
        for (const auto& one : enter.events) {
            if (one.type == protocol::eventAbsolute &&
                one.code == protocol::absoluteMtTrackingId)
                id = one.value;
        }
        require(id >= 0 && id <= protocol::touchscreenTrackingIdMaximum);
        require(id != last);
        last = id;
        (void)pointer.leave();
    }
}

void theRawPositionLandsOnTheSamePixelATouchWould()
{
    using uml::input::virtio::pointerRawPosition;
    // No size, and no coordinate outside the range.
    require(!pointerRawPosition(0, 0, 0, 1080));
    require(!pointerRawPosition(0, 0, 1920, 0));
    require(!pointerRawPosition(32768, 0, 1920, 1080));
    require(!pointerRawPosition(0, 32768, 1920, 1080));
    // The corners, on the long axis and the short one.
    const auto landscape = pointerRawPosition(32767, 32767, 1920, 1080);
    require(landscape && landscape->x == 32767 && landscape->y == 18431);
    const auto portrait = pointerRawPosition(32767, 32767, 1080, 1920);
    require(portrait && portrait->x == 18431 && portrait->y == 32767);
    const auto square = pointerRawPosition(12345, 23456, 1000, 1000);
    require(square && square->x == 12345 && square->y == 23456);
    const auto origin = pointerRawPosition(0, 0, 1920, 1080);
    require(origin && origin->x == 0 && origin->y == 0);

    // Android maps an absolute mouse's raw range onto the display with one
    // scale, max(width, height) / 32768 pixels per raw unit, and a touchscreen
    // 's onto each axis. For the centre of every pixel, the pixel Android puts
    // the cursor on is the pixel a touch there would touch.
    struct Mode {
        std::uint32_t width;
        std::uint32_t height;
    };
    for (const Mode mode : {Mode {1920, 1080}, Mode {1080, 1920},
                            Mode {1280, 720}, Mode {800, 1280},
                            Mode {1000, 1000}, Mode {2560, 1600},
                            Mode {3840, 2160}, Mode {320, 240}}) {
        const std::uint32_t larger = std::max(mode.width, mode.height);
        const auto touch = [](std::uint32_t pixel, std::uint32_t extent) {
            // The touchscreen coordinate of a pixel's centre, which Android
            // maps back to that pixel.
            return static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(2U * pixel + 1U) * 32768U) /
                (2U * extent));
        };
        const auto android = [larger](std::uint32_t raw) {
            return static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(raw) * larger / 32768U);
        };
        for (std::uint32_t py = 0; py < mode.height; ++py) {
            const auto ty = touch(py, mode.height);
            for (std::uint32_t px = 0; px < mode.width; ++px) {
                const auto tx = touch(px, mode.width);
                const auto raw = pointerRawPosition(
                    tx, ty, mode.width, mode.height);
                require(raw.has_value());
                require(raw->x <= 32767 && raw->y <= 32767);
                require(android(raw->x) == px);
                require(android(raw->y) == py);
                // Skip rows to keep the check quick on the big modes.
                if (mode.width > 1000 && px > 64)
                    px += 31;
            }
            if (mode.height > 1000)
                py += 15;
        }
    }
}

// ---- HardwareInput over the three devices.

struct Rig final {
    std::shared_ptr<EventQueue> keyboardQueue {
        std::make_shared<EventQueue>(EventQueue::Attachment::pluggable)};
    std::shared_ptr<EventQueue> mouseQueue {
        std::make_shared<EventQueue>(EventQueue::Attachment::pluggable)};
    std::shared_ptr<EventQueue> pointerQueue {
        std::make_shared<EventQueue>(EventQueue::Attachment::pluggable)};
    VirtioInputDevice keyboardDevice {
        DeviceKind::hardwareKeyboard, keyboardQueue};
    VirtioInputDevice mouseDevice {DeviceKind::mouse, mouseQueue};
    VirtioInputDevice pointerDevice {DeviceKind::pointer, pointerQueue};
    HardwareInput input {keyboardQueue, mouseQueue, pointerQueue};
    std::vector<std::pair<HardwareStatus, bool>> statuses;

    Rig()
    {
        input.setStatusCallback([this](HardwareStatus kind, bool on) {
            statuses.emplace_back(kind, on);
        });
    }

    static void status(VirtioInputDevice& device, Event sent)
    {
        const auto result = device.processQueue(
            protocol::statusQueue, 1, uml::virtio::toBytes(sent), 0);
        require(result.action == uml::virtio::VhostUserQueueAction::complete);
    }

    static void registers(VirtioInputDevice& device)
    {
        status(device, event(
            protocol::statusNotice, protocol::noticeRegistration, 1));
    }

    static Events received(VirtioInputDevice& device)
    {
        Events all;
        for (;;) {
            const auto result =
                device.processQueue(protocol::eventQueue, 2, {}, 8);
            if (result.action != uml::virtio::VhostUserQueueAction::complete)
                return all;
            all.push_back(uml::testing::loadExact<protocol::InputEvent>(
                result.response));
        }
    }

    void plugEverything()
    {
        input.setKeyboardConnected(true);
        input.setMouseConnected(true);
        registers(keyboardDevice);
        registers(mouseDevice);
        registers(pointerDevice);
    }
};

void theKeyboardAndTheMouseArePluggedSeparately()
{
    Rig rig;
    require(!rig.input.keyboardConnected() && !rig.input.mouseConnected());
    rig.input.setKeyboardConnected(true);
    require(rig.input.keyboardConnected() && !rig.input.mouseConnected());
    require(rig.keyboardDevice.configChangePending());
    require(!rig.mouseDevice.configChangePending());
    require(!rig.pointerDevice.configChangePending());

    // The mouse and the pointer are one mouse to Android, so they are plugged
    // together, one configuration change each.
    rig.input.setMouseConnected(true);
    require(rig.input.mouseConnected());
    require(rig.mouseDevice.configChangePending());
    require(rig.pointerDevice.configChangePending());
    require(rig.mouseQueue->connected() && rig.pointerQueue->connected());

    // Plugging a plugged device changes nothing.
    for (auto* device : {&rig.keyboardDevice, &rig.mouseDevice,
                         &rig.pointerDevice}) {
        require(device->claimConfigChange());
        device->clearConfigChange();
    }
    rig.input.setKeyboardConnected(true);
    rig.input.setMouseConnected(true);
    require(!rig.keyboardDevice.configChangePending());
    require(!rig.mouseDevice.configChangePending());
    require(!rig.pointerDevice.configChangePending());

    rig.input.setMouseConnected(false);
    require(!rig.input.mouseConnected() && rig.input.keyboardConnected());
    require(rig.mouseDevice.configChangePending());
    require(rig.pointerDevice.configChangePending());
    require(!rig.keyboardDevice.configChangePending());
}

void inputToAnUnpluggedDeviceIsDroppedWithoutBeingRemembered()
{
    Rig rig;
    require(!rig.input.key(30, true));
    require(!rig.input.keyUsage(0x04, true));
    require(!rig.input.pointer(10, 10, 1, true, 1920, 1080));
    require(!rig.input.mouse(1, 1, 1, 0, 0));
    rig.plugEverything();
    require(Rig::received(rig.keyboardDevice).empty());
    require(Rig::received(rig.pointerDevice).empty());
    require(Rig::received(rig.mouseDevice).empty());
    // The press that was dropped is not held: its release is nothing, and
    // the pointer enters afresh.
    require(!rig.input.key(30, false));
    require(Rig::received(rig.keyboardDevice).empty());
    require(!rig.input.pointer(10, 10, 0, true, 1920, 1080));
    const auto pointer = Rig::received(rig.pointerDevice);
    require(pointer.size() == 7 &&
        pointer[0].code == protocol::buttonToolMouse && pointer[0].value == 1);
}

void keysArriveAsLinuxCodesAndTheirReturnWakes()
{
    Rig rig;
    rig.plugEverything();
    // A HID usage becomes the Linux key a USB keyboard would produce: A.
    require(rig.input.keyUsage(0x04, true));
    require(!rig.input.keyUsage(0x04, false));
    // Left GUI, which is Android's Meta, and a usage with no key.
    require(rig.input.keyUsage(0xe3, true));
    require(!rig.input.keyUsage(0x82, true));
    require(!rig.input.keyUsage(0x10004, true));
    require(!rig.input.key(0, true));
    const auto keys = Rig::received(rig.keyboardDevice);
    require(sameEvents(keys, {
        event(key, 30, 1), sync(), event(key, 30, 0), sync(),
        event(key, 125, 1), sync()}));
    // The same key held down, then the table's own code through key().
    require(rig.input.key(48, true));
    require(!rig.input.key(48, true));
    require(Rig::received(rig.keyboardDevice).size() == 2);
}

void thePointerIsMappedOntoTheGuestDisplay()
{
    Rig rig;
    rig.plugEverything();
    // No display mode, no position.
    require(!rig.input.pointer(1000, 1000, 0, true, 0, 0));
    require(Rig::received(rig.pointerDevice).empty());
    require(!rig.input.pointer(32767, 32767, 0, true, 1920, 1080));
    const auto enter = Rig::received(rig.pointerDevice);
    require(sameEvents(enter, {
        event(key, protocol::buttonToolMouse, 1),
        event(key, protocol::buttonTouch, 1), slot0(), trackingId(0),
        positionX(32767), positionY(18431), sync()}));
    // A press wakes; out of range ends the contact even with no display.
    require(rig.input.pointer(0, 0, 1, true, 1920, 1080));
    require(!rig.input.pointer(0, 0, 0, false, 0, 0));
    require(sameEvents(Rig::received(rig.pointerDevice), {
        slot0(), positionX(0), positionY(0),
        event(key, protocol::buttonLeft, 1), sync(),
        event(key, protocol::buttonLeft, 0), slot0(), trackingId(-1),
        event(key, protocol::buttonTouch, 0),
        event(key, protocol::buttonToolMouse, 0), sync()}));
}

void theMouseCarriesMotionButtonsAndWheels()
{
    Rig rig;
    rig.plugEverything();
    require(!rig.input.mouse(3, 4, 0, 0, 0));
    require(rig.input.mouse(0, 0, 1, 0, 0));
    require(rig.input.mouse(0, 0, 1, 120, 0));
    require(!rig.input.mouse(0, 0, 1, 30, 0));
    const auto all = Rig::received(rig.mouseDevice);
    require(sameEvents(all, {
        event(protocol::eventRelative, protocol::relativeX, 3),
        event(protocol::eventRelative, protocol::relativeY, 4), sync(),
        event(key, protocol::buttonLeft, 1), sync(),
        event(protocol::eventRelative, protocol::relativeWheelHighResolution, 120),
        event(protocol::eventRelative, protocol::relativeWheel, 1), sync(),
        event(protocol::eventRelative, protocol::relativeWheelHighResolution, 30),
        sync()}));
}

void anUnplugReleasesEverythingTheHostHolds()
{
    Rig rig;
    rig.plugEverything();
    (void)rig.input.key(30, true);
    (void)rig.input.key(48, true);
    (void)rig.input.mouse(0, 0, 0b101, 0, 0);
    (void)rig.input.pointer(100, 100, 0b11, true, 1000, 1000);
    (void)Rig::received(rig.keyboardDevice);
    (void)Rig::received(rig.mouseDevice);
    (void)Rig::received(rig.pointerDevice);
    for (auto* device : {&rig.keyboardDevice, &rig.mouseDevice,
                         &rig.pointerDevice}) {
        require(device->claimConfigChange());
        device->clearConfigChange();
    }

    rig.input.setKeyboardConnected(false);
    require(rig.keyboardDevice.configChangePending());
    // The keyboard is not the mouse: it stays down.
    require(rig.input.mouseConnected());
    require(sameEvents(Rig::received(rig.keyboardDevice), {
        event(key, 30, 0), event(key, 48, 0), sync()}));

    rig.input.setMouseConnected(false);
    require(sameEvents(Rig::received(rig.mouseDevice), {
        event(key, protocol::buttonLeft, 0),
        event(key, protocol::buttonMiddle, 0), sync()}));
    require(sameEvents(Rig::received(rig.pointerDevice), {
        event(key, protocol::buttonLeft, 0),
        event(key, protocol::buttonRight, 0), slot0(), trackingId(-1),
        event(key, protocol::buttonTouch, 0),
        event(key, protocol::buttonToolMouse, 0), sync()}));
    require(rig.mouseDevice.configChangePending());
    require(rig.pointerDevice.configChangePending());

    // And all of it is gone: nothing is sent, and a new plug starts clean.
    require(!rig.input.key(30, true));
    require(!rig.input.mouse(1, 1, 1, 0, 0));
    rig.input.setKeyboardConnected(true);
    Rig::registers(rig.keyboardDevice);
    require(rig.input.key(30, true));
    require(Rig::received(rig.keyboardDevice).size() == 2);
}

void theGuestsLedWritesReachTheCallback()
{
    Rig rig;
    rig.plugEverything();
    Rig::status(rig.keyboardDevice, event(protocol::eventLed, protocol::ledCapsLock, 1));
    Rig::status(rig.keyboardDevice, event(protocol::eventLed, protocol::ledNumLock, 1));
    Rig::status(rig.keyboardDevice, event(protocol::eventLed, protocol::ledScrollLock, 0));
    Rig::status(rig.keyboardDevice, event(protocol::eventLed, protocol::ledMisc, 1));
    Rig::status(rig.mouseDevice, event(protocol::eventLed, protocol::ledMisc, 1));
    Rig::status(rig.mouseDevice, event(protocol::eventLed, protocol::ledMisc, 0));
    Rig::status(rig.mouseDevice, event(protocol::eventLed, protocol::ledCapsLock, 1));
    Rig::status(rig.pointerDevice, event(protocol::eventLed, protocol::ledMisc, 1));
    require(rig.statuses.size() == 5);
    require(rig.statuses[0] == std::pair {HardwareStatus::capsLock, true});
    require(rig.statuses[1] == std::pair {HardwareStatus::numLock, true});
    require(rig.statuses[2] == std::pair {HardwareStatus::scrollLock, false});
    require(rig.statuses[3] == std::pair {HardwareStatus::pointerCapture, true});
    require(rig.statuses[4] == std::pair {HardwareStatus::pointerCapture, false});
    // A cleared callback hears nothing, and the host stays up.
    rig.input.setStatusCallback({});
    Rig::status(rig.mouseDevice, event(protocol::eventLed, protocol::ledMisc, 1));
    require(rig.statuses.size() == 5);
}

void aRegistrationThatFollowsDroppedInputMakesTheHostForgetWhatItHolds()
{
    Rig rig;
    rig.input.setMouseConnected(true);
    // The pointer enters, and the guest is slow: more reports than the buffer
    // holds are dropped, the enter among them.
    (void)rig.input.pointer(1, 1, 0, true, 1000, 1000);
    for (std::uint32_t step = 0; step < 200; ++step)
        (void)rig.input.pointer(2 + step, 1, 0, true, 1000, 1000);
    Rig::registers(rig.pointerDevice);
    require(Rig::received(rig.pointerDevice).empty());
    // The host knows the guest saw none of it, so the next report is an entry
    // again, not a move that Android would ignore for want of a contact.
    (void)rig.input.pointer(50, 60, 0, true, 1000, 1000);
    const auto next = Rig::received(rig.pointerDevice);
    require(next.size() == 7);
    require(next[0].code == protocol::buttonToolMouse && next[0].value == 1);

    // The keyboard: a key held through the loss is not left down.
    rig.input.setKeyboardConnected(true);
    (void)rig.input.key(30, true);
    for (int round = 0; round < 200; ++round) {
        (void)rig.input.key(48, true);
        (void)rig.input.key(48, false);
    }
    Rig::registers(rig.keyboardDevice);
    require(Rig::received(rig.keyboardDevice).empty());
    require(rig.input.key(30, true)); // pressed afresh: it was forgotten
    require(Rig::received(rig.keyboardDevice).size() == 2);
}

} // namespace

int main()
{
    aKeyGoesDownOnceAndUpOnce();
    aKeyTheTableCannotProduceIsDropped();
    releaseAllLetsGoOfEveryKeyInOneReport();
    aMouseMovesAndScrollsAndPressesButtons();
    theWheelsReportHighResolutionUnitsAndDeriveTheSteps();
    thePointerEntersMovesAndLeaves();
    thePointersTrackingIdsStayInTheRangeItAdvertises();
    theRawPositionLandsOnTheSamePixelATouchWould();
    theKeyboardAndTheMouseArePluggedSeparately();
    inputToAnUnpluggedDeviceIsDroppedWithoutBeingRemembered();
    keysArriveAsLinuxCodesAndTheirReturnWakes();
    thePointerIsMappedOntoTheGuestDisplay();
    theMouseCarriesMotionButtonsAndWheels();
    anUnplugReleasesEverythingTheHostHolds();
    theGuestsLedWritesReachTheCallback();
    aRegistrationThatFollowsDroppedInputMakesTheHostForgetWhatItHolds();
    return EXIT_SUCCESS;
}
