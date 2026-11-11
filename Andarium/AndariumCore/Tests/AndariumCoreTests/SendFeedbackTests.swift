// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore
@testable import AndariumFeedbackLogic

/// Send feedback's logic: the mail, the information block, the attachments, the
/// enabling of every control and the staging folder. The package compiles the
/// application's own sources for this, with Send feedback's condition set.
struct SendFeedbackTests {
    private func information(
        machine: SendFeedbackInformation.VirtualMachine? = .init(
            vcpus: 6, memoryMiB: 2560, display: "1206 x 2622 px, Retina", graphicsMiB: 1157),
        running: Int? = 2520
    ) -> SendFeedbackInformation {
        SendFeedbackInformation(
            appVersion: "1.0", build: "9", configuration: "AppStore", deviceModel: "iPhone18,1",
            systemVersion: "27.2", language: "en-US", virtualMachine: machine, androidRelease: "r20",
            runningSeconds: running, freeSpaceBytes: 44_700_000_000, thermalState: "nominal")
    }

    private func attachment(_ kind: SendFeedbackAttachment.Kind, bytes: UInt64, name: String = "file.bin")
        -> SendFeedbackAttachment
    {
        SendFeedbackAttachment(kind: kind, url: URL(fileURLWithPath: "/staged/\(name)"), bytes: bytes)
    }

    // MARK: The mail

    @Test func theSubjectIsThePrefixAndTheProblemOnOneLine() {
        #expect(SendFeedbackMail.subject(problem: "Game freezes") == "Andarium feedback: Game freezes")
        #expect(SendFeedbackMail.subject(problem: "  Game \n freezes\t after\n") == "Andarium feedback: Game freezes after")
    }

    @Test func theBodyIsTheDetailsASeparatorAndTheInformation() {
        let body = SendFeedbackMail.body(details: "  It stops.\nEvery time.  \n", information: information())
        #expect(body.hasPrefix("It stops.\nEvery time.\n\n--\nApp: Andarium 1.0 (9), AppStore\n"))
        #expect(body.hasSuffix("Thermal state: nominal\n"))
    }

    @Test func withoutDetailsTheBodyLeavesRoomToType() {
        let body = SendFeedbackMail.body(details: " \n ", information: information())
        #expect(body.hasPrefix("\n\n--\nApp: "))
    }

    @Test func anAddressNeedsALocalPartAndADottedDomain() {
        #expect(SendFeedbackMail.isWellFormed(address: "feedback@example.com"))
        #expect(SendFeedbackMail.isWellFormed(address: "a.b+c@mail.example.org"))
        for bad in ["", "feedback", "feedback@", "@example.com", "a@b", "a@b.", "a@.com", "a b@example.com", "a@b@c.com"] {
            #expect(!SendFeedbackMail.isWellFormed(address: bad), "\(bad)")
        }
    }

    // MARK: The information block

    @Test func theInformationBlockNamesTheAppTheDeviceAndTheVirtualMachine() {
        #expect(information().lines == [
            "App: Andarium 1.0 (9), AppStore",
            "Device: iPhone18,1",
            "iOS: 27.2",
            "Language: en-US",
            "Virtual machine: 6 vCPUs, 2.5 GiB memory, display 1206 x 2622 px, Retina, graphics memory about 1.13 GiB",
            "Android: r20",
            "Running: yes, for 42 min",
            "Free space: 41.6 GiB",
            "Thermal state: nominal",
        ])
    }

    @Test func beforeAVirtualMachineExistsOrRunsTheBlockSaysSo() {
        let none = information(machine: nil, running: nil)
        #expect(none.lines.contains("Virtual machine: not created"))
        #expect(none.lines.contains("Running: no"))
        let stopped = information(running: nil)
        #expect(stopped.lines.contains("Running: no"))
        #expect(stopped.lines.contains { $0.hasPrefix("Virtual machine: 6 vCPUs") })
        let withoutEstimate = information(
            machine: .init(vcpus: 1, memoryMiB: 1536, display: "1x", graphicsMiB: nil))
        #expect(withoutEstimate.lines.contains("Virtual machine: 1 vCPU, 1.5 GiB memory, display 1x"))
    }

    @Test func aRunningTimeIsInTheLargestUnitThatFits() {
        #expect(SendFeedbackInformation.duration(0) == "0 s")
        #expect(SendFeedbackInformation.duration(59) == "59 s")
        #expect(SendFeedbackInformation.duration(60) == "1 min")
        #expect(SendFeedbackInformation.duration(3599) == "59 min")
        #expect(SendFeedbackInformation.duration(3600) == "1 h 0 min")
        #expect(SendFeedbackInformation.duration(4320) == "1 h 12 min")
        #expect(SendFeedbackInformation.duration(-5) == "0 s")
    }

    @Test func aValueWithALineBreakStaysOneLine() {
        var info = information()
        info.language = "en\nUS"
        info.androidRelease = "r20\r\nextra"
        #expect(info.lines.count == 9)
        #expect(info.lines.allSatisfy { !$0.contains("\n") && !$0.contains("\r") })
    }

    @Test func theBlockHoldsNothingThatSinglesOutTheDeviceOrThePerson() {
        // The type has no field for a name, an identifier, an account or a
        // path; the text a normal value gives has no path and no address.
        let text = information().text
        #expect(!text.contains("/"))
        #expect(!text.contains("@"))
        #expect(!text.lowercased().contains("name"))
        #expect(!text.lowercased().contains("account"))
    }

    // MARK: Attachments

    @Test func theTotalIsTheSumOfTheSizesAndTheWarningStartsAboveTwentyMiB() {
        var set = SendFeedbackAttachments()
        #expect(set.isEmpty && set.totalBytes == 0 && !set.exceedsWarningSize)
        set.add(attachment(.bugReport, bytes: 8 << 20))
        set.add(attachment(.screenshot, bytes: 2 << 20))
        set.add(attachment(.video, bytes: 10 << 20))
        #expect(set.count == 3 && set.totalBytes == 20 << 20)
        #expect(!set.exceedsWarningSize, "exactly 20 MiB does not warn")
        set.add(attachment(.photo, bytes: 1))
        #expect(set.exceedsWarningSize)
    }

    @Test func removingAFileGivesItBackAndLowersTheTotal() {
        var set = SendFeedbackAttachments()
        let photo = attachment(.photo, bytes: 3 << 20)
        set.add(attachment(.screenshot, bytes: 1 << 20))
        set.add(photo)
        #expect(set.remove(photo.id) == photo)
        #expect(set.totalBytes == 1 << 20)
        #expect(set.remove(photo.id) == nil)
    }

    @Test func aBugReportGoesFirstAndReplacesTheOneThatIsThere() {
        var set = SendFeedbackAttachments()
        set.add(attachment(.photo, bytes: 5))
        let first = attachment(.bugReport, bytes: 100, name: "a.zip")
        #expect(set.add(first) == nil)
        #expect(set.items.map(\.kind) == [.bugReport, .photo])
        let second = attachment(.bugReport, bytes: 200, name: "b.zip")
        #expect(set.add(second) == first)
        #expect(set.items.map(\.kind) == [.bugReport, .photo])
        #expect(set.bugReport == second)
    }

    @Test func aFileHasItsMimeTypeFromItsExtension() {
        func mime(_ name: String) -> String { attachment(.photo, bytes: 1, name: name).mimeType }
        #expect(mime("a.png") == "image/png")
        #expect(mime("a.jpeg") == "image/jpeg")
        #expect(mime("a.mov") == "video/quicktime")
        #expect(mime("a.zip") == "application/zip")
        #expect(mime("a.unknownext") == "application/octet-stream")
    }

    // MARK: The form

    @Test func sendWaitsForAProblem() {
        var form = SendFeedbackForm(mailAvailable: true, running: true)
        #expect(!form.canSend)
        form.problem = "  \n "
        #expect(!form.canSend)
        form.problem = "It froze"
        #expect(form.canSend)
        form.details = "more"
        #expect(form.canSend)
    }

    @Test func whileAReportIsMadeNothingIsEditedOrAddedOrSent() {
        var form = SendFeedbackForm(mailAvailable: true, running: true)
        form.problem = "x"
        form.makingBugReport = true
        #expect(!form.fieldsEnabled && !form.canSend && !form.canMakeBugReport && !form.canTakeScreenshot
                && !form.canAddMedia)
        form.makingBugReport = false
        form.takingScreenshot = true
        #expect(form.fieldsEnabled && !form.canSend && !form.canMakeBugReport && !form.canTakeScreenshot
                && !form.canAddMedia)
        form.takingScreenshot = false
        form.importingMedia = true
        #expect(!form.canSend && !form.canAddMedia)
        form.importingMedia = false
        #expect(form.canSend && form.canMakeBugReport && form.canTakeScreenshot && form.canAddMedia)
    }

    @Test func withoutMailEveryControlIsOff() {
        var form = SendFeedbackForm(mailAvailable: false, running: true)
        form.problem = "x"
        #expect(!form.fieldsEnabled && !form.canSend && !form.canMakeBugReport && !form.canTakeScreenshot
                && !form.canAddMedia)
        form.mailAvailable = true
        #expect(form.fieldsEnabled && form.canSend && form.canMakeBugReport)
    }

    @Test func aBugReportAndAScreenshotNeedARunningVirtualMachine() {
        var form = SendFeedbackForm(mailAvailable: true, running: false)
        #expect(form.showsHowToReportBug)
        #expect(!form.canMakeBugReport && !form.canTakeScreenshot)
        #expect(form.canAddMedia, "the rest of the form works")
        form.problem = "an idea"
        #expect(form.canSend)
        form.running = true
        #expect(!form.showsHowToReportBug && form.canMakeBugReport && form.canTakeScreenshot)
    }

    @Test func oneBugReportAtMostIsMade() {
        var form = SendFeedbackForm(mailAvailable: true, running: true)
        form.attachments.add(attachment(.bugReport, bytes: 10))
        #expect(!form.canMakeBugReport)
        #expect(form.canTakeScreenshot)
        form.attachments.remove(form.attachments.items[0].id)
        #expect(form.canMakeBugReport)
    }

    @Test func closingAsksOnceSomethingIsTypedOrAttached() {
        var form = SendFeedbackForm(mailAvailable: true, running: false)
        #expect(!form.hasContent)
        form.problem = "   "
        form.details = "\n"
        #expect(!form.hasContent, "blanks are nothing")
        form.details = "x"
        #expect(form.hasContent)
        form.details = ""
        form.attachments.add(attachment(.photo, bytes: 1))
        #expect(form.hasContent)
    }

    // MARK: Staging

    private func scratch() throws -> URL {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-feedback-test-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }

    @Test func aFolderIsMadeWithItsFirstFileAndRemovedWithEverythingInIt() throws {
        let root = try scratch()
        defer { try? FileManager.default.removeItem(at: root) }
        let staging = SendFeedbackStaging(root: root.appendingPathComponent("Feedback"))
        let folder = staging.makeFolder()
        #expect(!folder.exists, "no file, no folder")
        let first = try folder.reserve("photo.heic")
        try Data([1]).write(to: first)
        let second = try folder.reserve("photo.heic")
        #expect(first != second && second.lastPathComponent == "photo-2.heic")
        try Data([2]).write(to: second)
        #expect(folder.exists)
        folder.remove(first)
        #expect(!FileManager.default.fileExists(atPath: first.path))
        folder.removeAll()
        #expect(!folder.exists)
        #expect(!FileManager.default.fileExists(atPath: second.path))
    }

    @Test func aFileIsMovedIntoTheFolderUnderASafeName() throws {
        let root = try scratch()
        defer { try? FileManager.default.removeItem(at: root) }
        let staging = SendFeedbackStaging(root: root.appendingPathComponent("Feedback"))
        let folder = staging.makeFolder()
        let incoming = root.appendingPathComponent("picked")
        try Data(repeating: 7, count: 5).write(to: incoming)
        let adopted = try folder.adopt(incoming, named: "../My Photo (1).jpg")
        #expect(adopted.deletingLastPathComponent().standardizedFileURL == folder.url.standardizedFileURL)
        #expect(adopted.lastPathComponent == "_My_Photo__1_.jpg")
        #expect(!FileManager.default.fileExists(atPath: incoming.path))
        #expect(try Data(contentsOf: adopted).count == 5)
        #expect(SendFeedbackFolder.safeName("...") == "file")
        #expect(SendFeedbackFolder.safeName(String(repeating: "a", count: 300)).count == 100)
    }

    @Test func aSweepRemovesWhatAnEndedProcessLeftBehind() throws {
        let root = try scratch()
        defer { try? FileManager.default.removeItem(at: root) }
        let staging = SendFeedbackStaging(root: root.appendingPathComponent("Feedback"))
        let left = staging.makeFolder()
        try Data([1]).write(to: try left.reserve("bugreport.zip"))
        try FileManager.default.createDirectory(at: staging.incoming, withIntermediateDirectories: true)
        try Data([2]).write(to: staging.incoming.appendingPathComponent("video.mov"))
        staging.sweep()
        #expect(!FileManager.default.fileExists(atPath: staging.root.path))
        staging.sweep()  // nothing left is not an error
    }
}
