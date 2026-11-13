// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing

@testable import AndariumCore
@testable import AndariumFeedbackLogic

/// The files Send feedback takes from Files: what an attachment of that kind
/// is, how a picked file is looked at without being read, the access held to
/// it, the copy of a file that cannot be used where it is, and the names. The
/// package compiles the application's own sources for this.
struct SendFeedbackFilesTests {
    private let mebibyte: UInt64 = 1 << 20

    private func scratch() throws -> URL {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-feedback-files-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }

    /// A file of `bytes` bytes that is mostly a hole: the first byte is real, the rest reads as zeros
    /// and takes no space, so a multi-gigabyte file costs nothing to make.
    private func sparse(_ url: URL, bytes: UInt64) throws {
        try Data([0x50]).write(to: url)
        let handle = try FileHandle(forWritingTo: url)
        defer { try? handle.close() }
        try handle.truncate(atOffset: bytes)
    }

    /// The process's footprint: what its own memory costs the system.
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

    /// What the footprint grew by since `before`; a footprint that shrank grew by nothing.
    private func growth(since before: UInt64) -> UInt64 {
        let now = footprint()
        return now > before ? now - before : 0
    }

    private func attachment(
        _ kind: SendFeedbackAttachment.Kind, bytes: UInt64 = 1, path: String, inPlace: Bool = false, mailName: String? = nil
    ) -> SendFeedbackAttachment {
        SendFeedbackAttachment(kind: kind, url: URL(fileURLWithPath: path), bytes: bytes, mailName: mailName, inPlace: inPlace)
    }

    private nonisolated final class Counter: @unchecked Sendable {
        private let lock = NSLock()
        private var values: [String] = []
        func add(_ value: String) { lock.withLock { values.append(value) } }
        var all: [String] { lock.withLock { values } }
    }

    // MARK: The list

    @Test func aFileIsAnAttachmentLikeTheOthersAndCountsInTheTotalAndTheWarning() {
        var set = SendFeedbackAttachments()
        set.add(attachment(.photo, bytes: 5, path: "/staged/photo.png"))
        set.add(attachment(.file, bytes: 20 * mebibyte, path: "/Files/app.apk", inPlace: true))
        set.add(attachment(.bugReport, bytes: 3, path: "/staged/report.zip"))
        #expect(set.items.map(\.kind) == [.bugReport, .photo, .file], "a report first, the rest in the order added")
        #expect(set.totalBytes == 20 * mebibyte + 8)
        #expect(set.exceedsWarningSize, "the warning counts the files: 20 MiB and 8 bytes")
        let file = set.items[2]
        set.remove(file.id)
        #expect(!set.exceedsWarningSize && set.count == 2)
    }

    @Test func twoAttachmentsNeverShareAMailNameWhateverTheLettersCase() {
        var set = SendFeedbackAttachments()
        set.add(attachment(.screenshot, path: "/staged/screenshot.png"))
        set.add(attachment(.file, path: "/Files/Screenshot.PNG", inPlace: true, mailName: "Screenshot.PNG"))
        set.add(attachment(.file, path: "/Other/screenshot.png", inPlace: true, mailName: "screenshot.png"))
        set.add(attachment(.file, path: "/Other/SCREENSHOT-2.png", inPlace: true, mailName: "SCREENSHOT-2.png"))
        #expect(set.items.map(\.fileName) == ["screenshot.png", "Screenshot-2.PNG", "screenshot-3.png", "SCREENSHOT-2-2.png"])
        // The staged one keeps its own name.
        #expect(set.items[0].mailName == nil)
        // A bug report that replaces the one there is not renamed after it.
        set.add(attachment(.bugReport, path: "/staged/bugreport.zip"))
        let replaced = set.add(attachment(.bugReport, path: "/staged/bugreport.zip"))
        #expect(replaced?.fileName == "bugreport.zip")
        #expect(set.bugReport?.fileName == "bugreport.zip")
    }

    @Test func theListMapsEveryFileBeforeTheComposerIsGivenOneAndNamesTheOnesThatFailed() throws {
        let root = try scratch()
        defer { try? FileManager.default.removeItem(at: root) }
        let a = root.appendingPathComponent("a.bin"), b = root.appendingPathComponent("b.bin"), c = root.appendingPathComponent("c.bin")
        try Data([1, 2, 3]).write(to: a)
        try Data([4, 5]).write(to: b)
        try Data([6]).write(to: c)
        var set = SendFeedbackAttachments()
        for (url, size) in [(a, 3), (b, 2), (c, 1)] {
            set.add(SendFeedbackAttachment(kind: .file, url: url, bytes: UInt64(size), inPlace: true))
        }
        guard case .success(let data) = set.mapAll() else {
            Issue.record("every file is there: they map")
            return
        }
        #expect(data == [Data([1, 2, 3]), Data([4, 5]), Data([6])], "in the order of the list")
        // A file that went since the last look is not skipped: it fails the whole, by name.
        try FileManager.default.removeItem(at: b)
        try FileManager.default.removeItem(at: c)
        guard case .failure(let failure) = set.mapAll() else {
            Issue.record("a file is gone: the mapping fails")
            return
        }
        #expect(failure.attachments == [set.items[1].id, set.items[2].id])
        #expect(SendFeedbackAttachments().mapAll() == .success([]), "no files, nothing to map")
    }

    @Test func anApkIsTypedAsAnAndroidPackageAndTheOthersByTheirExtension() {
        #expect(attachment(.file, path: "/Files/app.APK").mimeType == "application/vnd.android.package-archive")
        #expect(attachment(.file, path: "/Files/log.txt").mimeType == "text/plain")
        #expect(attachment(.file, path: "/Files/blob.unknownext").mimeType == "application/octet-stream")
        #expect(attachment(.file, path: "/Files/noextension").mimeType == "application/octet-stream")
        // The mail's name decides, not the place the file is kept.
        #expect(attachment(.file, path: "/Files/x.bin", mailName: "y.zip").mimeType == "application/zip")
    }

    // MARK: Names

    @Test func aMailNameIsPlainAndCannotBeMangled() {
        func safe(_ name: String) -> String { SendFeedbackFolder.safeName(name) }
        #expect(safe("My App (v2).apk") == "My_App__v2_.apk")
        #expect(safe("a\"b;c,d.apk") == "a_b_c_d.apk", "quotes, semicolons and commas split a header")
        #expect(safe("line\nbreak\r.apk") == "line_break_.apk", "a line break ends a header")
        #expect(safe("evil\u{202E}gpj.apk") == "evil_gpj.apk", "a right-to-left override reorders what is read")
        #expect(safe("a/b\\c:d.apk") == "a_b_c_d.apk", "separators")
        #expect(safe("\u{5E94}\u{7528}\u{1F4F1}.apk") == "___.apk", "every character outside ASCII letters and digits")
        #expect(safe("..hidden.apk") == "hidden.apk" && safe("...") == "file" && safe("") == "file")
        #expect(safe("../../etc/passwd") == "_.._etc_passwd")
    }

    @Test func aLongNameKeepsItsExtension() {
        let long = String(repeating: "a", count: 200) + ".apk"
        let name = SendFeedbackFolder.safeName(long)
        #expect(name.count == 100 && name.hasSuffix(".apk"))
        #expect(SendFeedbackFolder.safeName(String(repeating: "a", count: 300)).count == 100)
        // A tail that is no extension (too long) is cut like the rest.
        let odd = String(repeating: "a", count: 90) + "." + String(repeating: "b", count: 40)
        #expect(SendFeedbackFolder.safeName(odd).count == 100)
    }

    @Test func aNumberedNameStaysInsideTheLimitAndTwoLongNamesStayTwo() {
        let long = String(repeating: "a", count: 96) + ".apk"
        #expect(long.count == 100 && SendFeedbackNames.numbered(long, 2) == String(repeating: "a", count: 94) + "-2.apk")
        #expect(SendFeedbackNames.numbered("app.apk", 2) == "app-2.apk" && SendFeedbackNames.numbered("app", 12) == "app-12")
        // Two names that fill the limit meet in a set: both keep to it, and are two.
        var set = SendFeedbackAttachments()
        let name = SendFeedbackFolder.safeName(String(repeating: "b", count: 300) + ".apk")
        for _ in 0..<12 {
            set.add(SendFeedbackAttachment(kind: .file, url: URL(fileURLWithPath: "/Files/x.apk"), bytes: 1, mailName: name, inPlace: true))
        }
        let names = set.items.map(\.fileName)
        #expect(names.allSatisfy { $0.count <= 100 && $0.hasSuffix(".apk") })
        #expect(Set(names.map { $0.lowercased() }).count == 12, "no two share a name")
        #expect(names[1].hasSuffix("-2.apk") && names[11].hasSuffix("-12.apk"))
        // A long tail that is no extension is part of the name; the number still fits.
        let odd = String(repeating: "c", count: 90) + "." + String(repeating: "d", count: 9)
        #expect(SendFeedbackNames.numbered(odd + "ddddddddddd", 3).count <= 100)
    }

    @Test func aNameOnTheRowLosesOnlyWhatAScreenShouldNotObey() {
        #expect(SendFeedbackNames.display("\u{5E94}\u{7528} (1).apk") == "\u{5E94}\u{7528} (1).apk")
        #expect(SendFeedbackNames.display("evil\u{202E}gpj.apk") == "evilgpj.apk")
        #expect(SendFeedbackNames.display("two\nlines\u{200B}.apk") == "twolines.apk")
        #expect(SendFeedbackNames.display(" \n\u{202E} ") == nil)
        #expect(SendFeedbackNames.display("") == nil)
    }

    // MARK: Looking at a file

    @Test func aFileIsLookedAtWithoutBeingReadAndAFolderOrAPackageIsNotAFile() throws {
        let root = try scratch()
        defer { try? FileManager.default.removeItem(at: root) }
        let file = root.appendingPathComponent("app.apk")
        try Data(repeating: 1, count: 5000).write(to: file)
        let found = SendFeedbackFileInspection.read(file)
        #expect(found.kind == .file && found.bytes == 5000 && found.isLocal && found.mappable && found.usableInPlace)
        let folder = root.appendingPathComponent("folder", isDirectory: true)
        let package = root.appendingPathComponent("Thing.app", isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: package, withIntermediateDirectories: true)
        #expect(SendFeedbackFileInspection.read(folder).kind == .folder && !SendFeedbackFileInspection.read(folder).usableInPlace)
        #expect(SendFeedbackFileInspection.read(package).kind == .folder, "a package is a folder on disk")
        #expect(SendFeedbackFileInspection.read(root.appendingPathComponent("gone.apk")).kind == .missing)
        let empty = root.appendingPathComponent("empty.apk")
        try Data().write(to: empty)
        let none = SendFeedbackFileInspection.read(empty)
        #expect(none.kind == .file && none.bytes == 0 && !none.usableInPlace, "an empty file is a file that cannot be attached")
    }

    @Test func aFileIsUsableInPlaceOnlyWhenItIsHereAndMapsAndHasBytes() {
        func inspection(
            _ kind: SendFeedbackFileInspection.Kind = .file, bytes: UInt64 = 10, local: Bool = true, mappable: Bool = true
        ) -> SendFeedbackFileInspection {
            SendFeedbackFileInspection(kind: kind, bytes: bytes, isLocal: local, mappable: mappable)
        }
        #expect(inspection().usableInPlace)
        #expect(!inspection(local: false).usableInPlace, "a provider's placeholder is not downloaded")
        #expect(!inspection(mappable: false).usableInPlace, "a volume or a provider that refuses mapping")
        #expect(!inspection(bytes: 0).usableInPlace)
        #expect(!inspection(.folder).usableInPlace && !inspection(.missing).usableInPlace && !inspection(.other).usableInPlace)
    }

    /// An APK can be several gigabytes: its size is a `stat`, and the composer's
    /// mapping reads nothing until the composer does.
    @Test func aFileOfSeveralGiBIsInspectedAndMappedWithoutBeingReadIntoMemory() throws {
        let root = try scratch()
        defer { try? FileManager.default.removeItem(at: root) }
        let file = root.appendingPathComponent("big.apk")
        let size = 3 << 30
        try sparse(file, bytes: UInt64(size))
        let before = footprint()
        let found = SendFeedbackFileInspection.read(file)
        #expect(found.usableInPlace && found.bytes == UInt64(size))
        let item = SendFeedbackAttachment(kind: .file, url: file, bytes: found.bytes, inPlace: true)
        let data = try item.mappedData()
        #expect(data.count == size)
        #expect(data[0] == 0x50 && data[size - 1] == 0, "the ends are there, and read on demand")
        let grown = growth(since: before)
        // The process runs other tests alongside, which allocate: the bound is half the file, which a read would pass.
        #expect(grown < 1536 * mebibyte, "\(grown / mebibyte) MiB grew for a 3 GiB file")
    }

    // MARK: The access

    @Test func everyAccessThatStartedIsStoppedExactlyOnce() {
        let log = Counter()
        let scopes = SendFeedbackScopes(system: SendFeedbackFileSystem(
            start: { url in
                log.add("start \(url.lastPathComponent)")
                return url.lastPathComponent != "own.bin"
            },
            stop: { log.add("stop \($0.lastPathComponent)") }))
        let first = UUID(), second = UUID(), own = UUID(), third = UUID()
        #expect(scopes.hold(first, URL(fileURLWithPath: "/a/one.bin")))
        #expect(scopes.hold(first, URL(fileURLWithPath: "/a/one.bin")), "one access per attachment")
        #expect(scopes.hold(second, URL(fileURLWithPath: "/a/two.bin")))
        #expect(!scopes.hold(own, URL(fileURLWithPath: "/a/own.bin")), "a file of the app's own carries none")
        #expect(scopes.count == 2)
        scopes.release(first)
        scopes.release(first)
        scopes.release(own)
        #expect(log.all == ["start one.bin", "start two.bin", "start own.bin", "stop one.bin"])
        scopes.hold(third, URL(fileURLWithPath: "/a/three.bin"))
        scopes.releaseAll()
        scopes.releaseAll()
        #expect(scopes.count == 0)
        #expect(log.all.sorted() == ["start one.bin", "start own.bin", "start three.bin", "start two.bin",
                                     "stop one.bin", "stop three.bin", "stop two.bin"])
    }

    @Test func aCancellationRunsItsHookOnceAndAtOnceWhenItIsLate() {
        let hooks = Counter()
        let token = SendFeedbackCancellation()
        token.onCancel { hooks.add("first") }
        #expect(!token.isCancelled)
        token.cancel()
        token.cancel()
        #expect(token.isCancelled && hooks.all == ["first"])
        token.onCancel { hooks.add("late") }
        #expect(hooks.all == ["first", "late"])
        let cleared = SendFeedbackCancellation()
        cleared.onCancel { hooks.add("cleared") }
        cleared.clearHook()
        cleared.cancel()
        #expect(hooks.all == ["first", "late"])
    }

    // MARK: Copying what cannot be used where it is

    private func folder(
        _ root: URL, free: UInt64? = nil
    ) -> SendFeedbackFolder {
        SendFeedbackStaging(root: root.appendingPathComponent("Feedback"), freeBytes: { _ in free }).makeFolder()
    }

    private func copy(
        _ folder: SendFeedbackFolder, _ source: URL, named name: String = "app.apk",
        cancellation: SendFeedbackCancellation = SendFeedbackCancellation(), progress: @escaping @Sendable (UInt64) -> Void = { _ in }
    ) throws -> URL {
        let bytes = UInt64((try? source.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        return try folder.stageCopy(of: source, bytes: bytes, named: name, cancellation: cancellation, progress: progress)
    }

    @Test func aCopyHasTheFilesBytesAndLeavesTheOriginalAlone() throws {
        let root = try scratch()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("Source App (1).apk")
        let content = Data((0..<3_000_000).map { UInt8(truncatingIfNeeded: $0 &* 31) })
        try content.write(to: source)
        let folder = folder(root)
        let reports = Counter()
        let staged = try copy(folder, source, named: source.lastPathComponent) { reports.add("\($0)") }
        #expect(staged.deletingLastPathComponent().resolvingSymlinksInPath() == folder.url.resolvingSymlinksInPath())
        #expect(staged.lastPathComponent == "Source_App__1_.apk")
        #expect(try Data(contentsOf: staged) == content)
        #expect(try Data(contentsOf: source) == content, "the person's file is not touched")
        #expect(reports.all.last == "3000000" && reports.all.compactMap(UInt64.init) == reports.all.compactMap(UInt64.init).sorted())
        #expect((try FileManager.default.attributesOfItem(atPath: staged.path)[.posixPermissions] as? Int) == 0o600)
    }

    @Test func aCopyWhoseNameIsTakenIsNumberedWhateverTheLettersCase() throws {
        let root = try scratch()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("s.bin")
        try Data([1, 2, 3]).write(to: source)
        let folder = folder(root)
        let one = try copy(folder, source, named: "APP.apk")
        let two = try copy(folder, source, named: "app.apk")
        let three = try copy(folder, source, named: "App.APK")
        #expect([one, two, three].map(\.lastPathComponent) == ["APP.apk", "app-2.apk", "App-3.APK"])
        // Two names asked for before either file is written are still two.
        let a = try folder.reserve("x.png"), b = try folder.reserve("X.png")
        #expect(a.lastPathComponent == "x.png" && b.lastPathComponent == "X-2.png")
        // Names that fill the limit stay inside it when numbered.
        let long = String(repeating: "n", count: 200) + ".bin"
        let first = try folder.reserve(long), second = try folder.reserve(long)
        #expect(first.lastPathComponent.count == 100 && second.lastPathComponent.count == 100)
        #expect(first.lastPathComponent != second.lastPathComponent)
    }

    @Test func aCopyIsRefusedBeforeItsFirstByteWhenItDoesNotFitAndLeavesNothing() throws {
        let root = try scratch()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("big.apk")
        try Data(repeating: 9, count: 4096).write(to: source)
        let needed = UInt64(4096) + SendFeedbackStaging.spareBytes
        let short = folder(root, free: needed - 1)
        #expect(throws: SendFeedbackFileRefusal.notEnoughSpace) { try copy(short, source) }
        #expect(!short.exists, "no file, no folder")
        #expect(FileManager.default.fileExists(atPath: source.path))
        let enough = folder(root, free: needed)
        let staged = try copy(enough, source)
        #expect(try Data(contentsOf: staged).count == 4096)
        // An unreadable figure is no refusal: the write says when the room runs out.
        let unknown = folder(root, free: nil)
        #expect(try Data(contentsOf: try copy(unknown, source)).count == 4096)
    }

    @Test func cancellingACopyRemovesWhatWasCopied() throws {
        let root = try scratch()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("big.apk")
        try sparse(source, bytes: 16 * mebibyte)
        let folder = folder(root)
        let token = SendFeedbackCancellation()
        let reports = Counter()
        #expect(throws: SendFeedbackFileRefusal.cancelled) {
            try copy(folder, source, cancellation: token) { copied in
                reports.add("\(copied)")
                token.cancel()
            }
        }
        #expect(reports.all.count == 1, "it stopped at the next piece")
        #expect(try FileManager.default.contentsOfDirectory(atPath: folder.url.path).isEmpty, "no partial copy")
        #expect(FileManager.default.fileExists(atPath: source.path))
        // Cancelled before it began.
        #expect(throws: SendFeedbackFileRefusal.cancelled) { try copy(folder, source, cancellation: token) }
        #expect(try FileManager.default.contentsOfDirectory(atPath: folder.url.path).isEmpty)
    }

    @Test func aSourceThatIsGoneOrEmptyIsRefusedAndLeavesNothing() throws {
        let root = try scratch()
        defer { try? FileManager.default.removeItem(at: root) }
        let folder = folder(root)
        let gone = root.appendingPathComponent("gone.apk")
        #expect(throws: SendFeedbackFileRefusal.unavailable) {
            try folder.stageCopy(of: gone, bytes: 10, named: "gone.apk", cancellation: SendFeedbackCancellation()) { _ in }
        }
        let empty = root.appendingPathComponent("empty.apk")
        try Data().write(to: empty)
        #expect(throws: SendFeedbackFileRefusal.empty) {
            try folder.stageCopy(of: empty, bytes: 10, named: "empty.apk", cancellation: SendFeedbackCancellation()) { _ in }
        }
        #expect(try FileManager.default.contentsOfDirectory(atPath: folder.url.path).isEmpty)
    }

    @Test func aCopyOfALargeFileIsStreamedNotHeldInMemory() throws {
        let root = try scratch()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("large.apk")
        try sparse(source, bytes: 600 * mebibyte)
        let folder = folder(root)
        let before = footprint()
        let staged = try copy(folder, source)
        let grown = growth(since: before)
        #expect((try FileManager.default.attributesOfItem(atPath: staged.path)[.size] as? UInt64) == 600 * mebibyte)
        // The process runs other tests alongside, which allocate: the bound is three quarters of the file, which a
        // copy that held the file would pass.
        #expect(grown < 450 * mebibyte, "\(grown / mebibyte) MiB grew for a 600 MiB copy")
    }

    @Test func onlyAFileOfTheFolderIsRemovedByIt() throws {
        let root = try scratch()
        defer { try? FileManager.default.removeItem(at: root) }
        let folder = folder(root)
        let own = try folder.reserve("mine.bin")
        try Data([1]).write(to: own)
        let theirs = root.appendingPathComponent("theirs.apk")
        try Data([2]).write(to: theirs)
        folder.remove(theirs)
        #expect(FileManager.default.fileExists(atPath: theirs.path), "the person's own file is never deleted")
        // Not by a name that only looks like the folder's own either.
        let sibling = folder.url.deletingLastPathComponent().appendingPathComponent("elsewhere/mine.bin")
        try FileManager.default.createDirectory(at: sibling.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data([3]).write(to: sibling)
        folder.remove(sibling)
        #expect(FileManager.default.fileExists(atPath: sibling.path))
        folder.remove(own)
        #expect(!FileManager.default.fileExists(atPath: own.path))
        #expect(try folder.reserve("MINE.bin").lastPathComponent == "MINE.bin", "its name is free again")
    }

    // MARK: The form

    @Test func addingFilesFollowsTheSameRulesAsAddingPhotos() {
        var form = SendFeedbackForm(mailAvailable: true, running: false)
        form.problem = "x"
        #expect(form.canAddFiles, "from Home too: a file needs no virtual machine")
        form.addingFiles = true
        #expect(form.isBusy && !form.canSend && !form.canAddFiles && !form.canAddMedia && !form.canTakeScreenshot)
        form.addingFiles = false
        form.importingMedia = true
        #expect(!form.canAddFiles)
        form.importingMedia = false
        form.mailAvailable = false
        #expect(!form.canAddFiles)
        form.mailAvailable = true
        form.attachments.add(attachment(.file, path: "/Files/a.apk", inPlace: true))
        #expect(form.hasContent, "closing asks once a file is attached")
    }
}
