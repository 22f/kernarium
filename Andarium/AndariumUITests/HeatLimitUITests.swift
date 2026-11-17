// SPDX-License-Identifier: GPL-2.0-or-later

import XCTest

/// Settings' "When hot" and the heat notice's alert in the real application.
/// The launch asks for the input probe with iOS's thermal state stated as
/// serious (`ANDARIUM_THERMAL_STATE`): the session opens with no guest behind
/// it, the thermal session runs against a stand-in for the guest's HAL, and
/// what Android would be told goes to the probe's log as "heat told <severity>".
/// No device is heated: the state is stated, never felt.
@MainActor
final class HeatLimitUITests: XCTestCase {
    private let notice = "Frame rate lowered while this device is hot"
    private let warning =
        "This device gets hotter and the battery drains faster. iOS may dim the screen, slow everything down or close Andarium."

    private func launch() -> XCUIApplication {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchEnvironment["ANDARIUM_INPUT_PROBE"] = "1"
        app.launchEnvironment["ANDARIUM_THERMAL_STATE"] = "serious"
        app.launch()
        XCTAssertTrue(app.buttons["Side keys"].waitForExistence(timeout: 90), "the session did not open")
        return app
    }

    private func told(_ app: XCUIApplication) -> [String] {
        app.descendants(matching: .any)["inputProbeLog"].label.split(separator: "\n")
            .map(String.init).filter { $0.hasPrefix("heat told") }
    }

    private func waitForLast(_ expected: String, in app: XCUIApplication, timeout: TimeInterval = 20) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if told(app).last == expected { return true }
            usleep(200_000)
        }
        return told(app).last == expected
    }

    private func openSettings(_ app: XCUIApplication) {
        app.buttons["Side keys"].tap()
        let settings = app.buttons["Settings"]
        XCTAssertTrue(settings.waitForExistence(timeout: 20), "the side keys did not open")
        settings.tap()
        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 30), "Settings did not open")
    }

    /// Scrolls Settings until `element` can be tapped.
    private func reveal(_ element: XCUIElement, in app: XCUIApplication) {
        XCTAssertTrue(element.waitForExistence(timeout: 30), "\(element) is not in Settings")
        var swipes = 0
        while !element.isHittable && swipes < 8 {
            app.swipeUp()
            swipes += 1
        }
        XCTAssertTrue(element.isHittable, "\(element) cannot be reached")
    }

    /// The "When hot" row: a menu, which UIKit gives its own element type.
    private func menu(_ app: XCUIApplication) -> XCUIElement {
        app.descendants(matching: .any)["heat-limit-menu"]
    }

    /// A choice of the open menu, whatever element type UIKit gives it.
    private func choice(_ title: String, in app: XCUIApplication) -> XCUIElement {
        app.descendants(matching: .any).matching(NSPredicate(format: "label BEGINSWITH %@", title)).firstMatch
    }

    private func value(_ row: XCUIElement) -> String { (row.value as? String) ?? "none" }

    private func waitForValue(_ expected: String, of row: XCUIElement, timeout: TimeInterval = 15) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if value(row) == expected { return true }
            usleep(200_000)
        }
        return value(row) == expected
    }

    private func choose(_ title: String, in app: XCUIApplication) {
        let row = menu(app)
        reveal(row, in: app)
        row.tap()
        let item = choice(title, in: app)
        XCTAssertTrue(item.waitForExistence(timeout: 10), "the menu has no \(title): \(app.debugDescription)")
        item.tap()
    }

    private func text(_ app: XCUIApplication, containing words: String) -> XCUIElement {
        app.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", words)).firstMatch
    }

    // MARK: Settings

    func testThePerformanceSectionHasTheRowAMenuAndAFootnoteInItsFourStates() {
        let app = launch()
        // The policy holds the device hot from the stated state.
        XCTAssertTrue(waitForLast("heat told severe", in: app), "Android was not told: \(told(app))")
        openSettings(app)
        let row = menu(app)
        reveal(row, in: app)
        XCTAssertTrue(app.staticTexts["Performance"].exists, "no Performance header")
        XCTAssertEqual(row.label, "When hot")
        XCTAssertEqual(value(row), "Limit frame rate")
        // Hot, limit on.
        XCTAssertTrue(
            text(app, containing: "This device is hot. The frame rate is limited until it cools.")
                .waitForExistence(timeout: 20), "the hot footnote is not up")

        // The menu holds the two choices, the current one on.
        row.tap()
        let limit = choice("Limit frame rate", in: app)
        let full = choice("Full frame rate", in: app)
        XCTAssertTrue(limit.waitForExistence(timeout: 10), "the menu has no Limit frame rate: \(app.debugDescription)")
        XCTAssertTrue(full.exists, "the menu has no Full frame rate")
        XCTAssertTrue(limit.label.contains("Lets this device cool down"), "the choice says \(limit.label)")
        XCTAssertTrue(full.label.contains("Hotter, and the battery drains faster"), "the choice says \(full.label)")
        full.tap()

        // Hot, limit off: Android is told nothing at once, and the footnote warns.
        XCTAssertTrue(waitForValue("Full frame rate", of: row), "the row says \(value(row))")
        XCTAssertTrue(waitForLast("heat told none", in: app), "Android heard \(told(app))")
        XCTAssertTrue(
            text(app, containing: "This device is hot. The frame rate is not limited.").waitForExistence(timeout: 20))
        XCTAssertTrue(text(app, containing: warning).exists, "the warning is not in the footnote")

        // Back on: Android is told the severity at once.
        choose("Limit frame rate", in: app)
        XCTAssertTrue(waitForValue("Limit frame rate", of: row), "the row says \(value(row))")
        XCTAssertTrue(waitForLast("heat told severe", in: app), "Android heard \(told(app))")
        XCTAssertTrue(
            text(app, containing: "This device is hot. The frame rate is limited until it cools.")
                .waitForExistence(timeout: 20))
    }

    // MARK: The notice

    func testTheNoticesWordsOpenTheAlertCancelLeavesItAndTurnOffRemovesItAndFlipsTheRow() {
        let app = launch()
        let words = app.buttons["thermal-notice-ask"]
        XCTAssertTrue(words.waitForExistence(timeout: 60), "the notice did not come")
        XCTAssertEqual(words.label, notice)
        XCTAssertTrue(app.buttons["Dismiss"].exists, "the notice has no Dismiss")

        // Cancel leaves the notice and the limit as they were.
        words.tap()
        let alert = app.alerts["Turn off the frame rate limit?"]
        XCTAssertTrue(alert.waitForExistence(timeout: 20), "the words opened no alert")
        XCTAssertTrue(
            alert.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", warning)).firstMatch.exists,
            "the alert has no warning")
        XCTAssertTrue(
            alert.staticTexts.matching(NSPredicate(
                format: "label CONTAINS %@", "You can turn it back on in Settings, under Performance.")).firstMatch.exists,
            "the alert does not say the way back")
        alert.buttons["Cancel"].tap()
        XCTAssertTrue(alert.waitForNonExistence(timeout: 10))
        XCTAssertTrue(words.exists, "Cancel took the notice away")
        XCTAssertEqual(told(app).last, "heat told severe", "Cancel changed what Android hears: \(told(app))")

        // Turn Off is the one tap: the notice goes and Android hears none.
        words.tap()
        XCTAssertTrue(alert.waitForExistence(timeout: 20), "the words opened no alert the second time")
        alert.buttons["Turn Off"].tap()
        XCTAssertTrue(alert.waitForNonExistence(timeout: 10))
        XCTAssertTrue(words.waitForNonExistence(timeout: 10), "Turn Off left the notice")
        XCTAssertTrue(waitForLast("heat told none", in: app), "Android heard \(told(app))")

        openSettings(app)
        let row = menu(app)
        reveal(row, in: app)
        XCTAssertEqual(value(row), "Full frame rate", "the row did not follow Turn Off")
        XCTAssertTrue(text(app, containing: warning).waitForExistence(timeout: 20))
    }

    func testDismissOnlyDismissesTheNotice() {
        let app = launch()
        let words = app.buttons["thermal-notice-ask"]
        XCTAssertTrue(words.waitForExistence(timeout: 60), "the notice did not come")
        app.buttons["Dismiss"].tap()
        XCTAssertTrue(words.waitForNonExistence(timeout: 10), "Dismiss left the notice")
        XCTAssertFalse(app.alerts.firstMatch.exists, "Dismiss asked something")
        XCTAssertEqual(told(app).last, "heat told severe", "Dismiss changed what Android hears: \(told(app))")
        openSettings(app)
        let row = menu(app)
        reveal(row, in: app)
        XCTAssertEqual(value(row), "Limit frame rate", "Dismiss changed the choice")
    }

    func testWithTheLimitOffNoNoticeComesAtAnyHeat() {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchEnvironment["ANDARIUM_INPUT_PROBE"] = "1"
        app.launchEnvironment["ANDARIUM_THERMAL_STATE"] = "critical"
        app.launchEnvironment["ANDARIUM_HEAT_LIMIT"] = "0"
        app.launch()
        XCTAssertTrue(app.buttons["Side keys"].waitForExistence(timeout: 90), "the session did not open")
        // Android hears its first record, which says none, and nothing hot after it.
        let deadline = Date().addingTimeInterval(20)
        while told(app).isEmpty && Date() < deadline { usleep(200_000) }
        XCTAssertEqual(told(app).first, "heat told none")
        // The notice would have come within moments at the stated critical state.
        sleep(8)
        XCTAssertFalse(app.buttons["thermal-notice-ask"].exists, "a notice came with the limit off")
        XCTAssertFalse(told(app).contains { $0 != "heat told none" }, "Android heard \(told(app))")
        openSettings(app)
        let row = menu(app)
        reveal(row, in: app)
        XCTAssertEqual(value(row), "Full frame rate")
        XCTAssertTrue(
            text(app, containing: "This device is hot. The frame rate is not limited.").waitForExistence(timeout: 20),
            "the footnote does not know the device is hot")
    }
}
