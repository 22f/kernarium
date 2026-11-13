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
    /// The access held to the files picked from Files, one per attachment.
    @ObservationIgnored let scopes: SendFeedbackScopes
    @ObservationIgnored private var fileCancellation: SendFeedbackCancellation?

    /// What is written and attached; `state` adds what the app says live.
    var form: SendFeedbackForm
    private(set) var thumbnails: [UUID: UIImage] = [:]
    /// The overlay of a file that cannot be used where it is while it is copied;
    /// the sheet draws it over itself. The Simulator preview sets it for a capture.
    var fileBusy: BusyState?
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

    init(app: AppModel, staging: SendFeedbackStaging = SendFeedbackStaging()) {
        self.app = app
        self.staging = staging
        folder = staging.makeFolder()
        scopes = SendFeedbackScopes(system: app.feedbackServices.files)
        form = SendFeedbackForm(mailAvailable: app.feedbackServices.canSendMail(), running: app.virtualMachineRuns)
    }

    deinit {
        folder.removeAll()
        scopes.releaseAll()
    }

    /// The form with what the app says now: whether a virtual machine runs,
    /// whether it can make a bug report (the Create bug report row's own
    /// rule, and no other task under the overlay) and whether the bug report
    /// is being made.
    var state: SendFeedbackForm {
        var state = form
        state.running = app.virtualMachineRuns
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

    // MARK: Files

    /// Opens the system's document picker. What it returns is added where it
    /// is kept (`addFiles`).
    func pickFiles() {
        guard state.canAddFiles else { return }
        _ = app.feedbackServices.pickFiles { [weak self] urls in
            guard let self else { return }
            Task { await self.addFiles(urls) }
        }
    }

    /// Takes in the files the picker opened where they are kept. Each one is
    /// looked at without being read: a folder, a package, an empty file or one
    /// that is not reachable is refused; a file the composer can map from
    /// where it is becomes a row, with the picker's access held until the row
    /// goes; any other (its provider has not downloaded it, its volume refuses
    /// mapping) is copied into the sheet's folder, with the overlay and its
    /// Cancel, and the access let go. Picking a file again attaches it again.
    /// Send waits for all of it.
    func addFiles(_ urls: [URL]) async {
        guard !urls.isEmpty, !closed else { return }
        form.addingFiles = true
        let cancellation = SendFeedbackCancellation()
        fileCancellation = cancellation
        var refusals: [SendFeedbackFileRefusal] = []
        for url in urls {
            if cancellation.isCancelled || closed { break }
            do {
                try await addFile(url, cancellation: cancellation)
            } catch let refusal as SendFeedbackFileRefusal {
                if refusal != .cancelled { refusals.append(refusal) }
            } catch {
                refusals.append(.unavailable)
            }
        }
        fileBusy = nil
        fileCancellation = nil
        form.addingFiles = false
        // A copy that was still under way as the sheet closed may have made its folder again.
        if closed { folder.removeAll() }
        guard !refusals.isEmpty, !closed else { return }
        fail(Self.fileFailureTitle(count: refusals.count), Self.fileFailureMessage(refusals, device: app.host.name))
    }

    private func addFile(_ url: URL, cancellation: SendFeedbackCancellation) async throws {
        let id = UUID()
        scopes.hold(id, url)
        var keepAccess = false
        defer { if !keepAccess { scopes.release(id) } }
        let system = app.feedbackServices.files
        let info = await Task.detached { system.inspect(url) }.value
        guard !closed else { return }
        switch info.kind {
        case .file: break
        case .folder, .other: throw SendFeedbackFileRefusal.notAFile
        case .missing: throw SendFeedbackFileRefusal.unavailable
        }
        guard info.bytes > 0 else { throw SendFeedbackFileRefusal.empty }
        let displayName = SendFeedbackNames.display(url.lastPathComponent)
        if info.usableInPlace {
            guard !closed else { return }
            keepAccess = true
            form.attachments.add(SendFeedbackAttachment(
                id: id, kind: .file, url: url, bytes: info.bytes, displayName: displayName,
                mailName: SendFeedbackFolder.safeName(url.lastPathComponent), inPlace: true))
            return
        }
        let staged = try await copy(url, bytes: info.bytes, cancellation: cancellation)
        guard !closed else {
            folder.remove(staged)
            return
        }
        form.attachments.add(SendFeedbackAttachment(
            kind: .file, url: staged, bytes: Self.size(of: staged), displayName: displayName))
    }

    /// Copies a file that cannot be used where it is into the sheet's folder,
    /// off the main thread, under the overlay with its Cancel.
    private func copy(_ url: URL, bytes: UInt64, cancellation: SendFeedbackCancellation) async throws -> URL {
        fileBusy = Self.copyingState(progress: 0, cancel: BusyCancel { [weak self] in self?.fileCancellation?.cancel() })
        defer { fileBusy = nil }
        let folder = folder
        let name = SendFeedbackFolder.safeName(url.lastPathComponent)
        return try await Task.detached(priority: .userInitiated) {
            try folder.stageCopy(of: url, bytes: bytes, named: name, cancellation: cancellation) { [weak self] copied in
                Task { @MainActor in self?.fileCopied(copied, of: bytes) }
            }
        }.value
    }

    /// The overlay of a copy at `progress` (0 to 1).
    static func copyingState(progress: Double, cancel: BusyCancel) -> BusyState {
        BusyState(
            title: String(localized: "Copying file", table: "Feedback"),
            detail: String(
                localized: "This file can\u{2019}t be used where it is, so a copy is made first. Large files take a while.",
                table: "Feedback"),
            progress: progress, cancel: cancel)
    }

    private func fileCopied(_ copied: UInt64, of total: UInt64) {
        guard fileBusy != nil, total > 0 else { return }
        fileBusy?.progress = min(1, Double(copied) / Double(total))
    }

    private static func fileFailureTitle(count: Int) -> String {
        count == 1
            ? String(localized: "One item wasn\u{2019}t added", table: "Feedback")
            : String(localized: "\(count) items weren\u{2019}t added", table: "Feedback")
    }

    /// One line for each reason a file was refused, in a fixed order.
    private static func fileFailureMessage(_ refusals: [SendFeedbackFileRefusal], device: String) -> String {
        var lines: [String] = []
        if refusals.contains(.notEnoughSpace) {
            lines.append(String(
                localized: "There isn\u{2019}t enough free space on this \(device). Free up space, then try again.",
                table: "Feedback"))
        }
        if refusals.contains(.notAFile) {
            lines.append(String(
                localized: "A folder or a package can\u{2019}t be attached. Compress it in Files first.",
                table: "Feedback"))
        }
        if refusals.contains(.empty) {
            lines.append(String(localized: "An empty file can\u{2019}t be attached.", table: "Feedback"))
        }
        if refusals.contains(.unavailable) {
            lines.append(String(
                localized: "A file couldn\u{2019}t be read. Check that it is still there, then try again.",
                table: "Feedback"))
        }
        return lines.joined(separator: "\n")
    }

    // MARK: Attachments

    /// Takes a file out of the mail. A staged file is deleted; a file kept
    /// where the person has it is only let go.
    func remove(_ attachment: SendFeedbackAttachment) {
        guard let removed = form.attachments.remove(attachment.id) else { return }
        if removed.inPlace {
            scopes.release(removed.id)
        } else {
            folder.remove(removed.url)
        }
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
        guard state.canSend, !presentingMail, let address = app.feedbackAddress, attachmentsAreThere() else { return }
        let request = SendFeedbackMailRequest(
            address: address, subject: SendFeedbackMail.subject(problem: form.problem),
            body: SendFeedbackMail.body(details: form.details, information: information),
            attachments: form.attachments.items)
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

    /// Looks at every file again as Send is tapped. A file that is gone, or
    /// that the composer can no longer map from where it is (the person moved
    /// or deleted it, its provider took it back, access was lost), is taken
    /// out of the list and said so, and nothing is sent: a mail never goes
    /// without a file the person added, unseen. A file that changed has the
    /// size it has now, and what it holds now is what goes.
    private func attachmentsAreThere() -> Bool {
        var gone: [SendFeedbackAttachment] = []
        for item in form.attachments.items {
            if item.inPlace {
                let now = app.feedbackServices.files.inspect(item.url)
                if now.usableInPlace {
                    if now.bytes != item.bytes { form.attachments.update(item.id, bytes: now.bytes) }
                } else {
                    gone.append(item)
                }
            } else if !FileManager.default.fileExists(atPath: item.url.path) {
                gone.append(item)
            }
        }
        guard !gone.isEmpty else { return true }
        gone.forEach(remove)
        fail(
            String(localized: "Attachment not available", table: "Feedback"),
            String(
                localized: "A file you added is no longer available, so it was taken out of the list. Check the list, then send again.",
                table: "Feedback"))
        return false
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
        fileCancellation?.cancel()
        app.busyOverSheet = false
        app.feedbackSheetOpen = false
        cleanUp()
    }

    private func cleanUp() {
        scopes.releaseAll()
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
