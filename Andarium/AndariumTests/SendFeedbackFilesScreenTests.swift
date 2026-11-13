// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import AndariumCore
import AndariumCoreTestSupport
import Darwin
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// Files picked from Files in the Send feedback sheet, hosted in the
/// application: a file stays where it is under an access the sheet holds, a
/// file that cannot be used there is copied, and everything is let go of when
/// the attachment goes or the sheet closes. The system's picker is replaced by
/// an injected source of URLs, the access by counters, and the look at a file
/// by the real one or, where a provider's state is needed, a stand-in. They
/// run where Send feedback is compiled in (`FEEDBACK_ADDRESS` on the xcodebuild
/// command line).
@Suite(.serialized)
@MainActor
struct SendFeedbackFilesScreenTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 41_000_000_000
    )
    private let mebibyte: UInt64 = 1 << 20

    private nonisolated final class Gate: @unchecked Sendable {
        private let lock = NSLock()
        private var open = false
        func release() { lock.withLock { open = true } }
        var isOpen: Bool { lock.withLock { open } }
        func wait() { while !isOpen { usleep(2000) } }
    }

    /// What the stand-in for the system's access and look at a file saw.
    private nonisolated final class Access: @unchecked Sendable {
        private let lock = NSLock()
        private var started: [String] = []
        private var stopped: [String] = []
        private var away: Set<String> = []
        func start(_ url: URL) { lock.withLock { started.append(url.lastPathComponent) } }
        func stop(_ url: URL) { lock.withLock { stopped.append(url.lastPathComponent) } }
        var starts: [String] { lock.withLock { started } }
        var stops: [String] { lock.withLock { stopped } }
        /// Names the provider has not downloaded.
        func notDownloaded(_ name: String, _ gone: Bool = true) {
            lock.withLock {
                if gone { away.insert(name) } else { away.remove(name) }
            }
        }
        func isAway(_ name: String) -> Bool { lock.withLock { away.contains(name) } }
    }

    private struct Fixture {
        let app: AppModel
        let composer: Composer
        let access: Access
        /// Where the staging folder is, and where the person's own files are.
        let root: URL
        let library: URL
        let staging: SendFeedbackStaging

        func sheet() -> SendFeedbackModel { SendFeedbackModel(app: app, staging: staging) }

        /// A file of the person's, in a folder of its own.
        func original(_ name: String, bytes: Int = 4096, fill: UInt8 = 7) throws -> URL {
            let url = library.appendingPathComponent(name)
            try Data(repeating: fill, count: bytes).write(to: url)
            return url
        }

        func tearDown() {
            try? FileManager.default.removeItem(at: root)
        }
    }

    @MainActor private final class Composer {
        var request: SendFeedbackMailRequest?
        private var finish: (@MainActor (SendFeedbackMailResult) -> Void)?
        func open(_ request: SendFeedbackMailRequest, _ finish: @escaping @MainActor (SendFeedbackMailResult) -> Void) {
            self.request = request
            self.finish = finish
        }
        func leave(_ result: SendFeedbackMailResult) {
            finish?(result)
            finish = nil
        }
    }

    private func fixture(free: UInt64? = nil, hold: Gate? = nil) throws -> Fixture {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KB pages", appVersion: "1.0 (9)")
        let app = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        app.driver = driver
        app.phase = .running(.normal)
        let composer = Composer()
        let access = Access()
        var services = SendFeedbackServices()
        services.address = { "feedback@example.com" }
        services.canSendMail = { true }
        services.presentMail = { request, finish in
            composer.open(request, finish)
            return true
        }
        services.captureScreen = { _ in Self.picture() }
        services.runningSeconds = { _ in 125 }
        services.files = SendFeedbackFileSystem(
            start: { url in
                access.start(url)
                return true
            },
            stop: { access.stop($0) },
            inspect: { url in
                var found = SendFeedbackFileInspection.read(url)
                if access.isAway(url.lastPathComponent) { found.isLocal = false }
                return found
            })
        app.feedbackServices = services
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-feedback-files-\(UUID().uuidString)", isDirectory: true)
        let library = root.appendingPathComponent("library", isDirectory: true)
        try FileManager.default.createDirectory(at: library, withIntermediateDirectories: true)
        let staging = SendFeedbackStaging(
            root: root.appendingPathComponent("staging", isDirectory: true),
            freeBytes: { _ in
                hold?.wait()
                return free
            })
        return Fixture(app: app, composer: composer, access: access, root: root, library: library, staging: staging)
    }

    private nonisolated static func picture() -> UIImage {
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        return UIGraphicsImageRenderer(size: CGSize(width: 90, height: 160), format: format).image { context in
            UIColor.systemTeal.setFill()
            context.fill(CGRect(x: 0, y: 0, width: 90, height: 160))
        }
    }

    /// A poll that returns as soon as the condition holds; the 10 s is only its deadline.
    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while ContinuousClock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(10))
        }
        return condition()
    }

    private func footprint() -> UInt64 {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size)
        let result = withUnsafeMutablePointer(to: &info) { pointer in
            pointer.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        return result == KERN_SUCCESS ? info.phys_footprint : 0
    }

    private func isEmpty(_ folder: SendFeedbackFolder) -> Bool {
        ((try? FileManager.default.contentsOfDirectory(atPath: folder.url.path)) ?? []).isEmpty
    }

    // MARK: In place

    @Test func aPickedFileStaysWhereItIsIsNotCopiedAndHoldsItsAccess() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let file = try fixture.original("app-release.apk", bytes: 12_345)
        let sheet = fixture.sheet()
        sheet.form.problem = "Crash"
        await sheet.addFiles([file])
        let row = try #require(sheet.form.attachments.items.first)
        #expect(sheet.form.attachments.count == 1)
        #expect(row.kind == .file && row.inPlace && row.url == file && row.bytes == 12_345)
        #expect(row.displayName == "app-release.apk" && row.fileName == "app-release.apk")
        #expect(!sheet.folder.exists, "no copy: nothing was staged")
        #expect(!FileManager.default.fileExists(atPath: fixture.staging.root.path))
        #expect(fixture.access.starts == ["app-release.apk"] && fixture.access.stops.isEmpty && sheet.scopes.count == 1)
        #expect(sheet.thumbnails.isEmpty, "a plain file has no picture")
        #expect(!sheet.state.addingFiles && sheet.state.canSend && sheet.fileBusy == nil && sheet.failure == nil)
        sheet.close()
    }

    @Test func theFilesPickedAreAddedAtTheEndInTheOrderTheyCame() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let sheet = fixture.sheet()
        sheet.takeScreenshot()
        #expect(await eventually { sheet.form.attachments.count == 1 })
        await sheet.addFiles([try fixture.original("b.bin"), try fixture.original("a.bin")])
        #expect(sheet.form.attachments.items.map(\.kind) == [.screenshot, .file, .file])
        #expect(sheet.form.attachments.items.map(\.fileName) == ["screenshot.png", "b.bin", "a.bin"])
        sheet.close()
    }

    @Test func removingAFileLetsGoOfItsAccessAndNeverDeletesTheOriginal() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let file = try fixture.original("keep-me.apk", bytes: 2000, fill: 9)
        let sheet = fixture.sheet()
        await sheet.addFiles([file])
        sheet.remove(try #require(sheet.form.attachments.items.first))
        #expect(sheet.form.attachments.isEmpty && sheet.scopes.count == 0)
        #expect(fixture.access.starts == ["keep-me.apk"] && fixture.access.stops == ["keep-me.apk"])
        #expect(try Data(contentsOf: file) == Data(repeating: 9, count: 2000), "the person's file is still there, whole")
        sheet.close()
        #expect(fixture.access.stops.count == 1, "closed, nothing more to stop")
    }

    @Test func closingStopsEveryAccessOnceAndLeavesTheFilesAlone() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let one = try fixture.original("one.apk"), two = try fixture.original("two.apk")
        let sheet = fixture.sheet()
        await sheet.addFiles([one, two])
        sheet.remove(try #require(sheet.form.attachments.items.first))
        sheet.close()
        sheet.close()
        #expect(fixture.access.starts.sorted() == ["one.apk", "two.apk"])
        #expect(fixture.access.stops.sorted() == ["one.apk", "two.apk"], "each exactly once")
        #expect(FileManager.default.fileExists(atPath: one.path) && FileManager.default.fileExists(atPath: two.path))
    }

    @Test func aSheetLetGoWithoutClosingStillStopsItsAccess() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let file = try fixture.original("gone-with-the-sheet.apk")
        do {
            let sheet = fixture.sheet()
            await sheet.addFiles([file])
            #expect(fixture.access.stops.isEmpty)
        }
        #expect(await eventually { fixture.access.stops == ["gone-with-the-sheet.apk"] })
        #expect(FileManager.default.fileExists(atPath: file.path))
    }

    @Test func aSentMailLetsGoOfTheFiles() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let sheet = fixture.sheet()
        sheet.form.problem = "x"
        await sheet.addFiles([try fixture.original("sent.apk")])
        sheet.send()
        #expect(fixture.access.stops.isEmpty, "the composer maps the file: the access stays until it is over")
        fixture.composer.leave(.sent)
        #expect(fixture.access.stops == ["sent.apk"] && sheet.scopes.count == 0)
        sheet.close()
        #expect(fixture.access.stops.count == 1)
    }

    @Test func aFileThePickerHandedUsTwiceIsAttachedTwiceWithNamesOfItsOwn() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let file = try fixture.original("app.apk")
        let sheet = fixture.sheet()
        await sheet.addFiles([file])
        await sheet.addFiles([file])
        #expect(sheet.form.attachments.items.map(\.fileName) == ["app.apk", "app-2.apk"])
        #expect(sheet.form.attachments.items.map(\.displayName) == ["app.apk", "app.apk"])
        #expect(fixture.access.starts.count == 2 && sheet.scopes.count == 2)
        sheet.remove(sheet.form.attachments.items[0])
        #expect(fixture.access.stops.count == 1 && FileManager.default.fileExists(atPath: file.path))
        sheet.close()
        #expect(fixture.access.stops.count == 2, "one stop for each start")
    }

    // MARK: Names

    @Test func theMailGetsASafeUniqueNameWhileTheRowKeepsTheName() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let sheet = fixture.sheet()
        sheet.form.problem = "x"
        sheet.takeScreenshot()
        #expect(await eventually { sheet.form.attachments.count == 1 })
        let clash = try fixture.original("Screenshot.PNG")
        let odd = try fixture.original("My App \u{5E94}\u{7528} (v2).apk")
        await sheet.addFiles([clash, odd])
        sheet.send()
        let request = try #require(fixture.composer.request)
        #expect(request.attachments.map(\.fileName) == ["screenshot.png", "Screenshot-2.PNG", "My_App_____v2_.apk"])
        #expect(request.attachments[1].url == clash && request.attachments[2].url == odd, "from where they are kept")
        #expect(request.attachments.map(\.displayName) == [nil, "Screenshot.PNG", "My App \u{5E94}\u{7528} (v2).apk"])
        #expect(request.attachments[2].mimeType == "application/vnd.android.package-archive")
        sheet.close()
    }

    // MARK: Refusals

    @Test func aFolderAMissingFileAndAnEmptyFileAreRefusedEachForItsReason() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let folder = fixture.library.appendingPathComponent("Some Folder", isDirectory: true)
        let package = fixture.library.appendingPathComponent("Thing.app", isDirectory: true)
        for directory in [folder, package] {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try Data([1]).write(to: directory.appendingPathComponent("inside.bin"))
        }
        let empty = try fixture.original("empty.txt", bytes: 0)
        let missing = fixture.library.appendingPathComponent("never-was.apk")
        let sheet = fixture.sheet()
        for (url, line) in [
            (folder, "A folder or a package can\u{2019}t be attached. Compress it in Files first."),
            (package, "A folder or a package can\u{2019}t be attached. Compress it in Files first."),
            (empty, "An empty file can\u{2019}t be attached."),
            (missing, "A file couldn\u{2019}t be read. Check that it is still there, then try again."),
        ] {
            sheet.failure = nil
            await sheet.addFiles([url])
            #expect(sheet.form.attachments.isEmpty, "\(url.lastPathComponent)")
            #expect(sheet.failure?.title == "One item wasn\u{2019}t added" && sheet.failure?.message == line, "\(url.lastPathComponent)")
            #expect(!sheet.state.addingFiles && sheet.fileBusy == nil)
        }
        #expect(fixture.access.stops.sorted() == fixture.access.starts.sorted(), "a refused file's access is let go at once")
        #expect(sheet.scopes.count == 0)
        // Nothing of the folders is touched.
        #expect(FileManager.default.fileExists(atPath: folder.appendingPathComponent("inside.bin").path))
        sheet.close()
    }

    @Test func severalRefusalsAreOneAlertWithALineForEachReasonAndTheGoodFileIsKept() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let folder = fixture.library.appendingPathComponent("Folder", isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let sheet = fixture.sheet()
        await sheet.addFiles([
            folder, try fixture.original("empty.txt", bytes: 0), try fixture.original("good.apk"), folder,
        ])
        #expect(sheet.form.attachments.items.map(\.fileName) == ["good.apk"])
        #expect(sheet.failure?.title == "3 items weren\u{2019}t added")
        #expect(sheet.failure?.message == """
            A folder or a package can\u{2019}t be attached. Compress it in Files first.
            An empty file can\u{2019}t be attached.
            """)
        #expect(fixture.access.starts.count == 4 && fixture.access.stops.count == 3 && sheet.scopes.count == 1)
        sheet.close()
        #expect(fixture.access.stops.count == 4)
    }

    // MARK: Large files

    /// An APK can be several gigabytes: added and given to the composer from where it is, with
    /// its size known, and not one byte of it in memory.
    @Test func aFileOfSeveralGiBIsAddedAndHandedOverWithoutBeingReadIntoMemory() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let big = fixture.library.appendingPathComponent("big.apk")
        try Data([0x50]).write(to: big)
        let handle = try FileHandle(forWritingTo: big)
        try handle.truncate(atOffset: 3 << 30)
        try handle.close()
        let sheet = fixture.sheet()
        sheet.form.problem = "Big"
        let before = footprint()
        await sheet.addFiles([big])
        let row = try #require(sheet.form.attachments.items.first)
        #expect(row.bytes == 3 << 30 && sheet.state.attachments.exceedsWarningSize)
        #expect(sheet.state.canSend, "the warning never blocks Send")
        sheet.send()
        let sent = try #require(fixture.composer.request?.attachments.first)
        let mapped = try sent.mappedData()
        #expect(mapped.count == 3 << 30 && mapped[0] == 0x50)
        let grown = footprint() &- before
        #expect(grown < 1536 * mebibyte, "\(grown / mebibyte) MiB grew for a 3 GiB file")
        #expect(!sheet.folder.exists)
        sheet.close()
    }

    // MARK: A file that cannot be used where it is

    @Test func aFileNotDownloadedIsCopiedAndItsAccessLetGoRightAfter() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let file = try fixture.original("cloud.apk", bytes: 70_000, fill: 3)
        fixture.access.notDownloaded("cloud.apk")
        let sheet = fixture.sheet()
        await sheet.addFiles([file])
        let row = try #require(sheet.form.attachments.items.first)
        #expect(row.kind == .file && !row.inPlace && row.bytes == 70_000)
        #expect(row.url.deletingLastPathComponent().resolvingSymlinksInPath() == sheet.folder.url.resolvingSymlinksInPath())
        #expect(try Data(contentsOf: row.url) == Data(repeating: 3, count: 70_000))
        #expect(row.displayName == "cloud.apk" && row.fileName == "cloud.apk")
        #expect(try Data(contentsOf: file) == Data(repeating: 3, count: 70_000), "the original is untouched")
        #expect(fixture.access.starts == ["cloud.apk"] && fixture.access.stops == ["cloud.apk"], "released once the copy was made")
        #expect(sheet.scopes.count == 0 && sheet.fileBusy == nil && !sheet.state.addingFiles)
        // Removing a copy deletes the copy, not the original.
        sheet.remove(row)
        #expect(!FileManager.default.fileExists(atPath: row.url.path) && FileManager.default.fileExists(atPath: file.path))
        #expect(fixture.access.stops.count == 1)
        sheet.close()
    }

    @Test func theCopyShowsTheOverlayWithProgressAndCancelAndSendWaits() async throws {
        let gate = Gate()
        let fixture = try fixture(hold: gate)
        defer { fixture.tearDown() }
        fixture.access.notDownloaded("slow.apk")
        let sheet = fixture.sheet()
        sheet.form.problem = "x"
        let file = try fixture.original("slow.apk")
        let adding = Task { await sheet.addFiles([file]) }
        #expect(await eventually { sheet.fileBusy != nil })
        let busy = try #require(sheet.fileBusy)
        #expect(busy.title == "Copying file" && busy.progress == 0 && busy.cancel != nil && busy.result == nil)
        #expect(sheet.state.addingFiles && !sheet.state.canSend && !sheet.state.canAddFiles)
        sheet.send()
        #expect(fixture.composer.request == nil, "Send waits for the files it will carry")
        gate.release()
        await adding.value
        #expect(sheet.fileBusy == nil && sheet.form.attachments.count == 1 && sheet.state.canSend)
        sheet.close()
    }

    @Test func cancellingTheCopyRemovesWhatWasCopiedAndSaysNothing() async throws {
        let gate = Gate()
        let fixture = try fixture(hold: gate)
        defer { fixture.tearDown() }
        fixture.access.notDownloaded("big.apk")
        let sheet = fixture.sheet()
        let file = try fixture.original("big.apk", bytes: 3_000_000)
        let later = try fixture.original("later.apk")
        let adding = Task { await sheet.addFiles([file, later]) }
        #expect(await eventually { sheet.fileBusy != nil })
        try #require(sheet.fileBusy?.cancel).action()
        gate.release()
        await adding.value
        #expect(sheet.form.attachments.isEmpty, "the rest of the batch is not added either")
        #expect(sheet.failure == nil && sheet.fileBusy == nil && !sheet.state.addingFiles)
        #expect(!sheet.folder.exists || isEmpty(sheet.folder), "no partial copy")
        #expect(fixture.access.starts == ["big.apk"] && fixture.access.stops == ["big.apk"])
        #expect(FileManager.default.fileExists(atPath: file.path))
        #expect(sheet.state.canAddFiles, "the person can try again")
        sheet.close()
    }

    @Test func aCopyThatDoesNotFitIsRefusedWithTheRoomMessageAndLeavesNothing() async throws {
        let fixture = try fixture(free: 1000)
        defer { fixture.tearDown() }
        fixture.access.notDownloaded("huge.apk")
        let file = try fixture.original("huge.apk", bytes: 5000)
        let sheet = fixture.sheet()
        await sheet.addFiles([file])
        #expect(sheet.form.attachments.isEmpty)
        #expect(sheet.failure?.title == "One item wasn\u{2019}t added")
        #expect(sheet.failure?.message == "There isn\u{2019}t enough free space on this iPhone. Free up space, then try again.")
        #expect(!sheet.folder.exists, "no file, no folder")
        #expect(fixture.access.starts == ["huge.apk"] && fixture.access.stops == ["huge.apk"])
        #expect(FileManager.default.fileExists(atPath: file.path))
        sheet.close()
    }

    @Test func aFileThatIsGoneBeforeItsCopyStartsIsReadAsUnavailable() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        // The inspection says the bytes are not here; the provider then fails to give the file.
        let file = fixture.library.appendingPathComponent("vanishing.apk")
        try Data(repeating: 1, count: 100).write(to: file)
        fixture.app.feedbackServices.files.inspect = { _ in
            SendFeedbackFileInspection(kind: .file, bytes: 100, isLocal: false, mappable: true)
        }
        try FileManager.default.removeItem(at: file)
        let sheet = fixture.sheet()
        await sheet.addFiles([file])
        #expect(sheet.failure?.message == "A file couldn\u{2019}t be read. Check that it is still there, then try again.")
        #expect(sheet.form.attachments.isEmpty && (!sheet.folder.exists || isEmpty(sheet.folder)))
        #expect(fixture.access.stops.count == 1)
        sheet.close()
    }

    @Test func closingTheSheetUnderACopyStopsItAndLeavesNothing() async throws {
        let gate = Gate()
        let fixture = try fixture(hold: gate)
        defer { fixture.tearDown() }
        fixture.access.notDownloaded("mid.apk")
        let sheet = fixture.sheet()
        let file = try fixture.original("mid.apk")
        let adding = Task { await sheet.addFiles([file]) }
        #expect(await eventually { sheet.fileBusy != nil })
        sheet.close()
        gate.release()
        await adding.value
        #expect(sheet.form.attachments.isEmpty && sheet.failure == nil && !sheet.folder.exists)
        #expect(fixture.access.stops == ["mid.apk"] && sheet.scopes.count == 0)
        #expect(FileManager.default.fileExists(atPath: file.path))
    }

    // MARK: Between the pick and Send

    @Test func aFileDeletedBeforeSendIsTakenOutAndNothingIsSent() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let keep = try fixture.original("keep.apk"), lose = try fixture.original("lose.apk")
        let sheet = fixture.sheet()
        sheet.form.problem = "x"
        await sheet.addFiles([keep, lose])
        try FileManager.default.removeItem(at: lose)
        sheet.send()
        #expect(fixture.composer.request == nil, "a mail never goes without a file the person added, unseen")
        #expect(sheet.form.attachments.items.map(\.fileName) == ["keep.apk"])
        #expect(sheet.failure?.title == "Attachment not available")
        #expect(sheet.failure?.message == "A file you added is no longer available, so it was taken out of the list. Check the list, then send again.")
        #expect(fixture.access.stops == ["lose.apk"] && sheet.form.problem == "x")
        sheet.failure = nil
        sheet.send()
        #expect(fixture.composer.request?.attachments.map(\.fileName) == ["keep.apk"], "the second try goes")
        sheet.close()
    }

    @Test func aFileTheProviderTookBackBeforeSendIsTakenOutToo() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let file = try fixture.original("evicted.apk")
        let sheet = fixture.sheet()
        sheet.form.problem = "x"
        await sheet.addFiles([file])
        fixture.access.notDownloaded("evicted.apk")
        sheet.send()
        #expect(fixture.composer.request == nil && sheet.form.attachments.isEmpty)
        #expect(sheet.failure?.title == "Attachment not available")
        #expect(fixture.access.stops == ["evicted.apk"] && FileManager.default.fileExists(atPath: file.path))
        sheet.close()
    }

    @Test func aFileThatChangedBeforeSendGoesAsItIsNowWithItsNewSize() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let file = try fixture.original("rebuilt.apk", bytes: 1000, fill: 1)
        let sheet = fixture.sheet()
        sheet.form.problem = "x"
        await sheet.addFiles([file])
        try Data(repeating: 2, count: 21 << 20).write(to: file)
        sheet.send()
        let sent = try #require(fixture.composer.request?.attachments.first)
        #expect(sent.bytes == 21 << 20 && sheet.form.attachments.items[0].bytes == 21 << 20)
        #expect(sheet.state.attachments.exceedsWarningSize, "the total and the warning follow the file")
        #expect(try sent.mappedData().prefix(4) == Data(repeating: 2, count: 4))
        #expect(sheet.failure == nil)
        sheet.close()
    }

    @Test func aFileThatBecameEmptyBeforeSendIsTakenOut() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let file = try fixture.original("emptied.apk")
        let sheet = fixture.sheet()
        sheet.form.problem = "x"
        await sheet.addFiles([file])
        try Data().write(to: file)
        sheet.send()
        #expect(fixture.composer.request == nil && sheet.form.attachments.isEmpty)
        #expect(sheet.failure?.title == "Attachment not available")
        sheet.close()
    }

    @Test func aStagedFileTheSystemTookAwayIsAlsoSaidNotSilentlyLeftOut() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let sheet = fixture.sheet()
        sheet.form.problem = "x"
        sheet.takeScreenshot()
        #expect(await eventually { sheet.form.attachments.count == 1 })
        try FileManager.default.removeItem(at: sheet.form.attachments.items[0].url)
        sheet.send()
        #expect(fixture.composer.request == nil && sheet.form.attachments.isEmpty)
        #expect(sheet.failure?.title == "Attachment not available")
        sheet.close()
    }

    // MARK: The picker

    @Test func theFilesButtonOpensThePickerAndWhatItReturnsIsAdded() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        var deliver: (@MainActor ([URL]) -> Void)?
        var opened = 0
        fixture.app.feedbackServices.pickFiles = { picked in
            opened += 1
            deliver = picked
            return true
        }
        let sheet = fixture.sheet()
        sheet.pickFiles()
        #expect(opened == 1)
        deliver?([try fixture.original("picked.apk")])
        #expect(await eventually { sheet.form.attachments.count == 1 })
        #expect(sheet.form.attachments.items[0].inPlace)
        // Nothing opens while something else is under way.
        sheet.form.takingScreenshot = true
        sheet.pickFiles()
        #expect(opened == 1)
        sheet.form.takingScreenshot = false
        sheet.close()
    }

    // MARK: The sheet as it is drawn

    private func nodes(in node: NSObject, depth: Int = 0) -> [NSObject] {
        guard depth < 64 else { return [] }
        var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = node as? UIView { children += view.subviews }
        return [node] + children.flatMap { nodes(in: $0, depth: depth + 1) }
    }

    private func element(identifier: String, in root: UIView) -> NSObject? {
        nodes(in: root).first { ($0 as? UIAccessibilityIdentification)?.accessibilityIdentifier == identifier }
    }

    /// The sheet on the application's window, the way a person meets it. A hook of the preview opens it
    /// on a state; the picker is the injected source of URLs.
    private func hostSheet(
        _ fixture: Fixture, in lease: HostWindow.Lease, prepare: @escaping @MainActor (SendFeedbackModel) async -> Void = { _ in }
    ) -> UIHostingController<some View> {
        fixture.app.feedbackServices.previewPrepare = prepare
        let controller = UIHostingController(
            rootView: SendFeedbackSheet(app: fixture.app).environment(fixture.app).frame(width: 390, height: 844))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        return controller
    }

    @Test func theBarHasAFilesButtonThatOpensThePickerAndTheRowAppearsWithItsLabelsAndCanBeRemoved() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let file = try fixture.original("example-app.apk", bytes: 25 << 20)
        var deliver: (@MainActor ([URL]) -> Void)?
        fixture.app.feedbackServices.pickFiles = { picked in
            deliver = picked
            return true
        }
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostSheet(fixture, in: lease)
        var button: NSObject?
        let drawn = await eventually {
            controller.view.setNeedsLayout()
            controller.view.layoutIfNeeded()
            button = element(identifier: "feedback-files", in: controller.view)
            return button != nil
        }
        let seen = nodes(in: controller.view).map {
            "\(($0 as? UIAccessibilityIdentification)?.accessibilityIdentifier ?? "-")|\($0.accessibilityLabel ?? "-")"
        }
        try #require(drawn, "the sheet was not drawn; is ApplicationAccessibilityEnabled on? \(seen)")
        #expect(button?.accessibilityLabel == "Add files", "VoiceOver names the button by what it does")
        _ = button?.accessibilityActivate()
        #expect(await eventually { deliver != nil }, "the button opens the injected picker")
        deliver?([file])
        var row: NSObject?
        let attached = await eventually {
            controller.view.layoutIfNeeded()
            row = element(identifier: "feedback-attachment", in: controller.view)
            return row != nil
        }
        try #require(attached, "the row was not drawn")
        #expect(row?.accessibilityLabel == "File, example-app.apk, 25 MiB")
        let total = try #require(element(identifier: "feedback-total", in: controller.view))
        let words = nodes(in: controller.view).compactMap(\.accessibilityLabel)
        #expect(words.contains("Remove file"), "\(words)")
        #expect(words.contains { $0.contains("1 attachment") && $0.contains("25 MiB") && $0.contains("20 MiB") }
                || total.accessibilityLabel?.contains("20 MiB") == true,
                "the total and the warning for 25 MiB: \(words)")
        let remove = try #require(nodes(in: controller.view).first { $0.accessibilityLabel == "Remove file" })
        _ = remove.accessibilityActivate()
        #expect(await eventually {
            controller.view.layoutIfNeeded()
            return element(identifier: "feedback-attachment", in: controller.view) == nil
        }, "removing the row takes it out")
        #expect(FileManager.default.fileExists(atPath: file.path), "the file itself stays")
    }

    @Test func theAlertSaysEachReasonOnItsOwnLine() async throws {
        let fixture = try fixture()
        defer { fixture.tearDown() }
        let folder = fixture.library.appendingPathComponent("A Folder", isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let empty = try fixture.original("empty.txt", bytes: 0)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostSheet(fixture, in: lease) { model in await model.addFiles([folder, empty]) }
        var alert: UIAlertController?
        let shown = await eventually {
            alert = controller.presentedViewController as? UIAlertController
            return alert != nil
        }
        try #require(shown, "the alert was not presented")
        #expect(alert?.title == "2 items weren\u{2019}t added")
        #expect(alert?.message == """
            A folder or a package can\u{2019}t be attached. Compress it in Files first.
            An empty file can\u{2019}t be attached.
            """)
        #expect(alert?.actions.map(\.title) == ["OK"])
    }

    @Test func theRowSpeaksItsKindItsNameAndItsSizeAndFilesHaveTheirOwnWords() {
        let file = SendFeedbackAttachment(
            kind: .file, url: URL(fileURLWithPath: "/Files/a b.apk"), bytes: 2_097_152, displayName: "a b.apk", inPlace: true)
        #expect(SendFeedbackSheet.spoken(file) == "File, a b.apk, 2 MiB")
        let nameless = SendFeedbackAttachment(
            kind: .file, url: URL(fileURLWithPath: "/Files/x.bin"), bytes: 10, mailName: "x.bin", inPlace: true)
        #expect(SendFeedbackSheet.spoken(nameless) == "File, x.bin, 10 B")
        #expect(String(localized: SendFeedbackSheet.removeLabel(.file)) == "Remove file")
        #expect(SendFeedbackSheet.detail(file) == "2 MiB", "a file shows no picture size and no length")
        #expect(SendFeedbackSheet.symbol(.file) == "doc.fill")
    }

    /// Reads the deprecated mode on purpose: it is the only way to see that the picker opens in place.
    @available(iOS, deprecated: 14.0, message: "reads the picker's mode, which has no other accessor")
    @Test func thePickerOpensFilesWhereTheyAreKeptAndAllowsSeveral() {
        let picker = SendFeedbackFilePicker.makePicker()
        #expect(picker.allowsMultipleSelection)
        #expect(picker.documentPickerMode == .open, "open in place, not an import")
    }
}

#endif
