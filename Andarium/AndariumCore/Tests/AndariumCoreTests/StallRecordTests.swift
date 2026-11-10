// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The runtime's stall slot as Andarium reads it. The slot text is the golden
/// fixture the runtime's own test checks its writer against
/// (`tests/unit/andarium/fixtures/stall-slot-golden.txt`), so the two sides
/// agree on the format from one file.
struct StallRecordTests {
    static func golden() throws -> String {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { url.deleteLastPathComponent() }
        url.append(path: "tests/unit/andarium/fixtures/stall-slot-golden.txt")
        return try String(contentsOf: url, encoding: .utf8)
    }

    /// A whole format 4 record: the memory slot, the stall slot, the state.
    static func record(stalls slot: String, state: String = "state running\nend running\nstatus 0\npid 77\n") -> String {
        var memory = "uml-session 4\nmemory at 1794114290 footprint 2147483648 peak 2415919104 available 900000000\n"
        memory += String(repeating: " ", count: 4095 - memory.utf8.count) + "\n"
        return memory + slot + state
    }

    @Test func theGoldenSlotParses() throws {
        let slot = try Self.golden()
        #expect(slot.utf8.count < 4096 && slot.hasSuffix("\n"))
        let record = try #require(StallRecord(text: slot))
        #expect(record.base == Date(timeIntervalSince1970: 1_794_114_300))
        #expect(record.written == Date(timeIntervalSince1970: 1_794_118_430))
        #expect(record.total == 37 && record.shown == 32 && record.events.count == 32)
        #expect(record.thermal == [
            .init(at: 1200, from: 0, to: 1), .init(at: 3_990_000, from: 1, to: 2),
        ])
        #expect(record.power == [
            .init(at: 50_000, lowPowerMode: true, processors: 6),
            .init(at: 4_121_000, lowPowerMode: true, processors: 4),
        ])
        #expect(record.pressure == [.init(at: 2_000_000, level: 1), .init(at: 4_100_000, level: 0)])

        let first = record.events[0]
        #expect(first.sequence == 6 && first.start == 6000 && first.duration == 106 && !first.open)
        #expect(first.stalled == [.init(vcpu: 0, cause: .starved, sharePercent: 4)])
        #expect(first.mainQueueLate == nil && first.watchLate == 0)

        // A starved event on six vCPUs, with the context of a hot device.
        let starved = record.events[30]
        #expect(starved.sequence == 36 && starved.start == 3_988_180 && starved.duration == 962 && !starved.open)
        #expect(starved.stalled.map(\.vcpu) == [0, 1, 2, 3, 4, 5])
        #expect(starved.stalled.allSatisfy { $0.cause == .starved })
        #expect(starved.stalled.map(\.sharePercent) == [3, 5, 0, 0, 2, 1])
        #expect(starved.mainQueueLate == 940 && starved.watchLate == 955)
        #expect(starved.thermalStart == 1 && starved.thermalEnd == 2 && !starved.lowPowerMode)
        #expect(starved.processorsStart == 6 && starved.processorsEnd == 6)
        #expect(starved.before == .init(cores: 587, performanceCores: 198, watts: 344))
        #expect(starved.during == .init(cores: 212, performanceCores: 198, watts: 255))
        #expect(starved.involuntarySwitches == 12 && starved.footprintMiB == 5752 && starved.leftMiB == 380)
        #expect(starved.compressedChangeMiB == 12 && starved.pageins == 41 && starved.faults == 18000)
        #expect(starved.systemPageins == 90 && starved.systemDecompressions == 3300 && starved.pressure == 1)

        // An event still going, blocked on two different waits.
        let blocked = record.events[31]
        #expect(blocked.open && blocked.duration == 5230 && blocked.leftMiB == nil)
        #expect(blocked.stalled == [
            .init(vcpu: 2, cause: .diskWrite, sharePercent: 0), .init(vcpu: 4, cause: .answer, sharePercent: 0),
        ])
        #expect(blocked.processorsStart == 6 && blocked.processorsEnd == 4 && blocked.compressedChangeMiB == -20)
    }

    @Test func aFormat4RecordHasTheStallsAndTheState() throws {
        // The runtime pads the slot with spaces to 4 KiB, the last byte a line feed.
        var slot = try Self.golden()
        slot += String(repeating: " ", count: 4095 - slot.utf8.count) + "\n"
        let record = try #require(SessionRecord(text: Self.record(stalls: slot)))
        #expect(record.running && record.end == "running" && record.processID == 77)
        #expect(record.memory?.footprintBytes == 2 << 30 && record.memory?.availableBytes == 900_000_000)
        #expect(record.stalls?.total == 37 && record.stalls?.events.count == 32)
    }

    @Test func theEarlierFormatsStillParse() throws {
        // Layouts 1 to 3 have the state right after the memory slot.
        for format in 1...3 {
            var slot = "uml-session \(format)\nmemory at 1794114290 footprint 2147483648 peak 2415919104\n"
            slot += String(repeating: " ", count: 255 - slot.utf8.count) + "\n"
            let text = slot + "state ended\nend guest-panic\nstatus 6\npid 4\nstarted 1794114290\nended 1794114295\n"
                + "message sysrq triggered crash\nkernel-output\n[ 0.5] Kernel panic\n"
            let record = try #require(SessionRecord(text: text), "layout \(format)")
            #expect(!record.running && record.end == "guest-panic" && record.status == 6 && record.processID == 4)
            #expect(record.message == "sysrq triggered crash" && record.kernelOutput == "[ 0.5] Kernel panic\n")
            #expect(record.memory?.footprintBytes == 2 << 30)
            #expect(record.stalls == nil)
        }
        // Layout 4 without a stall section (a slot never written) has none.
        let blank = String(repeating: " ", count: 4095) + "\n"
        let record = try #require(SessionRecord(text: Self.record(stalls: blank)))
        #expect(record.stalls == nil && record.processID == 77)
        // A layout of the future is no record.
        #expect(SessionRecord(text: "uml-session 5\nstate ended\nend powered-off\n") == nil)
    }

    @Test func aLaterRuntimeMayAddFieldsAndLines() throws {
        var lines = try Self.golden().split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        // A field added at the end of an event line, a line of a kind this reader
        // does not know, a line cut short and a line of nonsense.
        lines[7] += " 12345 extra"
        lines.insert("futurelines 1 2 3", at: 3)
        lines.insert("stall 9", at: 3)
        lines.insert("stall x y z a b c d e f g h i j k l m n o p q r s", at: 3)
        let record = try #require(StallRecord(text: lines.joined(separator: "\n")))
        #expect(record.events.count == 32)
        #expect(record.events.first?.sequence == 6)
        // A version this reader does not know is no stall record.
        let newer = try Self.golden().replacingOccurrences(of: "stalls 1 ", with: "stalls 2 ")
        #expect(StallRecord(text: newer) == nil)
    }

    @Test func theWordsSayWhichVcpusWhatTheyWaitedForAndHowLong() throws {
        let record = try #require(StallRecord(text: try Self.golden()))
        let blocked = record.words(for: record.events[31]).joined(separator: "\n")
        #expect(blocked.contains("Stall 37, 2026-11-08T06:13:40Z (1 h 8 min into the session): 5.2 s, still going"))
        #expect(blocked.contains("vCPU 2: waiting for a disk write."))
        #expect(blocked.contains("vCPU 4: waiting for a device service's answer."))
        #expect(blocked.contains("thermal state serious"))
        #expect(blocked.contains("Low Power Mode on, 6 active cores to 4"))
        #expect(blocked.contains("the process held 5801 MiB, memory pressure normal, compressed memory -20 MiB"))
        #expect(blocked.contains("waited on the host"))

        let starved = record.words(for: record.events[30]).joined(separator: "\n")
        #expect(starved.contains("vCPU 0, vCPU 1, vCPU 2, vCPU 3, vCPU 4 and vCPU 5: runnable but starved"))
        #expect(starved.contains("The watch's own tick ran 955 ms late, so the whole process was held up"))
        #expect(starved.contains("The app's main queue answered 940 ms late."))
        #expect(starved.contains("the system did not run the vCPU threads while the device was hot"))
        #expect(starved.contains("5.87 cores (1.98 on performance cores, 3.44 W)"))

        let all = record.report()
        #expect(all.contains("37 stalls recorded; the newest 32 are listed."))
        #expect(all.contains("thermal state nominal to fair") && all.contains("memory pressure warning"))
        #expect(all.contains("Low Power Mode on, 4 active cores"))
    }

    @Test func aStallOfTheWholeProcessNamesNoVcpu() throws {
        let line = "stall 1 9000 640 . - - 640 00 0 6>6 0,0,0 0,0,0 3 400 - 0 0 0 0 0 0"
        let text = "stalls 1 base 1794114300 at 1794114320 total 1 shown 1\n" + line + "\n"
        let record = try #require(StallRecord(text: text))
        #expect(record.events[0].stalled.isEmpty && record.events[0].watchLate == 640)
        let words = record.words(for: record.events[0]).joined(separator: "\n")
        #expect(words.contains("the whole process was not running"))
        #expect(words.contains("the whole process stopped"))
    }

    /// The most an event can say: every vCPU stalled for a different cause, the
    /// widest numbers in every field.
    private func worstEvent(_ sequence: Int) -> StallRecord.Event {
        let causes: [StallRecord.Cause] = [
            .starved, .fault, .other, .answer, .receive, .send, .diskRead, .diskWrite, .flush, .memory,
        ]
        return StallRecord.Event(
            sequence: sequence, start: 999_999_999, duration: 999_999, open: true,
            stalled: (0..<16).map { .init(vcpu: $0, cause: causes[$0 % causes.count], sharePercent: 99) },
            mainQueueLate: 99_999, watchLate: 99_999, thermalStart: 3, thermalEnd: 3, lowPowerMode: true,
            processorsStart: 99, processorsEnd: 99,
            before: .init(cores: 9999, performanceCores: 9999, watts: 9999),
            during: .init(cores: 9999, performanceCores: 9999, watts: 9999),
            involuntarySwitches: 9_999_999, footprintMiB: 99_999, leftMiB: 99_999, compressedChangeMiB: -99_999,
            pageins: 9_999_999, faults: 99_999_999, systemPageins: 99_999_999, systemDecompressions: 99_999_999,
            pressure: 2)
    }

    @Test func theReportCannotGrowPastItsBound() {
        // The slot holds at most 32 events and 3 x 8 changes, whatever they say.
        let record = StallRecord(
            base: Date(timeIntervalSince1970: 1_794_114_300), written: Date(timeIntervalSince1970: 1_794_118_430),
            total: 4_000_000_000, shown: 32, events: (0..<32).map { worstEvent(999_999 - $0) },
            thermal: (0..<8).map { .init(at: 999_999_999, from: 3, to: 2 + $0 % 2) },
            power: (0..<8).map { _ in .init(at: 999_999_999, lowPowerMode: true, processors: 99) },
            pressure: (0..<8).map { _ in .init(at: 999_999_999, level: 2) })
        let bytes = record.report().utf8.count
        #expect(bytes <= StallRecord.maximumReportBytes, "\(bytes)")
        let two = BugReportAttachments.stalls(
            previous: SessionRecord(text: Self.record(stalls: "")), current: nil)
        #expect(two == nil)
    }
}

extension StallRecord {
    fileprivate init(
        base: Date, written: Date, total: Int, shown: Int, events: [Event], thermal: [ThermalChange],
        power: [PowerChange], pressure: [PressureChange]
    ) {
        let text = "stalls 1 base \(Int(base.timeIntervalSince1970)) at \(Int(written.timeIntervalSince1970)) "
            + "total 0 shown 0\n"
        self = StallRecord(text: text)!
        self.total = total
        self.shown = shown
        self.events = events
        self.thermal = thermal
        self.power = power
        self.pressure = pressure
    }
}
