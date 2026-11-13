// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import AndariumCore
import Observation
import SwiftUI
import UIKit

/// Something that went wrong, said in an alert over the sheet.
struct SendFeedbackFailure: Identifiable, Equatable {
    let id = UUID()
    var title: String
    var message: String
}

/// The Send feedback sheet's state and what its controls do. The rules are
/// `SendFeedbackForm`'s; this holds the files and runs the bug report, the
/// screenshot, the picked media and the mail. It stops the virtual machine
/// nowhere, and nothing runs once the sheet is closed.
@Observable
final class SendFeedbackModel {
    @ObservationIgnored let app: AppModel
    @ObservationIgnored let staging: SendFeedbackStaging
    @ObservationIgnored let folder: SendFeedbackFolder

    /// What is written and attached; `state` adds what the app says live.
    var form: SendFeedbackForm
    private(set) var thumbnails: [UUID: UIImage] = [:]
    var failure: SendFeedbackFailure?
    /// Cancel asks before it discards.
    var confirmingDiscard = false
    /// The information block shows in full.
    var showsInformation = false
    /// The card that asks for a bug report was put away.
    var hintDismissed = false
    /// The mail was sent: the sheet shows its confirmation and closes.
    private(set) var sent = false
    private(set) var closed = false
    private var presentingMail = false
    /// The start log was offered once, whether or not it was attached, and
    /// whether or not it was removed since: it is made when the sheet opens
    /// and never again.
    @ObservationIgnored private var startLogOffered = false

    init(app: AppModel, staging: SendFeedbackStaging = SendFeedbackStaging()) {
        self.app = app
        self.staging = staging
        folder = staging.makeFolder()
        form = SendFeedbackForm(mailAvailable: app.feedbackServices.canSendMail(), running: app.virtualMachineRuns)
    }

    deinit {
        folder.removeAll()
    }

    /// The form with what the app says now: whether a virtual machine runs,
    /// whether it can make a bug report (the Create bug report row's own
    /// rule, and no other task under the overlay) and whether the bug report
    /// is being made.
    var state: SendFeedbackForm {
        var state = form
        state.running = app.virtualMachineRuns
        state.starting = app.virtualMachineStarts
        state.bugReportPossible = app.canCreateBugReport && app.busy == nil
        state.makingBugReport = app.feedbackBusy != nil
        return state
    }

    /// The basic information the mail ends with.
    var information: SendFeedbackInformation { app.feedbackInformation() }

    /// Mail can come back when the person adds an account in iOS Settings, and
    /// go when the system restricts it: asked again when the app returns to
    /// the foreground.
    func refresh() {
        form.mailAvailable = app.feedbackServices.canSendMail()
    }

    // MARK: Start log

    /// Opened during a start, the sheet attaches the start log by itself, once:
    /// the step, the time on the notice's clock, the kind of start and this
    /// boot's output as the runtime's record quotes it. It is a row like the
    /// others; taking it out is remembered for this sheet, and it is not made
    /// again while the sheet is open, whatever the start does.
    func offerStartLog() {
        guard !startLogOffered else { return }
        startLogOffered = true
        guard !closed, case let .booting(kind) = app.phase, let boot = information.boot else { return }
        let text = SendFeedbackStartLog.text(
            step: boot.step, seconds: boot.seconds, kind: kind, restarts: boot.restarts, output: app.startOutput(),
            containers: app.diagnosticsContainers)
        do {
            let url = try folder.reserve(SendFeedbackStartLog.fileName(at: app.feedbackServices.now()))
            try Data(text.utf8).write(to: url)
            form.attachments.add(SendFeedbackAttachment(kind: .startLog, url: url, bytes: Self.size(of: url)))
        } catch {
            // The log helps; a sheet without it is as usable.
        }
    }

    // MARK: Bug report

    /// Makes the bug report under the app's progress overlay, which the sheet
    /// draws over itself, and attaches the archive.
    func createBugReport() {
        guard state.canMakeBugReport else { return }
        app.createBugReportForFeedback(
            finished: { [weak self] file in
                if let self {
                    adoptBugReport(file)
                } else {
                    try? FileManager.default.removeItem(at: file)
                }
            },
            failed: { [weak self] message in self?.fail(AppModel.bugReportFailure, message) })
    }

    func adoptBugReport(_ file: URL) {
        guard !closed else {
            try? FileManager.default.removeItem(at: file)
            return
        }
        do {
            let staged = try folder.adopt(file, named: file.lastPathComponent)
            if let replaced = form.attachments.add(
                SendFeedbackAttachment(kind: .bugReport, url: staged, bytes: Self.size(of: staged))) {
                folder.remove(replaced.url)
            }
        } catch {
            try? FileManager.default.removeItem(at: file)
            fail(String(localized: "Bug report not attached", table: "Feedback"), error.localizedDescription)
        }
    }

    // MARK: Screenshot

    /// Takes the virtual machine's picture as it is now and attaches it.
    func takeScreenshot() {
        guard state.canTakeScreenshot else { return }
        guard let image = app.feedbackServices.captureScreen(app) else {
            fail(
                String(localized: "Screenshot not taken", table: "Feedback"),
                String(
                    localized: "The virtual machine\u{2019}s display isn\u{2019}t available right now.",
                    table: "Feedback"))
            return
        }
        form.takingScreenshot = true
        let folder = folder
        Task { [weak self] in
            let box = ImageBox(image: image)
            let written = await Task.detached { try Self.writePNG(box.image, into: folder) }.result
            let thumbnail = await image.byPreparingThumbnail(
                ofSize: Self.thumbnailSize(for: image.size, longSide: CGFloat(SendFeedbackMedia.thumbnailPixels)))
            guard let self else { return }
            form.takingScreenshot = false
            switch written {
            case .success(let url):
                guard !closed else {
                    folder.remove(url)
                    return
                }
                let pixels = image.cgImage.map { SendFeedbackPixels(width: $0.width, height: $0.height) }
                let attachment = SendFeedbackAttachment(
                    kind: .screenshot, url: url, bytes: Self.size(of: url), pixels: pixels)
                form.attachments.add(attachment)
                thumbnails[attachment.id] = thumbnail
            case .failure(let error):
                fail(String(localized: "Screenshot not taken", table: "Feedback"), error.localizedDescription)
            }
        }
    }

    private nonisolated struct ImageBox: @unchecked Sendable {
        let image: UIImage
    }

    private nonisolated static func writePNG(_ image: UIImage, into folder: SendFeedbackFolder) throws -> URL {
        guard let data = image.pngData() else { throw CocoaError(.fileWriteUnknown) }
        let url = try folder.reserve("screenshot.png")
        try data.write(to: url)
        return url
    }

    private static func thumbnailSize(for size: CGSize, longSide: CGFloat) -> CGSize {
        let scale = longSide / max(size.width, size.height, 1)
        return CGSize(width: max(1, (size.width * scale).rounded()), height: max(1, (size.height * scale).rounded()))
    }

    // MARK: Photos and videos

    /// How one picked item becomes a file of ours: the picker hands over an
    /// item that loads slowly (a video comes down from iCloud) and may not
    /// load at all.
    typealias MediaLoader = @MainActor () async -> SendFeedbackPickedFile?

    /// Takes in what the picker returned: each item loads, then moves into the
    /// sheet's folder and becomes a row with its picture, size and, for a
    /// video, its length. The importing state lasts from before the first
    /// load to after the last, so Send waits for the files it will carry; an
    /// item that does not load counts as one that was not added.
    func importMedia(loading loaders: [MediaLoader]) async {
        guard !loaders.isEmpty else { return }
        form.importingMedia = true
        var failed = 0
        for load in loaders {
            if let file = await load() {
                if await take(file) == false { failed += 1 }
            } else {
                failed += 1
            }
        }
        form.importingMedia = false
        guard failed > 0, !closed else { return }
        let device = app.host.name
        if failed == 1 {
            fail(
                String(localized: "One item wasn\u{2019}t added", table: "Feedback"),
                String(
                    localized: "It couldn\u{2019}t be copied. Check the free space on this \(device), then try again.",
                    table: "Feedback"))
        } else {
            fail(
                String(localized: "\(failed) items weren\u{2019}t added", table: "Feedback"),
                String(
                    localized: "They couldn\u{2019}t be copied. Check the free space on this \(device), then try again.",
                    table: "Feedback"))
        }
    }

    /// Takes in files that are already loaded.
    func importMedia(_ files: [SendFeedbackPickedFile]) async {
        await importMedia(loading: files.map { file in { file } })
    }

    /// Moves one loaded file into the sheet's folder and makes its row; false
    /// when it could not be copied. A sheet that closed meanwhile takes
    /// nothing, and the file goes.
    private func take(_ file: SendFeedbackPickedFile) async -> Bool {
        let source = file.url
        // The picker's copy sits in a folder of its own.
        defer {
            let parent = source.deletingLastPathComponent()
            if parent.deletingLastPathComponent().standardizedFileURL == staging.incoming.standardizedFileURL {
                try? FileManager.default.removeItem(at: parent)
            }
        }
        guard !closed else { return true }
        do {
            let kind = file.isVideo ? "video" : "photo"
            let ext = source.pathExtension.isEmpty ? (file.isVideo ? "mov" : "jpg") : source.pathExtension
            let staged = try folder.adopt(source, named: "\(kind).\(ext)")
            let inspection = await SendFeedbackMedia.inspect(staged, isVideo: file.isVideo)
            guard !closed else {
                folder.remove(staged)
                return true
            }
            let attachment = SendFeedbackAttachment(
                kind: file.isVideo ? .video : .photo, url: staged, bytes: Self.size(of: staged),
                pixels: inspection.pixels, durationSeconds: inspection.durationSeconds)
            form.attachments.add(attachment)
            if let data = inspection.thumbnail { thumbnails[attachment.id] = UIImage(data: data) }
            return true
        } catch {
            return false
        }
    }

    // MARK: Attachments

    func remove(_ attachment: SendFeedbackAttachment) {
        guard let removed = form.attachments.remove(attachment.id) else { return }
        folder.remove(removed.url)
        thumbnails[removed.id] = nil
    }

    private static func size(of url: URL) -> UInt64 {
        UInt64((try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
    }

    // MARK: Mail

    /// Hands the subject, the body and the files to the system's composer,
    /// where the person reads and may change the mail; nothing is sent from
    /// here.
    func send() {
        guard state.canSend, !presentingMail, let address = app.feedbackAddress else { return }
        // A file the system took away since is not offered.
        let present = form.attachments.items.filter { FileManager.default.fileExists(atPath: $0.url.path) }
        let request = SendFeedbackMailRequest(
            address: address, subject: SendFeedbackMail.subject(problem: form.problem),
            body: SendFeedbackMail.body(details: form.details, information: information),
            attachments: present)
        presentingMail = true
        let shown = app.feedbackServices.presentMail(request) { [weak self] result in
            self?.mailFinished(result)
        }
        if !shown {
            presentingMail = false
            fail(
                String(localized: "Mail not opened", table: "Feedback"),
                String(localized: "Andarium couldn\u{2019}t open the mail composer.", table: "Feedback"))
        }
    }

    /// The person left the composer. Sent ends the draft; saved and cancelled
    /// leave it as it was, for another try; a failure is the system's own words.
    func mailFinished(_ result: SendFeedbackMailResult) {
        presentingMail = false
        switch result {
        case .sent:
            sent = true
            form.problem = ""
            form.details = ""
            cleanUp()
        case .saved, .cancelled:
            break
        case .failed(let message):
            fail(String(localized: "Feedback not sent", table: "Feedback"), message)
        }
    }

    // MARK: Closing

    /// The sheet is closed or the mail is sent: nothing stays behind. A
    /// parent torn down under the sheet (the low-space pause, the end of a
    /// run) closes it too, so the sheet's claim on the overlay of a task under
    /// way goes with it: the screens draw that overlay, and its Cancel, again.
    func close() {
        guard !closed else { return }
        closed = true
        app.busyOverSheet = false
        app.feedbackSheetOpen = false
        app.feedbackFromBoot = false
        cleanUp()
    }

    private func cleanUp() {
        folder.removeAll()
        try? FileManager.default.removeItem(at: staging.incoming)
        thumbnails = [:]
        _ = form.attachments.removeAll()
    }

    private func fail(_ title: String, _ message: String) {
        failure = SendFeedbackFailure(title: title, message: message)
    }
}

#endif
