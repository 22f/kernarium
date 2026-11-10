// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// Every file the application keeps of a session, and everything a bug report
/// copies of them, has a size it cannot pass whatever the session did.
///
/// The runtime's record is at most 13312 bytes (`session_record_max_bytes`);
/// the application keeps two of them (the current and the previous), a bug
/// report copies each once, adds the stalls in words, the guest's counters and
/// at most 30 MetricKit reports of at most 1 MiB.
struct SessionRecordBoundsTests {
    private func scratch() throws -> URL {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-bounds-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }

    private func write(_ text: String, _ name: String, in directory: URL) throws {
        try text.write(to: directory.appendingPathComponent(name), atomically: true, encoding: .utf8)
    }

    @Test func aRecordLongerThanTheRuntimeWritesIsNeitherReadNorCopiedWhole() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        // A header that parses, then far more than 13312 bytes.
        let text = try StallRecordTests.record(stalls: StallRecordTests.golden())
            + String(repeating: "x", count: 4 << 20)
        try write(text, BugReportAttachments.sessionRecord, in: directory)
        #expect(SessionRecord.read(at: directory.appendingPathComponent(BugReportAttachments.sessionRecord)) == nil)
        let entries = BugReportAttachments.entries(from: directory, containers: [])
        let current = try #require(entries.first { $0.name == "andarium/session-current.txt" })
        #expect(current.data.count == SessionRecord.maximumBytes)
        // An oversized file is not consumed as the previous session's.
        #expect(PreviousSession.consume(in: directory, endedOutOfForeground: false) == nil)
    }

    @Test func aMetricKitReportIsKeptAndCopiedWithinItsCap() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        let cap = BugReportAttachments.maximumMetricKitBytes
        MetricKitReports.keep(Data(repeating: 0x61, count: 3 * cap), named: "big.json", in: directory)
        let kept = directory.appendingPathComponent(BugReportAttachments.metricKitDirectory)
            .appendingPathComponent("big.json")
        #expect(try Data(contentsOf: kept).count == cap)
        // A file an earlier version kept whole is cut when it is copied.
        try Data(repeating: 0x62, count: 2 * cap).write(
            to: kept.deletingLastPathComponent().appendingPathComponent("old.json"))
        let entries = BugReportAttachments.entries(from: directory, containers: [])
        #expect(entries.filter { $0.name.hasPrefix("andarium/metrickit/") }.allSatisfy { $0.data.count == cap })
        // Only the newest reports are kept.
        for index in 0..<40 {
            MetricKitReports.keep(Data("{}".utf8), named: String(format: "r%02d.json", index), in: directory)
        }
        let names = try FileManager.default.contentsOfDirectory(
            atPath: kept.deletingLastPathComponent().path)
        #expect(names.count == 30)
    }

    @Test func theStallsAreAnEntryOfTheirOwnWhenARecordHasThem() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        let golden = try StallRecordTests.golden()
        var slot = golden
        slot += String(repeating: " ", count: 4095 - slot.utf8.count) + "\n"
        try write(
            StallRecordTests.record(stalls: slot, state: "state ended\nend powered-off\nstatus 0\npid 31\n"),
            BugReportAttachments.previousSessionRecord, in: directory)
        try write(
            StallRecordTests.record(stalls: String(repeating: " ", count: 4095) + "\n"),
            BugReportAttachments.sessionRecord, in: directory)
        let entries = BugReportAttachments.entries(from: directory, containers: [])
        #expect(entries.map(\.name) == [
            "andarium/session-previous.txt", "andarium/session-current.txt", "andarium/stalls.txt",
        ])
        let words = String(decoding: entries[2].data, as: UTF8.self)
        #expect(words.contains("== The previous session (process 31) =="))
        #expect(words.contains("vCPU 2: waiting for a disk write."))
        #expect(words.contains("== The current session (process 77) =="))
        #expect(words.contains("The record has no stall slot."))
        #expect(entries[2].data.count <= 2 * StallRecord.maximumReportBytes + 4096)

        // A runtime from before layout 4 has no stalls to report.
        try FileManager.default.removeItem(at: directory.appendingPathComponent(BugReportAttachments.previousSessionRecord))
        try write("uml-session 3\nstate running\nend running\n", BugReportAttachments.sessionRecord, in: directory)
        #expect(BugReportAttachments.entries(from: directory, containers: []).map(\.name)
            == ["andarium/session-current.txt"])
    }

    @Test func theGuestsCountersAreReadWithinTheirLimits() {
        let command = GuestProcFiles.command
        for source in GuestProcFiles.sources {
            #expect(command.contains("r \(source.path) \(source.limit)"))
        }
        #expect(GuestProcFiles.sources.map(\.path).contains("/proc/timer_list"))
        #expect(GuestProcFiles.sources.map(\.path).contains("/proc/pressure/memory"))
        #expect(GuestProcFiles.entry(from: "") == nil)
        let output = String(repeating: "interrupts\n", count: 1 << 20)
        let entry = GuestProcFiles.entry(from: output)
        #expect(entry?.name == "andarium/guest-proc.txt")
        #expect(entry?.data.count == GuestProcFiles.maximumBytes)
        #expect(GuestProcFiles.maximumBytes < 256 << 10)

        let directory = FileManager.default.temporaryDirectory
        let attached = BugReportAttachments.entries(
            from: directory.appendingPathComponent("nothing-\(UUID().uuidString)"), containers: [],
            guest: entry.map { [$0] } ?? [])
        #expect(attached.map(\.name) == ["andarium/guest-proc.txt"])
    }

    /// The most a bug report gains from Andarium, which every part above bounds.
    @Test func theWholeOfWhatABugReportGainsIsBounded() {
        let records = 2 * SessionRecord.maximumBytes
        let stalls = 2 * StallRecord.maximumReportBytes + 4096
        let metrics = 30 * BugReportAttachments.maximumMetricKitBytes
        let total = records + stalls + GuestProcFiles.maximumBytes + metrics
        #expect(total < 32 << 20)
    }
}
