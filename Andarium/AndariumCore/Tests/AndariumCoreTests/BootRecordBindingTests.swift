// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// Which session record is the boot under way's: the start log quotes the
/// record's kernel output only for it.
struct BootRecordBindingTests {
    /// 2026-11-08 05:04:50 UTC, and the process the application runs as.
    private static let begun = Date(timeIntervalSince1970: 1_794_114_290)
    private static let pid = 4242

    /// A record as the runtime writes it, with the lines the binding reads.
    private func record(
        state: String = "running", end: String = "running", pid: Int = pid, started: Int = 1_794_114_291,
        restarts: Int? = 0, output: String = "init: starting service 'netd'\n"
    ) -> SessionRecord? {
        var text = "uml-session 7\nstate \(state)\nend \(end)\nstatus 0\npid \(pid)\nstarted \(started)\n"
        if state != "running" { text += "ended \(started + 5)\n" }
        text += "boot unfinished\n"
        if let restarts { text += "restarts \(restarts)\n" }
        text += "kernel-output\n" + output
        return SessionRecord(text: text)
    }

    private let binding = BootRecordBinding(processID: pid, runStarted: begun)

    @Test func theRunningRecordOfTheBootUnderWayIsAccepted() {
        #expect(binding.output(of: record()) == "init: starting service 'netd'\n")
        // The run began within the second the record's `started` is in.
        #expect(binding.output(of: record(started: 1_794_114_290)) != nil)
        #expect(BootRecordBinding(processID: Self.pid, runStarted: Self.begun.addingTimeInterval(0.9))
            .output(of: record(started: 1_794_114_290)) != nil)
    }

    @Test func aRecordThatEndedIsNotTheBootUnderWays() {
        for end in ["stopped", "restarted", "powered-off", "guest-panic", "failed"] {
            #expect(binding.output(of: record(state: "ended", end: end)) == nil, "\(end)")
        }
    }

    @Test func aRunningRecordOfAnotherProcessIsNot() {
        #expect(binding.output(of: record(pid: Self.pid + 1)) == nil)
        #expect(binding.output(of: record(pid: 1)) == nil)
    }

    @Test func aRecordThatBeganBeforeTheRunIsNot() {
        // The run's own process, whose earlier run ended without writing its end; or a process id that came round.
        #expect(binding.output(of: record(started: 1_794_114_289)) == nil)
        #expect(binding.output(of: record(started: 1_700_000_000)) == nil)
    }

    @Test func aRunningRecordOfAnEarlierBootOfThisProcessIsNot() {
        var restarted = binding
        restarted.restarts = 2
        #expect(restarted.output(of: record(restarts: 1)) == nil)
        #expect(restarted.output(of: record(restarts: 0)) == nil)
        #expect(restarted.output(of: record(restarts: 2)) != nil)
        // A record of a later boot is not this one's either: the application has not yet been told of it.
        #expect(restarted.output(of: record(restarts: 3)) == nil)
    }

    @Test func betweenARestartAndTheNextRecordTheFileHoldsTheBootBeforeAndNothingIsQuoted() {
        var restarted = binding
        restarted.restarts = 1
        // The runtime ended the boot before (`restarted`), and has not begun the next one's record.
        #expect(restarted.output(of: record(state: "ended", end: "restarted", restarts: 0)) == nil)
        // Whatever the old record's end says, its count is the boot before's.
        #expect(restarted.output(of: record(restarts: 0)) == nil)
        // The next boot's record is its own.
        #expect(restarted.output(of: record(restarts: 1, output: "init: next boot\n")) == "init: next boot\n")
    }

    @Test func aRecordWithoutARestartCountOrOutputOrAtAllGivesNothing() {
        #expect(binding.output(of: record(restarts: nil)) == nil)
        #expect(binding.output(of: record(output: "")) == nil)
        #expect(binding.output(of: nil) == nil)
    }

    @Test func aRecordPaddedWithNewlinesGivesTheOutputWithoutThem() {
        #expect(binding.output(of: record(output: "init: a\ninit: b\n" + String(repeating: "\n", count: 4000)))
            == "init: a\ninit: b\n")
    }
}
