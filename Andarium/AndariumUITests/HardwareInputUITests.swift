// SPDX-License-Identifier: GPL-2.0-or-later

import UIKit
import XCTest

/// The hardware keyboard and mouse in the real application. The launch asks for
/// the input probe (`ANDARIUM_INPUT_PROBE`): the session opens with no guest
/// behind it, and what the guest's keyboard and mouse would receive goes to a
/// log the session shows as one accessibility element. XCUITest's synthetic
/// keys and pointer events are the hardware.
///
/// XCUITest has no pointer on an iPhone ("Pointer events are not supported for
/// this device"), so the iPhone's tests are the keys and the absence of the
/// iPad's mouse setting.
@MainActor
final class HardwareInputUITests: XCTestCase {
    private var app: XCUIApplication!

    override func setUp() async throws {
        continueAfterFailure = false
        app = XCUIApplication()
        app.launchEnvironment["ANDARIUM_INPUT_PROBE"] = "1"
        app.launch()
        XCTAssertTrue(probe.waitForExistence(timeout: 90), "the session did not open")
        // The display takes the keyboard focus once it is in a window.
        sleep(2)
    }

    // MARK: Reading the guest's end

    private var probe: XCUIElement { app.descendants(matching: .any)["inputProbeLog"] }

    private var lines: [String] {
        probe.label.split(separator: "\n").map(String.init)
    }

    /// Waits for a line that satisfies `matches`; false at the deadline.
    @discardableResult
    private func waitForLine(timeout: TimeInterval = 20, _ matches: (String) -> Bool) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if lines.contains(where: matches) { return true }
            usleep(200_000)
        }
        return lines.contains(where: matches)
    }

    private func waitForLine(containing text: String, timeout: TimeInterval = 20) -> Bool {
        waitForLine(timeout: timeout) { $0.contains(text) }
    }

    /// The pointer lines as (x, y, buttons, inRange).
    private var pointerReports: [(x: Int, y: Int, buttons: Int, inRange: Bool)] {
        lines.compactMap { line in
            let words = line.split(separator: " ")
            guard words.count == 5, words[0] == "pointer", let x = Int(words[1]), let y = Int(words[2]),
                  words[3].hasPrefix("b"), let buttons = Int(words[3].dropFirst()) else { return nil }
            return (x, y, buttons, words[4] == "in")
        }
    }

    /// The mouse lines as (dx, dy, buttons, wheel, horizontalWheel).
    private var mouseReports: [(dx: Int, dy: Int, buttons: Int, wheel: Int, horizontalWheel: Int)] {
        lines.compactMap { line in
            let words = line.split(separator: " ")
            guard words.count == 6, words[0] == "mouse", let dx = Int(words[1]), let dy = Int(words[2]),
                  let buttons = Int(words[3].dropFirst()), let wheel = Int(words[4].dropFirst()),
                  let horizontal = Int(words[5].dropFirst()) else { return nil }
            return (dx, dy, buttons, wheel, horizontal)
        }
    }

    private func index(of line: String) -> Int? { lines.firstIndex(of: line) }

    /// Does `action` until a line that satisfies `matches` is in the log, up to
    /// `attempts` times. The Simulator reports its keyboard and its mouse only
    /// around each synthetic event, and the first event after a launch can come
    /// before that report and reach no view (the display's own log shows no
    /// press at all); a person would press again.
    @discardableResult
    private func perform(_ action: () -> Void, until matches: (String) -> Bool, attempts: Int = 4) -> Bool {
        for _ in 0..<attempts {
            action()
            if waitForLine(timeout: 8, matches) { return true }
        }
        return false
    }

    /// The same for a condition that is not in the log.
    private func perform(_ action: () -> Void, untilTrue condition: () -> Bool, attempts: Int = 4) -> Bool {
        for _ in 0..<attempts {
            action()
            let deadline = Date().addingTimeInterval(8)
            while Date() < deadline {
                if condition() { return true }
                usleep(200_000)
            }
        }
        return false
    }

    private func waitForValue(of element: XCUIElement, _ value: String, timeout: TimeInterval = 15) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if element.value as? String == value { return true }
            usleep(200_000)
        }
        return element.value as? String == value
    }

    private var isPad: Bool { UIDevice.current.userInterfaceIdiom == .pad }

    // MARK: Settings

    private func openSettings() {
        let keys = app.buttons["Side keys"]
        XCTAssertTrue(keys.waitForExistence(timeout: 40), "the side keys are not on screen")
        keys.tap()
        let settings = app.buttons["Settings"]
        XCTAssertTrue(settings.waitForExistence(timeout: 20), "the side keys did not open")
        settings.tap()
        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 30), "Settings did not open")
    }

    private func closeSettings() {
        // A tap can be lost while the host is loaded: tap Done again if the
        // sheet is still there.
        let bar = app.navigationBars["Settings"]
        for _ in 0..<3 where bar.exists {
            app.buttons["Done"].tap()
            if bar.waitForNonExistence(timeout: 10) { break }
        }
        XCTAssertTrue(bar.waitForNonExistence(timeout: 20), "Settings did not close")
        sleep(1)
    }

    // MARK: Keys

    func testKeysReachTheGuestAsPositionsAfterTheKeyboardIsPlugged() {
        XCTAssertTrue(perform({ app.typeKey("a", modifierFlags: []) }, until: { $0 == "key 0x4 up" }), "log: \(lines)")
        let plug = index(of: "connect keyboard 1")
        let down = index(of: "key 0x4 down")
        let up = index(of: "key 0x4 up")
        XCTAssertNotNil(plug, "log: \(lines)")
        XCTAssertNotNil(down, "log: \(lines)")
        XCTAssertNotNil(up, "log: \(lines)")
        if let plug, let down, let up {
            XCTAssertLessThan(plug, down)
            XCTAssertLessThan(down, up)
        }
    }

    func testModifiersAreTheirOwnKeysAroundTheKey() {
        XCTAssertTrue(
            perform({ app.typeKey("c", modifierFlags: .command) }, until: { $0 == "key 0xe3 up" }), "log: \(lines)")
        // Whatever attempt was the first to arrive, a whole press is four lines.
        let keys = lines.filter { $0.hasPrefix("key ") }
        XCTAssertEqual(keys.suffix(4), ["key 0xe3 down", "key 0x6 down", "key 0x6 up", "key 0xe3 up"])
        let beforeShiftControl = keys.count

        app.typeKey("a", modifierFlags: [.shift, .control])
        XCTAssertTrue(waitForLine(containing: "key 0x4 up"), "log: \(lines)")
        let more = Array(lines.filter { $0.hasPrefix("key ") }.dropFirst(beforeShiftControl))
        // Both modifiers go down before the key and come up after it.
        XCTAssertEqual(more.count, 6, "log: \(lines)")
        XCTAssertEqual(Set(more.prefix(2)), ["key 0xe0 down", "key 0xe1 down"])
        XCTAssertEqual(Array(more[2...3]), ["key 0x4 down", "key 0x4 up"])
        XCTAssertEqual(Set(more.suffix(2)), ["key 0xe0 up", "key 0xe1 up"])
    }

    func testTheKeysThatAreCombinationsOfTheSystemReachTheGuestToo() {
        // Command-K, Command-W and Control-Space are the guest's.
        perform({ app.typeKey("k", modifierFlags: .command) }, until: { $0 == "key 0xe3 up" })
        app.typeKey("w", modifierFlags: .command)
        app.typeKey(XCUIKeyboardKey.space.rawValue, modifierFlags: .control)
        XCTAssertTrue(waitForLine(containing: "key 0x2c up"), "log: \(lines)")
        let downs = lines.filter { $0.hasSuffix("down") }
        XCTAssertTrue(downs.contains("key 0xe3 down"))
        XCTAssertTrue(downs.contains("key 0xe0 down"))
        XCTAssertTrue(downs.contains("key 0xe down"))
        XCTAssertTrue(downs.contains("key 0x1a down"))
        XCTAssertTrue(downs.contains("key 0x2c down"))
    }

    func testNoSoftwareKeyboardAppearsWhenTheGuestTakesTheKeys() {
        XCTAssertTrue(perform({ app.typeKey("a", modifierFlags: []) }, until: { $0 == "key 0x4 up" }), "log: \(lines)")
        sleep(1)
        XCTAssertFalse(app.keyboards.firstMatch.exists, "the guest's focus raised the software keyboard")
    }

    // MARK: A sheet has the keys

    func testASheetsTextFieldTakesTheKeysWhileItIsOpenAndTheGuestTakesThemBack() {
        XCTAssertTrue(perform({ app.typeKey("z", modifierFlags: []) }, until: { $0 == "key 0x1d up" }), "log: \(lines)")
        let before = lines.filter { $0.hasPrefix("key ") }.count

        openSettings()
        let field = app.textFields["inputProbeField"]
        XCTAssertTrue(field.waitForExistence(timeout: 10), "Settings has no probe field")
        field.tap()
        let text = { (field.value as? String) ?? "" }
        XCTAssertTrue(
            perform({ app.typeKey("h", modifierFlags: []) }, untilTrue: { text().contains("h") }),
            "the field took no key: \(text())")
        app.typeKey("i", modifierFlags: [])
        XCTAssertTrue(
            perform({}, untilTrue: { text().hasSuffix("i") }, attempts: 1), "the field took no second key: \(text())")
        sleep(1)
        XCTAssertEqual(
            lines.filter { $0.hasPrefix("key ") }.count, before, "the guest received the keys the sheet took: \(lines)")

        closeSettings()
        XCTAssertTrue(
            perform({ app.typeKey("c", modifierFlags: []) }, until: { $0 == "key 0x6 up" }),
            "the guest did not take the keys back: \(lines)")
    }

    func testASheetWithNoFieldTakesNoKeyFromTheGuestEither() {
        openSettings()
        app.typeKey("d", modifierFlags: [])
        sleep(2)
        XCTAssertFalse(lines.contains { $0.contains("key 0x7") }, "log: \(lines)")
        closeSettings()
        XCTAssertTrue(perform({ app.typeKey("d", modifierFlags: []) }, until: { $0 == "key 0x7 up" }), "log: \(lines)")
    }

    // MARK: Pointer (iPad)

    func testThePointerHoversClicksDragsAndScrollsAsAMouse() throws {
        try XCTSkipUnless(isPad, "XCUITest has no pointer on an iPhone")
        let window = app.windows.firstMatch
        let a = window.coordinate(withNormalizedOffset: CGVector(dx: 0.3, dy: 0.3))
        let b = window.coordinate(withNormalizedOffset: CGVector(dx: 0.6, dy: 0.5))

        XCTAssertTrue(
            perform({ a.hover(); b.hover() }, until: { $0.hasPrefix("pointer ") && $0.hasSuffix(" in") }),
            "the mouse was not plugged: \(lines)")
        XCTAssertTrue(lines.contains("connect mouse 1"), "log: \(lines)")
        // Where the pointer is on the display, on the touchscreen's range.
        let hovered = try XCTUnwrap(pointerReports.last { $0.inRange && $0.buttons == 0 })
        XCTAssertEqual(Double(hovered.x), 0.6 * 32767, accuracy: 0.15 * 32767)
        XCTAssertEqual(Double(hovered.y), 0.5 * 32767, accuracy: 0.15 * 32767)

        b.click()
        XCTAssertTrue(waitForLine { $0.hasPrefix("pointer ") && $0.contains(" b1 ") }, "a click: \(lines)")
        XCTAssertTrue(waitForLine { $0.hasPrefix("pointer ") && $0.contains(" b0 in") }, "the button's release: \(lines)")

        b.rightClick()
        XCTAssertTrue(waitForLine { $0.hasPrefix("pointer ") && $0.contains(" b2 ") }, "a right click: \(lines)")

        let reportsBeforeDrag = pointerReports.count
        a.click(forDuration: 0.2, thenDragTo: b)
        XCTAssertTrue(waitForLine(timeout: 20) { _ in self.pointerReports.count > reportsBeforeDrag + 2 }, "log: \(lines)")
        let drag = pointerReports.dropFirst(reportsBeforeDrag).filter { $0.buttons == 1 }
        XCTAssertGreaterThanOrEqual(Set(drag.map { $0.x }).count, 2, "a drag moves: \(drag)")

        XCTAssertTrue(
            perform(
                { b.scroll(byDeltaX: 0, deltaY: 60) },
                until: { $0.hasPrefix("mouse 0 0 b0 w") && !$0.hasPrefix("mouse 0 0 b0 w0 ") }),
            "scroll: \(lines)")
        XCTAssertTrue(
            perform(
                { b.scroll(byDeltaX: 40, deltaY: 0) },
                until: { $0.hasPrefix("mouse 0 0 b0 w0 h") && !$0.hasSuffix(" h0") }),
            "scroll: \(lines)")

        // The wheel goes on the guest's mouse with no buttons of its own, and
        // the system pointer is never also a touch of the touchscreen.
        XCTAssertTrue(mouseReports.allSatisfy { $0.buttons == 0 && $0.dx == 0 && $0.dy == 0 })
        // Record what the synthetic scroll was, for the hardware check.
        let scrolls = mouseReports.map { "w\($0.wheel) h\($0.horizontalWheel)" }
        let note = XCTAttachment(string: "scrolls for deltaY 60 then deltaX 40: \(scrolls)")
        note.lifetime = .keepAlways
        add(note)
    }

    func testUsingThePointerAsTouchKeepsTheMouseOffTheGuest() throws {
        try XCTSkipUnless(isPad, "XCUITest has no pointer on an iPhone")
        openSettings()
        let toggle = app.switches["pointerAsTouch"]
        XCTAssertTrue(toggle.waitForExistence(timeout: 10), "Settings has no pointer row")
        XCTAssertEqual(toggle.value as? String, "0", "the setting starts off")
        // The switch is at the row's trailing end.
        toggle.coordinate(withNormalizedOffset: CGVector(dx: 0.94, dy: 0.5)).tap()
        XCTAssertTrue(waitForValue(of: toggle, "1"), "the setting did not turn on: \(String(describing: toggle.value))")
        closeSettings()

        let window = app.windows.firstMatch
        let a = window.coordinate(withNormalizedOffset: CGVector(dx: 0.3, dy: 0.3))
        let b = window.coordinate(withNormalizedOffset: CGVector(dx: 0.6, dy: 0.5))
        a.hover()
        b.hover()
        b.click()
        sleep(4)
        XCTAssertFalse(lines.contains { $0.hasPrefix("connect mouse") || $0.hasPrefix("pointer ") }, "log: \(lines)")

        openSettings()
        let again = app.switches["pointerAsTouch"]
        again.coordinate(withNormalizedOffset: CGVector(dx: 0.94, dy: 0.5)).tap()
        XCTAssertTrue(waitForValue(of: again, "0"), "the setting did not turn off: \(String(describing: again.value))")
        closeSettings()
        XCTAssertTrue(
            perform({ a.hover(); b.hover() }, until: { $0 == "connect mouse 1" }), "the mouse did not return: \(lines)")
    }

    // MARK: iPhone

    func testAnIPhoneHasNoMouseSettingAndNoMouse() throws {
        try XCTSkipIf(isPad, "an iPad has the pointer setting")
        XCTAssertTrue(perform({ app.typeKey("a", modifierFlags: []) }, until: { $0 == "key 0x4 up" }), "log: \(lines)")
        openSettings()
        XCTAssertTrue(app.textFields["inputProbeField"].waitForExistence(timeout: 10))
        XCTAssertFalse(app.switches["pointerAsTouch"].exists, "an iPhone has no pointer to give the guest")
        closeSettings()
        XCTAssertFalse(lines.contains { $0.hasPrefix("connect mouse") })
    }
}
