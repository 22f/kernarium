// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import AndariumCore
import AndariumCoreTestSupport
import Foundation
import Testing
import UIKit
@testable import Andarium

/// The sheet the slow-start notice opens, hosted in the application: the tip it
/// shows in each state and the start log it attaches by itself, once, from what
/// the application already has (the runtime's session record of this boot).
/// Like `SendFeedbackScreenTests` they run where Send feedback is compiled in: a
/// build made with `FEEDBACK_ADDRESS=feedback@example.com` on the xcodebuild
/// command line.
@Suite(.serialized)
@MainActor
struct SendFeedbackStartLogTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 41_000_000_000
    )

    /// A key as the Android adb keeps it, and a serial: values that must never
    /// reach the log.
    private static let key = "QAAAAGJ4Z0n2Hq+Lx0aT/m8nR4sJ0d5bYwV9cE7uKf3PzQ1yM6hS2oXkUa8tNvB5rLq=="
    private static let serial = "K7QX2M9T4R"

    private struct Fixture {
        let app: AppModel
        let diagnostics: URL
        let staging: SendFeedbackStaging
        let composer: Composer

        var record: URL { diagnostics.appendingPathComponent(BugReportAttachments.sessionRecord) }

        func sheet() -> SendFeedbackModel { SendFeedbackModel(app: app, staging: staging) }

        /// The session record a start under way leaves, with `output` as what the
        /// guest printed.
        func writeRecord(output: String) throws {
            let text = "uml-session 6\nstate running\nend running\nstatus 0\npid 4242\nstarted 1794114290\n"
                + "boot unfinished\nrestarts 0\nkernel-output\n" + output
            try Data(text.utf8).write(to: record)
        }
    }

    @MainActor private final class Composer {
        var request: SendFeedbackMailRequest?
    }

    private func fixture(phase: Phase = .booting(.normal)) throws -> Fixture {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KB pages", appVersion: "1.0 (9)"
        )
        let app = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        app.driver = StubDriver(plan: .normal)
        app.phase = phase
        // A clock that stands still: the start has run 3:17.
        app.clock = { 5000 }
        if case .booting = phase {
            app.slowStart.begin(at: 5000 - 197)
            app.updateSlowStart()
        }
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-startlog-hosted-\(UUID().uuidString)", isDirectory: true)
        let diagnostics = root.appendingPathComponent("diagnostics", isDirectory: true)
        try FileManager.default.createDirectory(at: diagnostics, withIntermediateDirectories: true)
        app.diagnosticsDirectory = diagnostics
        let composer = Composer()
        var services = SendFeedbackServices()
        services.address = { "feedback@example.com" }
        services.canSendMail = { true }
        services.presentMail = { request, _ in
            composer.request = request
            return true
        }
        services.runningSeconds = { _ in 125 }
        app.feedbackServices = services
        return Fixture(
            app: app, diagnostics: diagnostics, staging: SendFeedbackStaging(root: root.appendingPathComponent("staging")),
            composer: composer)
    }

    private func contents(of attachment: SendFeedbackAttachment) throws -> String {
        try String(contentsOf: attachment.url, encoding: .utf8)
    }

    private static let output = """
        [   41.2] init: starting service 'servicemanager'...
        [  102.9] init: starting service 'netd'...
        [  184.7] init: starting service 'zygote'...

        """

    // MARK: The tip

    @Test func theTipFollowsTheStateTheSheetIsOpenedIn() throws {
        let home = try fixture(phase: .home).sheet()
        #expect(home.state.tip == .home && home.state.showsHowToReportBug)
        let running = try fixture(phase: .running(.normal)).sheet()
        #expect(running.state.tip == .running)
        let starting = try fixture().sheet()
        #expect(starting.state.tip == .starting)
        // A bug report and a screenshot stay unavailable during a start, and the rest works.
        #expect(!starting.state.canMakeBugReport && !starting.state.canTakeScreenshot)
        #expect(starting.state.canAddMedia)
        starting.close()
        home.close()
        running.close()
    }

    // MARK: The log

    @Test func theSheetOpenedDuringAStartAttachesTheLogOnceFromTheSeededBoot() throws {
        let fixture = try fixture()
        try fixture.writeRecord(output: Self.output)
        let sheet = fixture.sheet()
        #expect(sheet.form.attachments.isEmpty, "making the model attaches nothing; the sheet's task does")
        sheet.offerStartLog()
        sheet.offerStartLog()
        #expect(sheet.form.attachments.count == 1)
        let log = try #require(sheet.form.attachments.items.first)
        #expect(log.kind == .startLog && log.url.lastPathComponent == "start-log.txt" && log.mimeType == "text/plain")
        let text = try contents(of: log)
        #expect(UInt64(text.utf8.count) == log.bytes)
        let lines = text.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        #expect(lines.starts(with: ["Start log", "Step: \(sheet.information.boot?.step ?? "?")", "Time: 3:17", "Start: ordinary start"]))
        #expect(lines.contains("[  102.9] init: starting service 'netd'..."))
        // It counts with the rest.
        #expect(sheet.form.attachments.totalBytes == log.bytes)
        // And it goes with the mail.
        sheet.form.problem = "Too slow"
        sheet.send()
        let request = try #require(fixture.composer.request)
        #expect(request.attachments.map(\.url.lastPathComponent) == ["start-log.txt"])
        #expect(request.body.contains("Boot: not finished"))
        sheet.close()
    }

    @Test func theLogTakesOutTheKeyTheSerialAndThePaths() throws {
        let fixture = try fixture()
        let container = fixture.diagnostics.path
        try fixture.writeRecord(
            output: """
                [    0.0] Kernel command line: androidboot.serialno=\(Self.serial) androidboot.kernarium.adb_key=\(Self.key) --session-record=\(container)/session.txt
                [    5.1] eth0: address aa:bb:cc:dd:ee:ff
                [   99.0] init: starting service 'netd'...

                """)
        fixture.app.diagnosticsContainers = [container]
        let sheet = fixture.sheet()
        sheet.offerStartLog()
        let text = try contents(of: try #require(sheet.form.attachments.items.first))
        #expect(!text.contains(Self.key) && !text.contains("QAAAAGJ4") && !text.contains(Self.serial))
        #expect(!text.contains("aa:bb:cc") && !text.contains(container))
        #expect(text.contains("adb_key=<masked>") && text.contains("serialno=<masked>"))
        #expect(text.contains("[   99.0] init: starting service 'netd'..."))
        sheet.close()
    }

    @Test func aRecordWithoutOutputGivesALogThatSaysSo() throws {
        let fixture = try fixture()
        let sheet = fixture.sheet()
        sheet.offerStartLog()
        let text = try contents(of: try #require(sheet.form.attachments.items.first))
        #expect(text.hasSuffix("(nothing yet)\n") && text.contains("Step: "))
        sheet.close()
    }

    @Test func theLogIsAtMostTheRecordsFourKiBAndNeverTheLastBootsOutput() throws {
        let fixture = try fixture()
        // The record's cursor starts with the boot: a record of this boot holds
        // this boot's output only, and the log takes the last 4 KiB of it.
        let lines = (0..<400).map { "[\(String(format: "%7.1f", Double($0)))] message \($0) of this boot" }
        try fixture.writeRecord(output: lines.joined(separator: "\n") + "\n")
        let sheet = fixture.sheet()
        sheet.offerStartLog()
        let text = try contents(of: try #require(sheet.form.attachments.items.first))
        let printed = try #require(text.components(separatedBy: "at most):\n").last)
        #expect(printed.utf8.count <= SendFeedbackStartLog.outputLimit + 1)
        #expect(printed.hasSuffix("message 399 of this boot\n") && !printed.contains("message 0 of"))
        sheet.close()
    }

    @Test func removingTheLogIsRememberedAndItIsNotMadeAgain() throws {
        let fixture = try fixture()
        try fixture.writeRecord(output: Self.output)
        let sheet = fixture.sheet()
        sheet.offerStartLog()
        let log = try #require(sheet.form.attachments.items.first)
        sheet.remove(log)
        #expect(sheet.form.attachments.isEmpty && !FileManager.default.fileExists(atPath: log.url.path))
        // The sheet's task runs again (the view is rebuilt), and the start moves on.
        sheet.offerStartLog()
        fixture.app.clock = { 5060 }
        sheet.offerStartLog()
        #expect(sheet.form.attachments.isEmpty, "a removed log is not attached again")
        sheet.close()
    }

    @Test func theLogIsNotRefreshedWhileTheSheetIsOpen() throws {
        let fixture = try fixture()
        try fixture.writeRecord(output: Self.output)
        let sheet = fixture.sheet()
        sheet.offerStartLog()
        let log = try #require(sheet.form.attachments.items.first)
        let before = try contents(of: log)
        try fixture.writeRecord(output: Self.output + "[  190.0] init: a later line\n")
        fixture.app.clock = { 5063 }
        sheet.offerStartLog()
        #expect(try contents(of: log) == before && !before.contains("a later line"))
        #expect(sheet.form.attachments.count == 1)
        sheet.close()
    }

    @Test func theLogIsOnlyMadeDuringAStart() throws {
        for phase in [Phase.home, .running(.normal)] {
            let fixture = try fixture(phase: phase)
            try fixture.writeRecord(output: Self.output)
            let sheet = fixture.sheet()
            sheet.offerStartLog()
            #expect(sheet.form.attachments.isEmpty, "\(phase) has no start to log")
            sheet.close()
        }
    }

    @Test func aSheetThatHoldsOnlyTheLogClosesWithoutAskingButADraftDoesAsk() throws {
        let fixture = try fixture()
        let sheet = fixture.sheet()
        sheet.offerStartLog()
        #expect(sheet.form.attachments.count == 1 && !sheet.state.hasContent)
        sheet.form.problem = "Too slow"
        #expect(sheet.state.hasContent)
        sheet.close()
    }

    // MARK: The start ends

    @Test func whenTheStartFinishesTheTipIsTheRunningOneAndTheLogStays() throws {
        let fixture = try fixture()
        try fixture.writeRecord(output: Self.output)
        let sheet = fixture.sheet()
        sheet.offerStartLog()
        #expect(sheet.state.tip == .starting && !sheet.state.canTakeScreenshot)
        fixture.app.finishBoot(.normal)
        #expect(sheet.state.tip == .running && !sheet.state.showsHowToReportBug)
        #expect(sheet.state.canTakeScreenshot, "the screenshot is available once the virtual machine runs")
        #expect(sheet.form.attachments.items.map(\.kind) == [.startLog])
        // The sheet's task does not run again for a running session, and if it did, no second log is made.
        sheet.offerStartLog()
        #expect(sheet.form.attachments.count == 1)
        sheet.close()
    }
}

#endif
