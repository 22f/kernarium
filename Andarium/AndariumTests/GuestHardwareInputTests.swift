// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CoreGraphics
import GuestHostCore
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// A sink that keeps what the guest's input hands it.
@MainActor
private final class RecordingSink: GuestHardwareSink {
    enum Event: Equatable {
        case key(UInt32, Bool)
        case pointer(PointerReport)
        case mouse(Int32, Int32, UInt32, Int32, Int32)
        case keyboardUsed
        case pointerUsed
    }

    private(set) var events: [Event] = []

    func hardwareKey(usage: UInt32, pressed: Bool) { events.append(.key(usage, pressed)) }
    func hardwarePointer(_ report: PointerReport) { events.append(.pointer(report)) }
    func hardwareMouse(dx: Int32, dy: Int32, buttons: UInt32, wheel: Int32, horizontalWheel: Int32) {
        events.append(.mouse(dx, dy, buttons, wheel, horizontalWheel))
    }
    func hardwareKeyboardUsed() { events.append(.keyboardUsed) }
    func hardwarePointerUsed() { events.append(.pointerUsed) }
}

/// What the guest view does with hardware keys and the system pointer, before
/// the application decides what the guest has.
@MainActor
@Suite(.serialized)
struct GuestHardwareInputTests {
    private func makeInput(_ sink: RecordingSink, active: Bool = true, routed: Bool = true) -> GuestHardwareInput {
        let input = GuestHardwareInput(clock: { 10 })
        input.sink = sink
        input.isActive = active
        input.routesPointer = routed
        return input
    }

    private let inside = GuestPointerMapping.Position(x: 300, y: 400, inside: true)
    private let outside = GuestPointerMapping.Position(x: 0, y: 400, inside: false)

    // MARK: Keys

    @Test func keysGoOnlyWhileTheInputIsActive() {
        let sink = RecordingSink()
        let input = makeInput(sink, active: false)
        input.keyDown(usage: 0x04, alphaShift: false)
        #expect(sink.events.isEmpty)
        input.isActive = true
        input.keyDown(usage: 0x04, alphaShift: false)
        input.keyDown(usage: 0x04, alphaShift: false)
        input.keyUp(usage: 0x04)
        #expect(sink.events == [.keyboardUsed, .key(0x04, true), .keyboardUsed, .key(0x04, false)])
    }

    @Test func aUsageThatNamesNoKeyIsDropped() {
        let sink = RecordingSink()
        let input = makeInput(sink)
        input.keyDown(usage: 0x03, alphaShift: false)
        input.keyDown(usage: 0xE8, alphaShift: false)
        #expect(sink.events.isEmpty)
    }

    @Test func goingInactiveLiftsKeysAndPointerNewestFirst() {
        let sink = RecordingSink()
        let input = makeInput(sink)
        input.keyDown(usage: 0x04, alphaShift: false)
        input.keyDown(usage: 0xE3, alphaShift: false)
        input.hover(inside, at: CGPoint(x: 1, y: 1), beginning: true)
        input.pointerTouch(.began, at: inside, buttons: HardwareButtons.primary)
        let before = sink.events.count
        input.isActive = false
        #expect(sink.events[before...] == [
            .key(0xE3, false), .key(0x04, false),
            .pointer(PointerReport(x: 300, y: 400, buttons: 0, inRange: false)),
        ])
    }

    // MARK: Pointer

    @Test func theSystemPointerGoesToTheMouseOnlyWhenTheApplicationRoutesIt() {
        let sink = RecordingSink()
        let input = makeInput(sink, routed: false)
        input.hover(inside, at: CGPoint(x: 1, y: 1), beginning: true)
        input.pointerTouch(.began, at: inside, buttons: HardwareButtons.primary)
        input.scroll(translation: CGPoint(x: 0, y: 64), continuous: true, beginning: true)
        #expect(sink.events.isEmpty)
        #expect(!input.hidesSystemPointer)
        input.routesPointer = true
        #expect(input.hidesSystemPointer)
    }

    @Test func aHoverThatStartsWithoutMovingIsNotTheMousesUse() {
        let sink = RecordingSink()
        let input = makeInput(sink)
        // The Simulator hovers at the view's centre when its scene activates.
        input.hover(inside, at: CGPoint(x: 50, y: 50), beginning: true)
        input.hover(inside, at: CGPoint(x: 50, y: 50), beginning: false)
        #expect(!sink.events.contains(.pointerUsed))
        input.hover(inside, at: CGPoint(x: 51, y: 50), beginning: false)
        #expect(sink.events.contains(.pointerUsed))
    }

    @Test func aDragGoesOnPastTheEdgeAndItsReleaseIsReportedWhereverItHappens() {
        let sink = RecordingSink()
        let input = makeInput(sink)
        input.pointerTouch(.began, at: inside, buttons: HardwareButtons.primary)
        input.pointerTouch(.moved, at: outside, buttons: HardwareButtons.primary)
        input.pointerTouch(.ended, at: outside, buttons: 0)
        let reports = sink.events.compactMap { event -> PointerReport? in
            if case let .pointer(report) = event { return report }
            return nil
        }
        #expect(reports.first == PointerReport(x: 300, y: 400, buttons: HardwareButtons.primary, inRange: true))
        #expect(reports.dropFirst().first?.buttons == HardwareButtons.primary)
        #expect(reports.last?.buttons == 0)
    }

    @Test func aTouchThatBeginsBesideTheDisplayIsNotTheGuests() {
        let sink = RecordingSink()
        let input = makeInput(sink)
        input.pointerTouch(.began, at: outside, buttons: HardwareButtons.primary)
        input.pointerTouch(.moved, at: inside, buttons: HardwareButtons.primary)
        input.pointerTouch(.ended, at: inside, buttons: 0)
        let reports = sink.events.filter { if case .pointer = $0 { true } else { false } }
        #expect(reports.isEmpty)
    }

    @Test func aWheelAndATrackpadScrollAreDifferentDetents() {
        let sink = RecordingSink()
        let input = makeInput(sink)
        input.scroll(translation: CGPoint(x: 0, y: -32), continuous: true, beginning: true)
        input.scroll(translation: CGPoint(x: 0, y: -32), continuous: true, beginning: false)
        input.scroll(translation: CGPoint(x: 0, y: -20), continuous: false, beginning: true)
        let wheels = sink.events.compactMap { event -> Int32? in
            if case let .mouse(_, _, _, wheel, _) = event { return wheel }
            return nil
        }
        // Half of a trackpad's 64 points is half a detent in the wheel's 1/120
        // units, and a wheel's 20 points are a whole one.
        #expect(wheels == [-60, -60, -120])
    }

    @Test func whileTheGuestCapturesThePointerTheSystemsPointerSaysNothingButWhereItIs() {
        let sink = RecordingSink()
        let input = makeInput(sink)
        input.hover(inside, at: CGPoint(x: 1, y: 1), beginning: true)
        #expect(input.pointerIsOverDisplay)
        input.capturing = true
        // The contact ended, but the pointer is still over the display.
        #expect(input.pointerIsOverDisplay)
        let before = sink.events.count
        input.hover(outside, at: CGPoint(x: -4, y: 1), beginning: false)
        input.pointerTouch(.began, at: inside, buttons: HardwareButtons.primary)
        input.scroll(translation: CGPoint(x: 0, y: -64), continuous: true, beginning: true)
        #expect(sink.events.count == before)
        #expect(!input.pointerIsOverDisplay)
        input.hover(inside, at: CGPoint(x: 5, y: 1), beginning: false)
        #expect(input.pointerIsOverDisplay)
        input.capturing = false
        #expect(!input.pointerIsOverDisplay)
    }

    @Test func theViewIsToldWhenTheSystemPointerShouldHideOrShow() {
        let sink = RecordingSink()
        let input = GuestHardwareInput(clock: { 10 })
        input.sink = sink
        var told = 0
        input.hidesSystemPointerDidChange = { told += 1 }
        input.isActive = true
        input.routesPointer = true
        input.routesPointer = true
        input.isActive = false
        #expect(told == 3)
    }

    // MARK: The view

    @Test func theViewTakesTheMouseRecognizersOnlyWithAnInput() {
        let view = GuestMetalView(frame: CGRect(x: 0, y: 0, width: 200, height: 100), device: nil)
        let recognizers = { view.gestureRecognizers ?? [] }
        #expect(!recognizers().contains { $0 is UIHoverGestureRecognizer })
        #expect(!view.interactions.contains { $0 is UIPointerInteraction })

        let input = GuestHardwareInput()
        view.hardwareInput = input
        #expect(recognizers().contains { $0 is UIHoverGestureRecognizer })
        let scrolls = recognizers().compactMap { $0 as? UIPanGestureRecognizer }
        #expect(scrolls.count == 2)
        #expect(Set(scrolls.map { $0.allowedScrollTypesMask.rawValue }).count == 2)
        #expect(scrolls.allSatisfy { $0.maximumNumberOfTouches == 0 })
        #expect(view.interactions.contains { $0 is UIPointerInteraction })

        view.hardwareInput = nil
        #expect(!recognizers().contains { $0 is UIHoverGestureRecognizer })
        #expect(!recognizers().contains { $0 is UIPanGestureRecognizer })
        #expect(!view.interactions.contains { $0 is UIPointerInteraction })
    }

    @Test func theViewHidesTheSystemPointerOnlyWhileTheMouseIsTheGuests() throws {
        let view = GuestMetalView(frame: CGRect(x: 0, y: 0, width: 200, height: 100), device: nil)
        let input = GuestHardwareInput()
        view.hardwareInput = input
        let interaction = try #require(view.interactions.compactMap { $0 as? UIPointerInteraction }.first)
        let style = { view.pointerInteraction(interaction, styleFor: UIPointerRegion(rect: view.bounds)) }
        #expect(style() == nil)
        input.isActive = true
        #expect(style() == nil)
        input.routesPointer = true
        #expect(style() != nil)
        input.isActive = false
        #expect(style() == nil)
    }

    @Test func aViewWithoutAnInputLeavesEveryTouchToTheTouchscreen() {
        let view = GuestMetalView(frame: CGRect(x: 0, y: 0, width: 200, height: 100), device: nil)
        var reports = 0
        view.touchDidBegin = { reports += 1 }
        view.touchesBegan([UITouch()], with: nil)
        #expect(reports == 1)
        #expect(view.hardwareInput == nil)
    }

    @Test func settingAnotherInputLiftsWhatTheFirstHolds() {
        let view = GuestMetalView(frame: CGRect(x: 0, y: 0, width: 200, height: 100), device: nil)
        let sink = RecordingSink()
        let first = makeInput(sink)
        view.hardwareInput = first
        first.keyDown(usage: 0x04, alphaShift: false)
        view.hardwareInput = nil
        #expect(sink.events.last == .key(0x04, false))
    }

    // MARK: Positions

    /// The pointer and the touchscreen share one range, so Android's cursor
    /// is where a finger would be.
    @Test(arguments: [
        (CGSize(width: 402, height: 874), CGSize(width: 1206, height: 2622), CGSize(width: 1080, height: 2400)),
        (CGSize(width: 1194, height: 834), CGSize(width: 1194, height: 834), CGSize(width: 1194, height: 834)),
        (CGSize(width: 1000, height: 700), CGSize(width: 2000, height: 1400), CGSize(width: 1920, height: 1080)),
        (CGSize(width: 820, height: 1180), CGSize(width: 1640, height: 2360), CGSize(width: 1024, height: 768)),
    ])
    func thePointersPositionIsTheTouchscreensForTheSamePoint(bounds: CGSize, drawable: CGSize, content: CGSize) throws {
        let mapping = try #require(GuestPointerMapping(boundsSize: bounds, drawableSize: drawable, contentSize: content))
        let geometry = try #require(GuestTouchGeometry(
            coordinateSpace: .touchscreen, boundsSize: bounds, drawableSize: drawable, contentSize: content))
        let viewport = try #require(GuestDisplayViewport.aspectFit(contentSize: content, in: drawable))
        // Points of the view that fall on the display.
        let fractions: [(CGFloat, CGFloat)] = [(0.001, 0.001), (0.25, 0.4), (0.5, 0.5), (0.9, 0.1), (0.999, 0.999)]
        for (fx, fy) in fractions {
            let drawablePoint = CGPoint(x: viewport.minX + viewport.width * fx, y: viewport.minY + viewport.height * fy)
            let point = CGPoint(x: drawablePoint.x * bounds.width / drawable.width, y: drawablePoint.y * bounds.height / drawable.height)
            var contacts = GuestTouchContacts<Int>()
            let touch = try #require(contacts.begin(1, at: point, geometry: geometry).first)
            let position = try #require(mapping.position(of: point))
            #expect(position.inside)
            #expect(position.x == touch.event.x)
            #expect(position.y == touch.event.y)
        }
    }

    @Test func aPointBesideTheDisplayHasThePositionOfTheNearestEdge() throws {
        let bounds = CGSize(width: 400, height: 800)
        let mapping = try #require(GuestPointerMapping(
            boundsSize: bounds, drawableSize: bounds, contentSize: CGSize(width: 800, height: 400)))
        // The display is a band across the middle of the view.
        let above = try #require(mapping.position(of: CGPoint(x: 200, y: 10)))
        #expect(!above.inside)
        #expect(above.y == 0)
        let below = try #require(mapping.position(of: CGPoint(x: 200, y: 790)))
        #expect(!below.inside)
        #expect(below.y == GuestPointerMapping.maximum)
    }

    // MARK: The scene's pointer lock

    @Test func theRootAsksForThePointerLockWhileTheGuestWantsIt() {
        let model = AppModel(
            guest: nil, host: AppEntry.hostDevice(),
            system: SystemInfo(version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
        let controller = AndariumHostingController(rootView: Color.clear, model: model)
        #expect(!controller.prefersPointerLocked)
        model.pointerLockWanted = true
        #expect(controller.prefersPointerLocked)
        model.pointerLockWanted = false
        #expect(!controller.prefersPointerLocked)
    }

    @Test func theSettingReachesTheDriverAndIsKeptForTheInstall() {
        let model = AppModel(
            guest: nil, host: AppEntry.hostDevice(),
            system: SystemInfo(version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
        let driver = StubDriver(plan: nil)
        model.driver = driver
        #expect(!model.pointerAsTouch)
        #expect(model.pointerAsTouchToKeep == false)
        model.setPointerAsTouch(true)
        #expect(driver.pointerAsTouch)
        #expect(model.pointerAsTouchToKeep == true)
        // A launch that decided the value keeps nothing.
        model.pointerAsTouchDecidedByLaunch = true
        #expect(model.pointerAsTouchToKeep == nil)
    }

    // MARK: The UI tests' launch

    @Test func theProbeLogsWhatTheGuestWouldReceive() {
        let probe = InputProbe()
        let mouse = KeyboardMouse.probe(probe, supportsMouse: true) { _ in }
        defer { mouse.shutDown() }
        mouse.update(running: true, foreground: true)
        mouse.input.keyDown(usage: 0x04, alphaShift: false)
        mouse.input.keyUp(usage: 0x04)
        #expect(probe.lines.contains("connect keyboard 1"))
        #expect(probe.lines.suffix(2) == ["key 0x4 down", "key 0x4 up"])
        #expect(probe.text.contains("\n"))
    }

    @Test func theProbeKeepsTheNewestLinesWithinItsCapacity() {
        let probe = InputProbe()
        for number in 0..<(InputProbe.capacity + 25) { probe.record("line \(number)") }
        #expect(probe.lines.count == InputProbe.capacity)
        #expect(probe.lines.first == "line 25")
        #expect(probe.lines.last == "line \(InputProbe.capacity + 24)")
    }

    @Test func theProbeLaunchIsAHarnessLaunch() {
        let environment = [InputProbeLaunch.environmentName: "1"]
        #expect(InputProbeLaunch.requested(in: environment))
        #expect(!InputProbeLaunch.requested(in: [:]))
        #expect(!InputProbeLaunch.requested(in: [InputProbeLaunch.environmentName: "0"]))
        #expect(!AppEntry.isPersonsLaunch(overrides: LaunchOverrides(), environment: environment))
        #expect(AppEntry.isPersonsLaunch(overrides: LaunchOverrides(), environment: [:]))
    }
}
