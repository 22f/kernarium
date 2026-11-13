// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import AndariumCore
import SwiftUI
import UIKit

/// The Simulator preview's Send feedback screens (`-screen feedback…`), played
/// on the real sheet and model: only the system's side is replaced. There is
/// no mail composer, no photo library and no guest display, so the preview
/// answers for them (`SendFeedbackServices`) and fills a draft with files it
/// draws itself. The receiver is the example address. Nothing here exists in a
/// build made without the preview's `FEEDBACK_ADDRESS`.
enum FeedbackPreview {
    /// The screens that open the sheet, over Settings of a running session
    /// (`feedback`…) or over Home, where nothing runs (`feedback-home`…).
    static func opensSheet(_ screen: String) -> Bool { screen.hasPrefix("feedback") }

    static func overHome(_ screen: String) -> Bool { screen.hasPrefix("feedback-home") }

    /// The shots of the sheet the slow-start notice opens, over the boot screen.
    static func overBoot(_ screen: String) -> Bool { screen.hasPrefix("feedback-boot") }

    /// Readies the model for `screen`: its phase, the sheet that stays beneath,
    /// and the services the sheet asks.
    static func configure(screen: String, model: AppModel) {
        guard opensSheet(screen) || screen == "settings-feedback"
            || ["home", "home-menu", "welcome", "welcome-menu", "info"].contains(screen) else { return }
        var services = SendFeedbackServices()
        services.address = { "feedback@example.com" }
        services.canSendMail = { screen != "feedback-nomail" }
        services.presentMail = { _, _ in true }
        services.captureScreen = { _ in guestPicture() }
        services.runningSeconds = { _ in 2520 }
        services.previewOpensAtLaunch = opensSheet(screen) && !overBoot(screen)
        services.previewOpensOverBoot = overBoot(screen)
        services.previewPrepare = { feedback in await prepare(screen: screen, feedback: feedback) }
        services.previewKeepsConfirmation = screen == "feedback-sent"
        model.feedbackServices = services
        guard opensSheet(screen) else { return }
        if overHome(screen) {
            model.phase = .home
        } else if overBoot(screen) {
            // The slow start's own screen stays beneath; AppEntry set it.
        } else {
            model.phase = .running(.normal)
            model.sessionPreset = SessionPreset(settings: true, settingsLarge: true)
        }
    }

    // MARK: Drafts

    @MainActor
    private static func prepare(screen: String, feedback: SendFeedbackModel) async {
        let words = Words()
        switch screen {
        case "feedback", "feedback-home", "feedback-nomail":
            break
        case "feedback-filled", "feedback-details", "feedback-discard",
             "feedback-boot-filled", "feedback-boot-details", "feedback-boot-finished":
            feedback.form.problem = words.problem
            feedback.form.details = words.details
            await attachAll(feedback, report: 8.6, photo: 2.4)
            if screen == "feedback-details" || screen == "feedback-boot-details" { feedback.showsInformation = true }
            if screen == "feedback-boot-finished" {
                // The start finishes while the person writes: the sheet and the
                // draft stay up over the session that follows.
                try? await Task.sleep(for: .milliseconds(900))
                feedback.app.finishBoot(.normal)
            }
            if screen == "feedback-discard" {
                try? await Task.sleep(for: .milliseconds(500))
                feedback.confirmingDiscard = true
            }
        case "feedback-large":
            feedback.form.problem = words.problem
            feedback.form.details = words.details
            await attachAll(feedback, report: 8.6, photo: 21.4)
        case "feedback-generating":
            feedback.form.problem = words.problem
            feedback.form.details = words.details
            await attachAll(feedback, report: nil, photo: nil)
            // The real report lasts a few seconds: hold its overlay part way.
            let app = feedback.app
            app.busyOverSheet = true
            // The words are the app's own: the overlay of a real report.
            var overlay = AppModel.bugReportState
            overlay.progress = 0.6
            overlay.cancel = BusyCancel {}
            app.busy = overlay
        case "feedback-sent":
            feedback.form.problem = words.problem
            feedback.form.details = words.details
            await attachAll(feedback, report: 8.6, photo: 2.4)
            feedback.mailFinished(.sent)
        case "feedback-home-filled":
            feedback.form.problem = words.suggestion
            feedback.form.details = words.suggestionDetails
            await attachAll(feedback, report: nil, screenshot: false, photo: 2.4)
        default:
            break
        }
    }

    @MainActor
    private static func attachAll(
        _ feedback: SendFeedbackModel, report: Double?, screenshot: Bool = true, photo: Double?
    ) async {
        if let report, let file = try? makeFile(named: "bugreport.zip", head: emptyZip, megabytes: report) {
            feedback.adoptBugReport(file)
        }
        if screenshot, feedback.state.running {
            feedback.takeScreenshot()
            while feedback.form.takingScreenshot { try? await Task.sleep(for: .milliseconds(50)) }
        }
        var picked: [SendFeedbackPickedFile] = []
        if let photo, let head = photoData(),
           let file = try? makeFile(named: "photo.jpg", head: head, megabytes: photo) {
            picked.append(SendFeedbackPickedFile(url: file, isVideo: false))
        }
        await feedback.importMedia(picked)
    }

    private struct Words {
        let problem = "Game freezes after the intro video"
        let details = "The game plays its intro, then the picture stops while the sound goes on. It happens every time since this morning\u{2019}s update."
        let suggestion = "Let me keep the keyboard on screen"
        let suggestionDetails = "In games I type a lot, and the keyboard closes after every message. The picture shows where."
    }

    // MARK: Files

    private static let emptyZip = Data([0x50, 0x4B, 0x05, 0x06] + [UInt8](repeating: 0, count: 18))

    /// A file in the picker's folder with `head` at its start, and zeros after it
    /// up to `megabytes` MiB (a sparse file; decoders stop at the picture's end).
    private static func makeFile(named name: String, head: Data, megabytes: Double) throws -> URL {
        let folder = SendFeedbackStaging().incoming.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let url = folder.appendingPathComponent(name)
        try head.write(to: url)
        let handle = try FileHandle(forWritingTo: url)
        defer { try? handle.close() }
        try handle.truncate(atOffset: max(UInt64(megabytes * Double(Limits.bytesPerMiB)), UInt64(head.count)))
        return url
    }

    @MainActor
    private static func photoData() -> Data? {
        let renderer = ImageRenderer(content: PhotoScene().frame(width: 800, height: 600))
        renderer.scale = 1
        return renderer.uiImage?.jpegData(compressionQuality: 0.85)
    }

    /// The mock Android home screen at the display's own pixels.
    @MainActor
    private static func guestPicture() -> UIImage? {
        guard let screen = (UIApplication.shared.connectedScenes.first as? UIWindowScene)?.screen else { return nil }
        let size = screen.bounds.size
        let renderer = ImageRenderer(content: GuestHomeScreen().frame(width: size.width, height: size.height))
        renderer.scale = screen.nativeScale
        return renderer.uiImage
    }
}

/// A made-up photo: hills under an evening sky.
private struct PhotoScene: View {
    var body: some View {
        Canvas { context, size in
            let sky = Gradient(colors: [Color(hex: 0x3B4C8C), Color(hex: 0xE58B6B), Color(hex: 0xF6C77E)])
            context.fill(Path(CGRect(origin: .zero, size: size)), with: .linearGradient(
                sky, startPoint: .zero, endPoint: CGPoint(x: 0, y: size.height * 0.75)))
            let sun = CGRect(x: size.width * 0.58, y: size.height * 0.38, width: size.width * 0.2, height: size.width * 0.2)
            context.fill(Path(ellipseIn: sun), with: .color(Color(hex: 0xFFE3A3)))
            func hill(_ top: CGFloat, _ color: Color, _ phase: CGFloat) {
                var path = Path()
                path.move(to: CGPoint(x: 0, y: size.height))
                for step in 0...24 {
                    let x = size.width * CGFloat(step) / 24
                    let y = size.height * (top + 0.06 * sin(Double(step) / 3.6 + phase))
                    path.addLine(to: CGPoint(x: x, y: y))
                }
                path.addLine(to: CGPoint(x: size.width, y: size.height))
                path.closeSubpath()
                context.fill(path, with: .color(color))
            }
            hill(0.62, Color(hex: 0x6A4A6B), 0.4)
            hill(0.72, Color(hex: 0x3F3150), 2.1)
            hill(0.84, Color(hex: 0x231C33), 4.0)
        }
    }
}

#endif
