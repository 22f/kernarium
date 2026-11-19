// SPDX-License-Identifier: GPL-2.0-or-later

import XCTest

/// Add to Home Screen in the real application: Settings > Apps > the row, the
/// chooser and the guide, and Apple's own Shortcuts button, which opens
/// Apple's Shortcuts app on Andarium's page. The launch asks for the input
/// probe with the played list of made-up Android apps in place of the image's
/// tool (`ANDARIUM_ANDROID_APPS_PROBE`, see the Andarium README), which also
/// puts the apps in the App Shortcuts, so Shortcuts shows their tiles. The
/// second test needs the Shortcuts app, which a Simulator has.
@MainActor
final class AddToHomeUITests: XCTestCase {
    private let comet = "Add \u{201C}Comet Hop\u{201D} to Home Screen"

    private func launch() -> XCUIApplication {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchEnvironment["ANDARIUM_INPUT_PROBE"] = "1"
        app.launchEnvironment["ANDARIUM_ANDROID_APPS_PROBE"] = "1"
        app.launch()
        let keys = app.buttons["Side keys"]
        XCTAssertTrue(keys.waitForExistence(timeout: 90), "the session did not open")
        keys.tap()
        let settings = app.buttons["Settings"]
        XCTAssertTrue(settings.waitForExistence(timeout: 20), "the side keys did not open")
        settings.tap()
        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 30), "Settings did not open")
        return app
    }

    private func attach(_ name: String, of element: XCUIElement) {
        let attachment = XCTAttachment(screenshot: element.screenshot())
        attachment.name = name
        attachment.lifetime = .keepAlways
        add(attachment)
    }

    private func openTheGuide(_ app: XCUIApplication) {
        let row = app.buttons["settings-add-to-home-screen"]
        XCTAssertTrue(row.waitForExistence(timeout: 30), "Settings has no Add to Home Screen row")
        attach("settings-row", of: app)
        row.tap()
        XCTAssertTrue(app.navigationBars["Add to Home Screen"].waitForExistence(timeout: 20), "the chooser did not open")
        let app1 = app.buttons[comet]
        XCTAssertTrue(app1.waitForExistence(timeout: 30), "the list the tool gave is not on the page")
        attach("chooser", of: app)
        app1.tap()
        XCTAssertTrue(app.descendants(matching: .any)["add-to-home-screen-guide"].waitForExistence(timeout: 20), "the guide did not open")
    }

    func testTheRowTheChooserAndTheGuideOfTheBuiltApplication() {
        let app = launch()
        openTheGuide(app)
        XCTAssertTrue(app.staticTexts["Touch and hold \u{201C}Comet Hop\u{201D}"].exists)
        attach("guide", of: app)
        // Apple's button, drawn by the system.
        let link = app.descendants(matching: .any).matching(NSPredicate(
            format: "identifier == 'shortcuts-link' OR (label CONTAINS[c] 'shortcuts' AND (elementType == %d OR elementType == %d))",
            XCUIElement.ElementType.button.rawValue, XCUIElement.ElementType.link.rawValue)).firstMatch
        XCTAssertTrue(link.waitForExistence(timeout: 10), "Apple's Shortcuts button is missing")
    }

    func testApplesShortcutsButtonOpensShortcutsOnAndariumsPage() {
        let app = launch()
        openTheGuide(app)
        let link = app.descendants(matching: .any).matching(NSPredicate(
            format: "identifier == 'shortcuts-link' OR (label CONTAINS[c] 'shortcuts' AND (elementType == %d OR elementType == %d))",
            XCUIElement.ElementType.button.rawValue, XCUIElement.ElementType.link.rawValue)).firstMatch
        XCTAssertTrue(link.waitForExistence(timeout: 10), "Apple's Shortcuts button is missing")
        link.tap()
        let shortcuts = XCUIApplication(bundleIdentifier: "com.apple.shortcuts")
        XCTAssertTrue(shortcuts.wait(for: .runningForeground, timeout: 60), "Shortcuts did not open")
        // The first launch may show what is new.
        sleep(4)
        attach("shortcuts-1", of: shortcuts)
        let next = shortcuts.buttons["Continue"]
        if next.waitForExistence(timeout: 3) {
            next.tap()
            sleep(2)
        }
        attach("shortcuts-2", of: shortcuts)
        // Andarium's page: its name, then a tile for each of its apps. The
        // tiles are asked of the application and can come a moment later.
        XCTAssertTrue(
            shortcuts.staticTexts["Andarium"].firstMatch.waitForExistence(timeout: 30),
            "Shortcuts does not show Andarium's page: \(shortcuts.debugDescription)")
        attach("shortcuts-page", of: shortcuts)
        let tile = shortcuts.descendants(matching: .any).matching(NSPredicate(format: "label CONTAINS[c] 'Comet Hop'")).firstMatch
        let found = tile.waitForExistence(timeout: 60)
        attach("shortcuts-3", of: shortcuts)
        XCTAssertTrue(found, "Andarium's page has no tile for Comet Hop: \(shortcuts.debugDescription)")
    }
}
