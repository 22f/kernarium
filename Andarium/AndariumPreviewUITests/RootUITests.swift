// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

/// The root switch in the Simulator preview: the Root section is on Home only
/// where the release carries root, the switch asks before it turns root on and
/// turns it off at once, and the line under it says a pause. `-screen` names
/// a screen of `AppEntry` (`RootPreview`).
final class RootUITests: XCTestCase {
    override func setUp() {
        continueAfterFailure = false
    }

    @MainActor
    private func launch(_ screen: String) -> XCUIApplication {
        let app = XCUIApplication()
        app.launchArguments = previewTestLanguage + ["-screen", screen]
        app.launch()
        XCTAssertTrue(app.wait(for: .runningForeground, timeout: 20), "\(screen) did not open")
        return app
    }

    private func rootSwitch(_ app: XCUIApplication) -> XCUIElement {
        app.switches["home-root-switch"].firstMatch
    }

    /// The row carries the switch's identifier; the control is the switch itself or its child.
    private func flip(_ toggle: XCUIElement) {
        let control = toggle.switches.firstMatch
        (control.exists ? control : toggle).tap()
    }

    @MainActor
    func testAReleaseWithoutRootHasNoRootSection() {
        let app = launch("home")
        XCTAssertTrue(app.staticTexts["Virtual machine"].waitForExistence(timeout: 20))
        XCTAssertFalse(rootSwitch(app).exists, "the Root row is on Home for a release that has no root")
        XCTAssertFalse(app.staticTexts["Root access"].exists)
    }

    @MainActor
    func testTheSectionSitsBetweenCapabilitiesAndBackupAndStartsOff() {
        let app = launch("home-root")
        let toggle = rootSwitch(app)
        XCTAssertTrue(toggle.waitForExistence(timeout: 20), "no Root row")
        XCTAssertEqual(toggle.value as? String, "0")
        XCTAssertTrue(app.staticTexts["Apps you allow in Magisk can run as root. Takes effect at the next start."].exists)
        XCTAssertFalse(app.staticTexts["Paused after a failed start"].exists)
        // Capabilities is above it and Backup below it, in the order VoiceOver reads.
        let capabilities = app.staticTexts["Capabilities"].firstMatch
        let root = app.staticTexts["Root"].firstMatch
        let backup = app.staticTexts["Backup"].firstMatch
        XCTAssertTrue(capabilities.exists && root.exists && backup.exists)
        XCTAssertLessThan(capabilities.frame.minY, root.frame.minY)
        XCTAssertLessThan(root.frame.minY, backup.frame.minY)
    }

    @MainActor
    func testTurningRootOnAsksFirstAndCancelLeavesItOff() {
        let app = launch("home-root")
        let toggle = rootSwitch(app)
        XCTAssertTrue(toggle.waitForExistence(timeout: 20))
        flip(toggle)
        XCTAssertTrue(app.alerts["Turn on root access?"].waitForExistence(timeout: 10), "no warning")
        XCTAssertTrue(app.alerts.staticTexts.containing(NSPredicate(format: "label CONTAINS 'The official Magisk app is installed at the next start'")).firstMatch.exists)
        app.alerts.buttons["Cancel"].tap()
        XCTAssertTrue(app.alerts.firstMatch.waitForNonExistence(timeout: 5))
        XCTAssertEqual(toggle.value as? String, "0", "Cancel turned root on")
    }

    @MainActor
    func testTurnOnTurnsRootOnAndTheSwitchTurnsItOffAtOnce() {
        let app = launch("home-root")
        let toggle = rootSwitch(app)
        XCTAssertTrue(toggle.waitForExistence(timeout: 20))
        flip(toggle)
        XCTAssertTrue(app.alerts["Turn on root access?"].waitForExistence(timeout: 10))
        app.alerts.buttons["Turn On"].firstMatch.tap()
        XCTAssertTrue(app.alerts.firstMatch.waitForNonExistence(timeout: 5))
        XCTAssertEqual(toggle.value as? String, "1", "Turn On left root off")
        // Off applies at once, with nothing asked.
        flip(toggle)
        XCTAssertFalse(app.alerts.firstMatch.waitForExistence(timeout: 2), "turning root off asked something")
        XCTAssertEqual(toggle.value as? String, "0")
    }

    @MainActor
    func testARootStateOpensWithTheSwitchOn() {
        let app = launch("home-root-on")
        XCTAssertEqual(rootSwitch(app).waitForExistence(timeout: 20) ? rootSwitch(app).value as? String : nil, "1")
    }

    @MainActor
    func testTheWarningIsTheOneThePreviewAsksFor() {
        let app = launch("home-root-warning")
        XCTAssertTrue(app.alerts["Turn on root access?"].waitForExistence(timeout: 20))
        XCTAssertTrue(app.alerts.buttons["Turn On"].exists && app.alerts.buttons["Cancel"].exists)
    }

    @MainActor
    func testAPausedRootSaysSoUnderTheSwitchWhichStaysOn() {
        let app = launch("home-root-paused")
        let toggle = rootSwitch(app)
        XCTAssertTrue(toggle.waitForExistence(timeout: 20))
        XCTAssertEqual(toggle.value as? String, "1")
        XCTAssertTrue(app.staticTexts["Paused after a failed start"].exists)
    }

    @MainActor
    func testTheBootScreenOfAStartThatInstallsTheAppHasAFifthStep() {
        let app = launch("boot-root")
        let step = app.staticTexts["Magisk app"].firstMatch
        XCTAssertTrue(step.waitForExistence(timeout: 20), "no Magisk app step")
        XCTAssertTrue(app.staticTexts["Installs the official Magisk app"].exists)
        // The step comes after Startup, the last of the four.
        let startup = app.staticTexts["Startup"].firstMatch
        XCTAssertTrue(startup.exists)
        XCTAssertLessThan(startup.frame.minY, step.frame.minY)
    }

    @MainActor
    func testAStartWithoutRootKeepsTheFourSteps() {
        let app = launch("boot")
        XCTAssertTrue(app.staticTexts["Startup"].firstMatch.waitForExistence(timeout: 20))
        XCTAssertFalse(app.staticTexts["Magisk app"].exists, "a start without root has the Magisk app step")
    }

    @MainActor
    func testTheAlertSaysTheMagiskAppCouldNotBeInstalled() {
        let app = launch("home-root-app-alert")
        let alert = app.alerts["Magisk app not installed"]
        XCTAssertTrue(alert.waitForExistence(timeout: 20), "no alert")
        XCTAssertTrue(alert.staticTexts.containing(NSPredicate(format: "label CONTAINS 'tries again at the next start'")).firstMatch.exists)
        alert.buttons["OK"].tap()
        XCTAssertTrue(app.alerts.firstMatch.waitForNonExistence(timeout: 5))
    }
}
