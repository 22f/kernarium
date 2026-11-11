// SPDX-License-Identifier: GPL-2.0-or-later

import CoreGraphics
import Foundation
import GuestHostCore

/// Where the hardware keyboard and mouse of a guest view end up. The hosting
/// application implements it: it decides whether the guest has the devices
/// and passes the events to the runtime.
protocol GuestHardwareSink: AnyObject {
    /// A key by its position (a usage on the HID Keyboard/Keypad page).
    func hardwareKey(usage: UInt32, pressed: Bool)
    /// The system pointer over the display, on the touchscreen's range.
    func hardwarePointer(_ report: PointerReport)
    /// The mouse's wheels, in 1/120 of a detent, and (when Android captured
    /// the pointer) its motion and buttons.
    func hardwareMouse(dx: Int32, dy: Int32, buttons: UInt32, wheel: Int32, horizontalWheel: Int32)
    /// A key went down: the person is using a keyboard, whether or not iOS
    /// said so. Called before the key itself reaches the sink.
    func hardwareKeyboardUsed()
    /// The pointer moved over the display, was clicked or scrolled. Called
    /// before the event itself reaches the sink.
    func hardwarePointerUsed()
}

/// What a guest view does with hardware keys and with the system pointer,
/// between UIKit's events and a `GuestHardwareSink`. The view reports what UIKit
/// delivers; this holds the state that decides what goes to the guest: the
/// keys held down, Caps Lock, the pointer's contact and the scroll remainders.
///
/// Nothing is sent while the input is not active (no guest runs, or it is not
/// in front), and going inactive lifts everything that is down.
final class GuestHardwareInput {
    /// How much scrolling UIKit reports makes a detent of the wheel, in
    /// points: a trackpad's two-finger scroll is 1:1 with Android's own
    /// 64 dp to a detent, and a wheel's step is smaller.
    static let continuousPointsPerDetent: Double = 64
    static let discretePointsPerDetent: Double = 20

    weak var sink: (any GuestHardwareSink)?
    private let clock: () -> Double

    private var keys = HardwareKeyboardState()
    private var capsLock = CapsLockReconciler()
    private var pointer = GuestPointerTracker()
    private var continuousScroll = ScrollAccumulator(pointsPerDetent: continuousPointsPerDetent)
    private var discreteScroll = ScrollAccumulator(pointsPerDetent: discretePointsPerDetent)
    private var lastHover: CGPoint?

    init(clock: @escaping () -> Double = { ProcessInfo.processInfo.systemUptime }) {
        self.clock = clock
    }

    /// The guest runs and is in front. Keys and the pointer reach it only
    /// then; leaving lifts every key and button and ends the pointer's contact.
    var isActive = false {
        didSet {
            guard isActive != oldValue else { return }
            hidesSystemPointerDidChange?()
            if !isActive { releaseAll() }
        }
    }

    /// Called when `hidesSystemPointer` may have changed, so the view can ask
    /// UIKit for the pointer's style again.
    var hidesSystemPointerDidChange: (() -> Void)?

    /// The pointer of a mouse goes to the guest's mouse (the system's pointer
    /// clicks are not touches), and the system's own pointer is hidden over
    /// the display, since Android draws its cursor.
    var routesPointer = false {
        didSet {
            guard routesPointer != oldValue else { return }
            hidesSystemPointerDidChange?()
            if !routesPointer { releasePointer() }
        }
    }

    /// Android captured the pointer: the pointer, its buttons and the scroll
    /// reach the guest as the mouse's raw motion and buttons from another
    /// source, so none of them goes through here.
    var capturing = false {
        didSet {
            guard capturing, !oldValue else { return }
            hoverInside = pointer.inRange
            releasePointer()
        }
    }

    /// Whether the system's pointer hovers over the display, which decides
    /// whether raw motion reaches the guest when iOS refused the pointer lock.
    /// While Android has the pointer UIKit still says where it hovers, and the
    /// guest hears nothing of it.
    var pointerIsOverDisplay: Bool { capturing ? hoverInside : pointer.inRange }
    private var hoverInside = false

    var hidesSystemPointer: Bool { isActive && routesPointer }

    /// The mouse is the guest's for the events UIKit delivers.
    private var routesMouse: Bool { isActive && routesPointer && !capturing }

    // MARK: Keys

    /// A key went down. `alphaShift` is iOS's Caps Lock state.
    func keyDown(usage: UInt32, alphaShift: Bool) {
        guard isActive, HardwareKeyboardState.usages.contains(usage) else { return }
        sink?.hardwareKeyboardUsed()
        let now = clock()
        if usage == HardwareKeyboardState.capsLockUsage {
            capsLock.capsLockKeyForwarded(at: now)
        } else if capsLock.reconcile(alphaShift: alphaShift, at: now) {
            // Android's Caps Lock was not iOS's: tap it before this key.
            sink?.hardwareKey(usage: HardwareKeyboardState.capsLockUsage, pressed: true)
            sink?.hardwareKey(usage: HardwareKeyboardState.capsLockUsage, pressed: false)
        }
        send(keys.press(usage))
    }

    func keyUp(usage: UInt32) {
        send(keys.release(usage))
    }

    /// Lifts every key that is down.
    func releaseKeys() {
        send(keys.releaseAll())
    }

    /// Android's Caps Lock light changed.
    func androidCapsLock(lit: Bool) {
        capsLock.androidLight(lit)
    }

    /// The keyboard was plugged anew, so Android's Caps Lock starts off.
    func keyboardPlugged() {
        capsLock.keyboardPlugged()
    }

    private func send(_ events: [HardwareKeyEvent]) {
        for event in events { sink?.hardwareKey(usage: event.usage, pressed: event.pressed) }
    }

    // MARK: Pointer

    /// The system pointer hovers over the view (nil: it left), at `position`
    /// on the display. `point` is where it is in the view, which says whether
    /// it moved.
    func hover(_ position: GuestPointerMapping.Position?, at point: CGPoint?, beginning: Bool) {
        if capturing, isActive, routesPointer {
            hoverInside = position?.inside ?? false
            return
        }
        guard routesMouse else { return }
        if let point {
            // A hover that starts without the pointer having moved, as the
            // Simulator makes at the view's centre, is not the pointer's.
            if !beginning, let lastHover, lastHover != point { sink?.hardwarePointerUsed() }
            lastHover = point
        } else {
            lastHover = nil
        }
        send(pointer.hover(position))
    }

    /// A touch of the pointer's buttons, with the buttons down in its event.
    func pointerTouch(_ phase: GuestPointerTracker.Phase, at position: GuestPointerMapping.Position?, buttons: UInt32) {
        guard routesMouse else { return }
        if phase == .began { sink?.hardwarePointerUsed() }
        send(pointer.touch(phase, at: position, buttons: buttons))
    }

    /// A scroll gesture reported `translation` since its last report.
    func scroll(translation: CGPoint, continuous: Bool, beginning: Bool) {
        guard routesMouse else { return }
        if beginning {
            continuousScroll.reset()
            discreteScroll.reset()
        }
        let steps: (wheel: Int32, horizontalWheel: Int32)
        if continuous {
            steps = continuousScroll.add(translationX: translation.x, translationY: translation.y)
        } else {
            steps = discreteScroll.add(translationX: translation.x, translationY: translation.y)
        }
        guard steps.wheel != 0 || steps.horizontalWheel != 0 else { return }
        sink?.hardwarePointerUsed()
        // The buttons are the pointer's: the mouse reports none of its own
        // until Android captures the pointer.
        sink?.hardwareMouse(dx: 0, dy: 0, buttons: 0, wheel: steps.wheel, horizontalWheel: steps.horizontalWheel)
    }

    /// Ends the pointer's contact and lifts its buttons.
    func releasePointer() {
        lastHover = nil
        continuousScroll.reset()
        discreteScroll.reset()
        send(pointer.leave())
    }

    private func send(_ reports: [PointerReport]) {
        for report in reports { sink?.hardwarePointer(report) }
    }

    /// Lifts everything: the keys, the pointer's buttons and its contact.
    func releaseAll() {
        releaseKeys()
        releasePointer()
    }
}
