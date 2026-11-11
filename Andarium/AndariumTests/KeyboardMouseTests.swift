// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CoreGraphics
import GuestHostCore
import Testing
@testable import Andarium

/// iOS's end of the hardware keyboard and mouse, driven by a test.
@MainActor
private final class FakePresence: HardwarePresence {
    var keyboardConnected = false
    var mouseConnected = false
    var changed: (@MainActor () -> Void)?
    private(set) var running = false

    func start() { running = true }
    func stop() { running = false }

    /// iOS reports hardware coming or going.
    func report(keyboard: Bool? = nil, mouse: Bool? = nil) {
        if let keyboard { keyboardConnected = keyboard }
        if let mouse { mouseConnected = mouse }
        changed?()
    }
}

@MainActor
private final class FakeRawMouse: RawMouseSource {
    private var handler: (@MainActor (RawMouseEvent) -> Void)?
    var running: Bool { handler != nil }

    func startRaw(_ handler: @escaping @MainActor (RawMouseEvent) -> Void) { self.handler = handler }
    func stopRaw() { handler = nil }
    func emit(_ event: RawMouseEvent) { handler?(event) }
}

/// A session with a recording runtime in place of the guest's, a clock the
/// test moves and the people's hardware as the test says.
@MainActor
private final class Rig {
    let runtime = RecordingHardwareInputRuntime()
    let presence = FakePresence()
    let rawMouse = FakeRawMouse()
    var now = 100.0
    private(set) var lockWanted: [Bool] = []
    private(set) var session: KeyboardMouse!

    init(supportsMouse: Bool = true) {
        session = KeyboardMouse(
            runtime: runtime, presence: presence, rawMouse: rawMouse, supportsMouse: supportsMouse,
            clock: { [unowned self] in self.now },
            wantsLock: { [unowned self] in self.lockWanted.append($0) })
    }

    var input: GuestHardwareInput { session.input }
    var events: [HardwareInputEvent] { runtime.events }

    /// The run has started and the guest is in front.
    func start() { session.update(running: true, foreground: true) }
    func leaveTheFront() { session.update(running: true, foreground: false) }
    func returnToTheFront() { session.update(running: true, foreground: true) }
    func endTheRun() { session.update(running: false, foreground: false) }

    /// Time passes and the session is asked what the guest should have.
    func advance(_ seconds: Double) {
        now += seconds
        session.evaluate()
    }

    func type(_ usage: UInt32) {
        input.keyDown(usage: usage, alphaShift: false)
        input.keyUp(usage: usage)
    }

    /// The pointer hovers at `point`, which maps to `position` of the display.
    func hover(_ position: GuestPointerMapping.Position, at point: CGPoint, beginning: Bool = false) {
        input.hover(position, at: point, beginning: beginning)
    }

    /// A mouse iOS reports and the person has moved over the display.
    func useTheMouse(at position: GuestPointerMapping.Position = Spot.centre) {
        presence.report(mouse: true)
        hover(position, at: CGPoint(x: 10, y: 10), beginning: true)
        hover(position, at: CGPoint(x: 11, y: 10))
    }

    func finish() { session.shutDown() }

    enum Spot {
        static let centre = GuestPointerMapping.Position(x: 16384, y: 16384, inside: true)
        static let corner = GuestPointerMapping.Position(x: 100, y: 200, inside: true)
        static let beside = GuestPointerMapping.Position(x: 0, y: 5000, inside: false)
    }
}

private extension HardwareInputEvent {
    static func key(_ usage: UInt32, _ pressed: Bool) -> HardwareInputEvent { .key(usage: usage, pressed: pressed) }
}

/// What the guest's keyboard and mouse receive from the hardware iOS has: the
/// plug policy and the pointer capture at the application's end, over the
/// guest view's input and a runtime that records.
@MainActor
@Suite(.serialized)
struct KeyboardMouseTests {
    // MARK: Keyboard

    @Test func aKeyPlugsTheKeyboardBeforeItsFirstKeyAndGoesAsAPosition() {
        let rig = Rig()
        defer { rig.finish() }
        rig.start()
        rig.type(0x04)
        #expect(rig.events == [.connect(.keyboard, true), .key(0x04, true), .key(0x04, false)])
    }

    @Test func aKeyboardIOSReportsIsPluggedAtOnceAndGoesThreeSecondsAfterIt() {
        let rig = Rig()
        defer { rig.finish() }
        rig.start()
        rig.presence.report(keyboard: true)
        #expect(rig.events == [.connect(.keyboard, true)])
        rig.presence.report(keyboard: false)
        rig.advance(2)
        #expect(rig.events == [.connect(.keyboard, true)])
        rig.advance(1.5)
        #expect(rig.events == [.connect(.keyboard, true), .connect(.keyboard, false)])
    }

    @Test func aKeyboardStaysWhileAKeyIsHeldAndGoesWhenItComesUp() {
        let rig = Rig()
        defer { rig.finish() }
        rig.start()
        rig.input.keyDown(usage: 0x04, alphaShift: false)
        rig.advance(30)
        #expect(rig.events == [.connect(.keyboard, true), .key(0x04, true)])
        rig.input.keyUp(usage: 0x04)
        #expect(rig.events == [.connect(.keyboard, true), .key(0x04, true), .key(0x04, false), .connect(.keyboard, false)])
    }

    @Test func nothingIsSentBeforeTheRunHasStartedOrAfterItEnded() {
        let rig = Rig()
        defer { rig.finish() }
        rig.session.update(running: false, foreground: true)
        rig.type(0x04)
        #expect(rig.events.isEmpty)
        rig.start()
        rig.type(0x05)
        rig.endTheRun()
        rig.type(0x06)
        #expect(rig.events == [.connect(.keyboard, true), .key(0x05, true), .key(0x05, false)])
    }

    @Test func aRefusedPlugIsAskedAgainAtTheNextKey() {
        let rig = Rig()
        defer { rig.finish() }
        rig.runtime.accepts = false
        rig.start()
        rig.type(0x04)
        #expect(rig.events.isEmpty)
        rig.runtime.accepts = true
        rig.type(0x05)
        #expect(rig.events == [.connect(.keyboard, true), .key(0x05, true), .key(0x05, false)])
    }

    @Test func aNewRunPlugsTheDevicesAgain() {
        let rig = Rig()
        defer { rig.finish() }
        rig.start()
        rig.type(0x04)
        rig.endTheRun()
        rig.start()
        rig.type(0x04)
        let connects = rig.events.filter { $0 == .connect(.keyboard, true) }
        #expect(connects.count == 2)
    }

    @Test func leavingTheFrontLiftsEveryKeyNewestFirstAndKeepsTheKeyboard() {
        let rig = Rig()
        defer { rig.finish() }
        rig.start()
        rig.input.keyDown(usage: 0x04, alphaShift: false)
        rig.input.keyDown(usage: 0xE1, alphaShift: false)
        rig.leaveTheFront()
        #expect(rig.events.suffix(2) == [.key(0xE1, false), .key(0x04, false)])
        #expect(!rig.events.contains(.connect(.keyboard, false)))
        // Keys that come while it is behind go nowhere, and none is stuck
        // when it returns.
        let before = rig.events.count
        rig.type(0x05)
        #expect(rig.events.count == before)
        rig.returnToTheFront()
        rig.type(0x06)
        #expect(rig.events.suffix(2) == [.key(0x06, true), .key(0x06, false)])
    }

    @Test func iOSsCapsLockIsTappedInAndroidsBeforeTheKeyWhenTheyDiffer() {
        let rig = Rig()
        defer { rig.finish() }
        rig.start()
        rig.input.keyDown(usage: 0x04, alphaShift: true)
        #expect(rig.events == [
            .connect(.keyboard, true), .key(0x39, true), .key(0x39, false), .key(0x04, true),
        ])
        // Android's light answers; the two agree and nothing is tapped.
        rig.runtime.androidSays(.capsLock, true)
        rig.input.keyUp(usage: 0x04)
        rig.now += 5
        rig.input.keyDown(usage: 0x05, alphaShift: true)
        #expect(rig.events.suffix(2) == [.key(0x04, false), .key(0x05, true)])
    }

    // MARK: Mouse

    @Test func aMouseIOSReportsIsPluggedWhenUsedAndItsPointerIsTheGuestsPointer() {
        let rig = Rig()
        defer { rig.finish() }
        rig.start()
        rig.presence.report(mouse: true)
        #expect(rig.input.routesPointer)
        // The Simulator reports a mouse that is not there: reporting is not use.
        rig.hover(Rig.Spot.centre, at: CGPoint(x: 10, y: 10), beginning: true)
        #expect(rig.events.isEmpty)
        rig.hover(Rig.Spot.corner, at: CGPoint(x: 20, y: 10))
        #expect(rig.events == [
            .connect(.mouse, true), .pointer(PointerReport(x: 100, y: 200, buttons: 0, inRange: true)),
        ])
    }

    @Test func buttonsGoWithThePositionAndWheelsGoAsDetents() {
        let rig = Rig()
        defer { rig.finish() }
        rig.start()
        rig.useTheMouse(at: Rig.Spot.corner)
        let primary = HardwareButtons.primary
        rig.input.pointerTouch(.began, at: Rig.Spot.centre, buttons: primary)
        rig.input.pointerTouch(.moved, at: Rig.Spot.corner, buttons: primary)
        rig.input.pointerTouch(.ended, at: Rig.Spot.corner, buttons: 0)
        #expect(rig.events.suffix(3) == [
            .pointer(PointerReport(x: 16384, y: 16384, buttons: primary, inRange: true)),
            .pointer(PointerReport(x: 100, y: 200, buttons: primary, inRange: true)),
            .pointer(PointerReport(x: 100, y: 200, buttons: 0, inRange: true)),
        ])
        // A trackpad's 64 points and a wheel's 20 are a detent, and a scroll
        // down is a negative wheel.
        rig.input.scroll(translation: CGPoint(x: 0, y: -64), continuous: true, beginning: true)
        rig.input.scroll(translation: CGPoint(x: -20, y: 20), continuous: false, beginning: true)
        #expect(rig.events.suffix(2) == [
            .mouse(dx: 0, dy: 0, buttons: 0, wheel: -120, horizontalWheel: 0),
            .mouse(dx: 0, dy: 0, buttons: 0, wheel: 120, horizontalWheel: 120),
        ])
    }

    @Test func aPointerThatScrollsBeforeItMovesPlugsTheMouseToo() {
        let rig = Rig()
        defer { rig.finish() }
        rig.start()
        rig.presence.report(mouse: true)
        rig.input.scroll(translation: CGPoint(x: 0, y: 64), continuous: true, beginning: true)
        #expect(rig.events == [.connect(.mouse, true), .mouse(dx: 0, dy: 0, buttons: 0, wheel: 120, horizontalWheel: 0)])
    }

    @Test func anIPhoneHasNoMouseToGiveTheGuest() {
        let rig = Rig(supportsMouse: false)
        defer { rig.finish() }
        rig.start()
        rig.presence.report(mouse: true)
        #expect(!rig.input.routesPointer)
        rig.hover(Rig.Spot.centre, at: CGPoint(x: 10, y: 10), beginning: true)
        rig.hover(Rig.Spot.corner, at: CGPoint(x: 20, y: 10))
        #expect(rig.events.isEmpty)
    }

    @Test func usingThePointerAsTouchTakesTheMouseOffTheGuest() {
        let rig = Rig()
        defer { rig.finish() }
        rig.start()
        rig.useTheMouse()
        #expect(rig.events.contains(.connect(.mouse, true)))
        rig.session.setPointerAsTouch(true)
        #expect(!rig.input.routesPointer)
        #expect(rig.events.suffix(2) == [
            .pointer(PointerReport(x: 16384, y: 16384, buttons: 0, inRange: false)), .connect(.mouse, false),
        ])
        // Moving does not bring it back while the setting is on.
        rig.hover(Rig.Spot.corner, at: CGPoint(x: 30, y: 10))
        #expect(rig.events.last == .connect(.mouse, false))
    }

    @Test func aMouseGoesThreeSecondsAfterIOSStopsReportingIt() {
        let rig = Rig()
        defer { rig.finish() }
        rig.start()
        rig.useTheMouse()
        rig.presence.report(mouse: false)
        rig.advance(2)
        #expect(!rig.events.contains(.connect(.mouse, false)))
        rig.advance(1.5)
        #expect(rig.events.contains(.connect(.mouse, false)))
    }

    @Test func leavingTheFrontEndsThePointersContactAndKeepsTheMouse() {
        let rig = Rig()
        defer { rig.finish() }
        rig.start()
        rig.useTheMouse()
        rig.input.pointerTouch(.began, at: Rig.Spot.centre, buttons: HardwareButtons.primary)
        rig.leaveTheFront()
        #expect(rig.events.last == .pointer(PointerReport(x: 16384, y: 16384, buttons: 0, inRange: false)))
        #expect(!rig.events.contains(.connect(.mouse, false)))
    }

    // MARK: Capture

    @Test func theGuestsCaptureAsksForTheLockAndForwardsRawMotionWhileItIsHeld() {
        let rig = Rig()
        defer { rig.finish() }
        rig.start()
        rig.useTheMouse()
        // The pointer has left the display.
        rig.hover(Rig.Spot.beside, at: CGPoint(x: -3, y: 10))
        let outside = rig.events.count
        rig.runtime.androidSays(.pointerCapture, true)
        #expect(rig.lockWanted.last == true)
        #expect(rig.rawMouse.running)
        #expect(rig.input.capturing)
        // The lock is not granted and the pointer is not over the display:
        // the mouse is not the guest's.
        rig.rawMouse.emit(.moved(dx: 5, dy: 3))
        #expect(rig.events.count == outside)
        rig.session.pointerLockChanged(true)
        // Game Controller's up is positive; the guest's y grows downward.
        rig.rawMouse.emit(.moved(dx: 5, dy: 3))
        rig.rawMouse.emit(.buttons(HardwareButtons.primary | HardwareButtons.secondary))
        rig.rawMouse.emit(.moved(dx: -2, dy: -4))
        rig.rawMouse.emit(.scrolled(x: 64, y: -64))
        let both = HardwareButtons.primary | HardwareButtons.secondary
        #expect(rig.events.suffix(4) == [
            .mouse(dx: 5, dy: -3, buttons: 0, wheel: 0, horizontalWheel: 0),
            .mouse(dx: 0, dy: 0, buttons: both, wheel: 0, horizontalWheel: 0),
            .mouse(dx: -2, dy: 4, buttons: both, wheel: 0, horizontalWheel: 0),
            .mouse(dx: 0, dy: 0, buttons: both, wheel: -120, horizontalWheel: 120),
        ])
    }

    @Test func theSystemPointerIsNotTheGuestsWhileItCapturesAndRawMotionFollowsTheHoverWithoutALock() {
        let rig = Rig()
        defer { rig.finish() }
        rig.start()
        rig.useTheMouse()
        rig.hover(Rig.Spot.beside, at: CGPoint(x: -3, y: 10))
        rig.runtime.androidSays(.pointerCapture, true)
        let before = rig.events.count
        rig.input.pointerTouch(.began, at: Rig.Spot.corner, buttons: HardwareButtons.primary)
        rig.rawMouse.emit(.moved(dx: 1, dy: 0))
        #expect(rig.events.count == before)
        // The lock was refused: the pointer hovering over the display is
        // what lets the mouse move the guest.
        rig.hover(Rig.Spot.corner, at: CGPoint(x: 41, y: 10))
        rig.rawMouse.emit(.moved(dx: 1, dy: 0))
        #expect(rig.events.last == .mouse(dx: 1, dy: 0, buttons: 0, wheel: 0, horizontalWheel: 0))
        rig.hover(Rig.Spot.beside, at: CGPoint(x: -3, y: 10))
        let after = rig.events.count
        rig.rawMouse.emit(.moved(dx: 1, dy: 0))
        #expect(rig.events.count == after)
    }

    @Test func captureEndsWhenTheGuestLetsGoOrLeavesTheFrontOrTheRunEnds() {
        for end in [
            { (rig: Rig) in rig.runtime.androidSays(.pointerCapture, false) },
            { (rig: Rig) in rig.leaveTheFront() },
            { (rig: Rig) in rig.endTheRun() },
        ] {
            let rig = Rig()
            rig.start()
            rig.useTheMouse()
            rig.runtime.androidSays(.pointerCapture, true)
            rig.session.pointerLockChanged(true)
            rig.rawMouse.emit(.buttons(HardwareButtons.primary))
            end(rig)
            #expect(rig.lockWanted.last == false)
            #expect(!rig.rawMouse.running)
            #expect(!rig.input.capturing)
            rig.finish()
        }
    }

    @Test func aCaptureWithoutAMouseAsksForNothing() {
        let rig = Rig(supportsMouse: false)
        defer { rig.finish() }
        rig.start()
        rig.runtime.androidSays(.pointerCapture, true)
        #expect(rig.lockWanted.allSatisfy { !$0 })
        #expect(!rig.rawMouse.running)
    }
}
