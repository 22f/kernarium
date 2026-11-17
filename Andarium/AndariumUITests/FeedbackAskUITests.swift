// SPDX-License-Identifier: GPL-2.0-or-later

import XCTest

#if FEEDBACK_MAIL

/// Send feedback asks about a bug report, and shows what is in one, in the real application.
///
/// The launch asks for the input probe, so a session opens with no guest behind it, and for the feedback
/// probe on top of it (`ANDARIUM_FEEDBACK_PROBE`): the Simulator has no mail account and no guest to make a
/// bug report, so the probe session gives the sheet mail that can be sent, an alert that stands in for the
/// composer (it carries the identifier `feedback-probe-composer` and names the mail) and the preview's
/// report in place of the guest's. Both the report and the question are the real ones. It needs a build with
/// Send feedback, as the build the command in docs/testing.md makes (`FEEDBACK_ADDRESS=feedback@example.com`).
///
/// One path, through every step: Send with a problem and details written and no bug report asks, with two
/// buttons, and no mail opens; Create bug report leaves the person in the sheet with the report attached and
/// no composer; the report's row opens a viewer with its entries and the text of one; the report is removed
/// and Send asks again, and Send without bug report goes on to the mail.
@MainActor
final class FeedbackAskUITests: XCTestCase {
    private func any(_ identifier: String, in app: XCUIApplication) -> XCUIElement {
        app.descendants(matching: .any).matching(identifier: identifier).firstMatch
    }

    /// The elements whose label begins with `text`, of any kind.
    private func labelled(_ text: String, in app: XCUIApplication) -> XCUIElement {
        app.descendants(matching: .any).matching(NSPredicate(format: "label BEGINSWITH %@", text)).firstMatch
    }

    /// The report's row, the button that opens it: the row's buttons share the identifier `feedback-attachment`, and
    /// the one that opens it is named for the report and its size.
    private func reportRow(in app: XCUIApplication) -> XCUIElement {
        app.buttons.matching(NSPredicate(format: "identifier == 'feedback-attachment' AND label BEGINSWITH 'Bug report'"))
            .firstMatch
    }

    /// Sends the keyboard away without a tap that could land on the sheet: Send is out of its reach while it is up.
    private func dismissKeyboard(in app: XCUIApplication) {
        guard app.keyboards.firstMatch.exists else { return }
        for key in ["Return", "return", "Done", "done"] where app.keyboards.buttons[key].exists {
            app.keyboards.buttons[key].tap()
            break
        }
    }

    func testSendFeedbackAsksAboutTheBugReportAndShowsWhatIsInIt() {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchEnvironment["ANDARIUM_INPUT_PROBE"] = "1"
        app.launchEnvironment["ANDARIUM_FEEDBACK_PROBE"] = "1"
        app.launch()

        // The session's Settings, and its Send feedback row.
        let keys = app.buttons["Side keys"]
        XCTAssertTrue(keys.waitForExistence(timeout: 90), "the session did not open")
        keys.tap()
        let settings = app.buttons["Settings"]
        XCTAssertTrue(settings.waitForExistence(timeout: 20), "the side keys did not open")
        settings.tap()
        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 30), "Settings did not open")
        let row = app.buttons["settings-send-feedback"]
        for _ in 0..<5 where !(row.waitForExistence(timeout: 3) && row.isHittable) {
            app.collectionViews.firstMatch.swipeUp()
        }
        XCTAssertTrue(row.isHittable, "Settings has no Send feedback row")
        row.tap()

        // A problem and details, both written.
        let problem = any("feedback-problem", in: app)
        XCTAssertTrue(problem.waitForExistence(timeout: 30), "Send feedback did not open")
        problem.tap()
        problem.typeText("The game closes by itself")
        let details = any("feedback-details", in: app)
        details.tap()
        details.typeText("It closes after the first level.")
        dismissKeyboard(in: app)

        // Send, with no bug report attached: the question, and no mail.
        let send = app.buttons["feedback-send"]
        XCTAssertTrue(send.waitForExistence(timeout: 10), "the sheet has no Send")
        send.tap()
        let alert = app.alerts["Send without a bug report?"]
        XCTAssertTrue(alert.waitForExistence(timeout: 10), "Send did not ask about a bug report")
        XCTAssertEqual(alert.buttons.count, 2, "the question has not exactly two buttons")
        XCTAssertTrue(alert.buttons["Create bug report"].exists, "the question has no Create bug report")
        XCTAssertTrue(alert.buttons["Send without bug report"].exists, "the question has no Send without bug report")
        XCTAssertTrue(
            labelled("Without a bug report, a problem will most likely not get fixed.", in: app).exists,
            "the question does not say that a problem will most likely not get fixed")
        XCTAssertFalse(any("feedback-probe-composer", in: app).exists, "the mail opened before the question was answered")

        // Create bug report: the sheet stays, the report is attached, and no mail opens.
        alert.buttons["Create bug report"].tap()
        let report = reportRow(in: app)
        XCTAssertTrue(report.waitForExistence(timeout: 90), "the bug report was not attached")
        XCTAssertFalse(alert.exists, "the question stayed")
        XCTAssertFalse(any("feedback-probe-composer", in: app).exists, "Create bug report opened the mail")
        XCTAssertEqual(app.textFields["feedback-problem"].value as? String, "The game closes by itself", "the draft changed")

        // The report's row opens the viewer: its entries, and the text of one.
        report.tap()
        let viewer = any("feedback-viewer", in: app)
        XCTAssertTrue(viewer.waitForExistence(timeout: 30), "the bug report did not open")
        let version = labelled("version.txt", in: app)
        XCTAssertTrue(version.waitForExistence(timeout: 30), "the viewer lists no entry of the report")
        XCTAssertTrue(labelled("screenshot-2026-11-17.png", in: app).exists, "the viewer lists no picture")
        version.tap()
        let text = any("feedback-viewer-text", in: app)
        XCTAssertTrue(text.waitForExistence(timeout: 30), "the entry showed no text")
        XCTAssertTrue(text.label.contains("3.0"), "the entry's text is \(text.label)")
        let close = app.buttons["feedback-viewer-close"]
        XCTAssertTrue(close.exists, "the viewer has no Close")
        close.tap()
        XCTAssertTrue(viewer.waitForNonExistence(timeout: 10), "the viewer stayed")
        XCTAssertTrue(reportRow(in: app).waitForExistence(timeout: 10), "the sheet lost the report")

        // With the report attached, Send opens the mail at once.
        let sendAgain = app.buttons["feedback-send"]
        sendAgain.tap()
        let composer = any("feedback-probe-composer", in: app)
        XCTAssertTrue(composer.waitForExistence(timeout: 10), "Send with a report attached did not open the mail")
        XCTAssertFalse(app.alerts["Send without a bug report?"].exists, "Send asked with a report attached")
        app.buttons["Cancel mail"].tap()
        XCTAssertTrue(composer.waitForNonExistence(timeout: 10), "the mail stayed")

        // Without the report, Send asks again, and Send without bug report goes on to the mail.
        let remove = app.buttons["Remove bug report"]
        XCTAssertTrue(remove.waitForExistence(timeout: 10), "the report has no Remove")
        remove.tap()
        XCTAssertTrue(reportRow(in: app).waitForNonExistence(timeout: 10), "the report stayed")
        app.buttons["feedback-send"].tap()
        XCTAssertTrue(alert.waitForExistence(timeout: 10), "Send did not ask again")
        alert.buttons["Send without bug report"].tap()
        XCTAssertTrue(
            composer.waitForExistence(timeout: 10), "Send without bug report did not go on to the mail")
        XCTAssertFalse(alert.exists, "the question stayed")
    }
}

#endif
