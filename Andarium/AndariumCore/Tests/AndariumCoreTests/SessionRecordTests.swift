// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The runtime's record of how the previous session ended, read as the
/// application reads it at launch, the notice Home shows for it, and the
/// redaction its text goes through before it leaves the device.
struct SessionRecordTests {
    /// The record as the runtime writes it: a memory slot padded to 256
    /// bytes, then the session's state.
    private func record(_ state: String, memory: String = "memory at 1794114290 footprint 2147483648 peak 2415919104")
        -> String
    {
        var slot = "uml-session 2\n\(memory)\n"
        slot += String(repeating: " ", count: 255 - slot.utf8.count) + "\n"
        return slot + state
    }

    @Test func readsAPanicWithItsKernelOutput() throws {
        let text = record(
            """
            state ended
            end guest-panic
            status 6
            pid 46851
            started 1794114290
            ended 1794114295
            message sysrq triggered crash
            kernel-output
            [    0.566837] Kernel panic - not syncing: sysrq triggered crash
            Darwin UML panic: sysrq triggered crash

            """)
        let record = try #require(SessionRecord(text: text))
        #expect(!record.running && !record.orderly)
        #expect(record.end == "guest-panic" && record.status == 6 && record.processID == 46851)
        #expect(record.started == Date(timeIntervalSince1970: 1_794_114_290))
        #expect(record.ended == Date(timeIntervalSince1970: 1_794_114_295))
        #expect(record.message == "sysrq triggered crash")
        #expect(record.kernelOutput?.hasPrefix("[    0.566837] Kernel panic") == true)
        #expect(record.memory?.footprintBytes == 2 << 30 && record.memory?.availableBytes == nil)
        #expect(record.summary == "The previous session ended in a guest kernel panic: sysrq triggered crash.")
        #expect(record.notice(endedOutOfForeground: false) == SessionEndNotice(
            title: "Android's kernel panicked", text: "Android stopped with a kernel panic: sysrq triggered crash."))
    }

    @Test func aRecordStillRunningNearTheLimitWasEndedByIOS() throws {
        let near = try #require(SessionRecord(text: record(
            "state running\nend running\nstatus 0\npid 7\nstarted 1794114339\n",
            memory: "memory at 1794114400 footprint 5905580032 peak 5905580032 available 104857600")))
        #expect(near.running && !near.orderly && near.ended == nil && near.kernelOutput == nil)
        #expect(near.nearMemoryLimit)
        #expect(near.memory?.availableBytes == 100 << 20)
        #expect(near.summary.hasSuffix("it used 5632 MiB, 100 MiB left."))
        #expect(near.notice(endedOutOfForeground: false)?.title == "iOS ended Andarium")
        // Out of the foreground it is the pause marker's to report.
        #expect(near.notice(endedOutOfForeground: true) == nil)

        let roomy = try #require(SessionRecord(text: record(
            "state running\nend running\nstatus 0\npid 7\nstarted 1794114339\n",
            memory: "memory at 1794114400 footprint 2147483648 peak 2147483648 available 2147483648")))
        #expect(!roomy.nearMemoryLimit)
        #expect(roomy.notice(endedOutOfForeground: false)?.title == "Andarium stopped unexpectedly")
    }

    @Test func eachBadEndHasItsNotice() throws {
        let titles = [
            "kernel-abort": "Android's kernel crashed",
            "kernel-exit": "Android stopped",
            "hibernation-restore-failed": "Android could not resume",
            "host-fault": "Andarium crashed",
            "process-exit": "Andarium crashed",
        ]
        for (end, title) in titles {
            let ended = try #require(SessionRecord(text: record("state ended\nend \(end)\nstatus 11\npid 9\n")))
            #expect(ended.notice(endedOutOfForeground: false)?.title == title, "\(end)")
        }
    }

    @Test func anOrderlyEndHasNoNotice() throws {
        for end in ["powered-off", "restarted", "stopped"] {
            let ended = try #require(SessionRecord(text: record("state ended\nend \(end)\nstatus 0\npid 9\n")))
            #expect(ended.orderly && ended.message == nil)
            #expect(ended.notice(endedOutOfForeground: false) == nil)
        }
    }

    @Test func readsTheFirstFormatToo() throws {
        let first = try #require(SessionRecord(
            text: "uml-session 1\nstate ended\nend powered-off\nstatus 0\npid 9\nstarted 1\nended 2\n"))
        #expect(first.orderly && first.memory == nil)
    }

    @Test func readsTheTermsAndTheirHistory() throws {
        var slot = """
            uml-session 3
            memory at 1794210239 footprint 6441524416 peak 6442458280 available 926528
            terms guest 4090 gpu 1262/1251 age 0 graphics 905 media 0 cache 238 heaps 260 malloc 812/700 age 4 other 650 internal 5200 compressed 0
            peak-terms at 1794210239 footprint 6143 guest 4090 gpu 1262/1251 age 0 graphics 905 media 0 cache 238 heaps 260 malloc 812/700 age 4 other 650 internal 5200 compressed 0
            history seconds footprint available guest gpu graphics cache heaps malloc other
            h 484 6100 43 4090 1250 900 238 258 790 614
            h 486 6143 0 4090 1262 905 238 260 812 650

            """
        slot += String(repeating: " ", count: 4095 - slot.utf8.count) + "\n"
        let near = try #require(SessionRecord(
            text: slot + "state running\nend running\nstatus 0\npid 20373\nstarted 1794209753\n"))
        #expect(near.running && near.nearMemoryLimit)
        #expect(near.memory?.footprintBytes == 6_441_524_416 && near.memory?.availableBytes == 926_528)
        #expect(near.terms?.hasPrefix("guest 4090 gpu 1262/1251") == true)
        #expect(near.peakTerms?.hasPrefix("at 1794210239 footprint 6143 guest 4090") == true)
        #expect(near.notice(endedOutOfForeground: false)?.title == "iOS ended Andarium")
    }

    /// A layout 5 record, whose history rows have ten columns in a 4 KiB
    /// memory slot, and a layout 6 record, whose rows add the figures of the
    /// run in an 8 KiB one, carry the same slots, and the reader takes the
    /// same fields from both. A layout 6 record of the largest size the
    /// runtime writes is read whole.
    @Test func aLayout5ALayout6AndALayout7RecordReadAlike() throws {
        let columns = "h 484 6100 43 4090 1250 900 238 258 790 614"
        let figures = " 2000 120 45 2 1 12 5000 1 2 3 158 5900 5000 74 3000 11800"
        let names = " span frames gap thermal pressure compressed internal reliefs shed renewals largest"
            + " cpu vcpu perf mhz energy"
        for layout in [5, 6, 7] {
            var memory = """
                uml-session \(layout)
                memory at 1794210239 footprint 6441524416 peak 6442458280 available 926528
                terms guest 4090 gpu 1262/1251 age 0 graphics 905 media 0 cache 238 heaps 260 malloc 812/700 age 4 other 650 internal 5200 compressed 0
                peak-terms at 1794210239 footprint 6143 guest 4090 gpu 1262/1251 age 0 graphics 905 media 0 cache 238 heaps 260 malloc 812/700 age 4 other 650 internal 5200 compressed 0
                history seconds footprint available guest gpu graphics cache heaps malloc other\(layout >= 6 ? names : "")
                \(columns)\(layout >= 6 ? figures : "")

                """
            memory = NetworkRecordTests.padded(String(memory.dropLast()), to: layout >= 6 ? 8192 : 4096)
            // Layout 7 has the storage slot, blank until the first settle,
            // between the network slot and the state.
            let text = memory + NetworkRecordTests.padded(try StallRecordTests.golden(), to: 4096)
                + NetworkRecordTests.padded(try NetworkRecordTests.golden(), to: 512)
                + (layout == 7 ? NetworkRecordTests.padded("", to: 256) : "")
                + "state running\nend running\nstatus 0\npid 20373\nstarted 1794209753\n"
            let record = try #require(SessionRecord(text: text), "layout \(layout)")
            #expect(record.running && record.nearMemoryLimit && record.processID == 20373, "layout \(layout)")
            #expect(record.memory?.footprintBytes == 6_441_524_416 && record.memory?.availableBytes == 926_528)
            #expect(record.terms?.hasPrefix("guest 4090 gpu 1262/1251") == true)
            #expect(record.peakTerms?.hasPrefix("at 1794210239 footprint 6143 guest 4090") == true)
            #expect(record.stalls?.total == 37 && record.network?.tcp?.attempts == 12, "layout \(layout)")
            // An earlier layout has no storage slot; a blank one makes no claim.
            #expect(record.storage == nil, "layout \(layout)")
        }
        #expect(SessionRecord.maximumBytes >= 18176)
        var largest = "uml-session 7\nmemory at 1 footprint 2 peak 3\n"
        largest = NetworkRecordTests.padded(String(largest.dropLast()), to: 8192)
        largest += NetworkRecordTests.padded("stalls 1 base 1 at 1 total 0 shown 0", to: 4096)
        largest += NetworkRecordTests.padded("network 1\nnet-notice denials 0 now none", to: 512)
        largest += NetworkRecordTests.padded("", to: 256)
        largest += "state running\nend running\nstatus 0\npid 3\n"
        largest += String(repeating: "k", count: 18176 - largest.utf8.count)
        #expect(largest.utf8.count == 18176 && SessionRecord(text: largest)?.running == true)
    }

    /// The runtime's storage slot as Andarium reads it: the golden fixture the
    /// runtime's own test checks its writer against
    /// (`tests/unit/andarium/fixtures/storage-slot-golden.txt`).
    static func storageGolden() throws -> String {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { url.deleteLastPathComponent() }
        url.append(path: "tests/unit/andarium/fixtures/storage-slot-golden.txt")
        return try String(contentsOf: url, encoding: .utf8)
    }

    /// A whole layout 7 record with `storage` in the storage slot.
    static func layout7(storage: String) throws -> String {
        var memory = "uml-session 7\nmemory at 1794114290 footprint 2147483648 peak 2415919104 available 900000000\n"
        memory = NetworkRecordTests.padded(String(memory.dropLast()), to: 8192)
        return memory + NetworkRecordTests.padded(try StallRecordTests.golden(), to: 4096)
            + NetworkRecordTests.padded(try NetworkRecordTests.golden(), to: 512)
            + NetworkRecordTests.padded(storage, to: 256)
            + "state running\nend running\nstatus 0\npid 77\n"
    }

    @Test func theGoldenStorageSlotParses() throws {
        let slot = try #require(StorageRecord(text: try Self.storageGolden()))
        #expect(slot.settles == 4 && slot.unsettled == 2)
        let last = try #require(slot.last)
        #expect(last.call == "quiesce" && last.storage == "confirmed" && last.suspension == "suspended")
        #expect(last.at == Date(timeIntervalSince1970: 1_794_114_290) && last.settled)
    }

    @Test func aLayout7RecordCarriesTheStorageSlot() throws {
        let session = try #require(
            SessionRecord(text: try Self.layout7(storage: try Self.storageGolden().trimmingCharacters(in: .newlines))))
        #expect(session.processID == 77 && session.running)
        #expect(session.storage?.settles == 4 && session.storage?.unsettled == 2)
        // The other slots are read as they are in layout 6.
        #expect(session.stalls?.total == 37 && session.network?.tcp?.attempts == 12)
        #expect(session.memory?.footprintBytes == 2 << 30)
    }

    @Test func aBlankStorageSlotMakesNoClaim() throws {
        // A session that never settled the guest says nothing of it: neither
        // that its storage was settled nor that it was not.
        let session = try #require(SessionRecord(text: try Self.layout7(storage: "")))
        #expect(session.storage == nil && session.running && session.processID == 77)
    }

    @Test func aLayout6RecordHasNoStorageSlotEvenWithAStorageLookingLine() throws {
        let six = "uml-session 6\nmemory at 1 footprint 2 peak 3\nstate ended\nend powered-off\n"
            + "storage 1\nsettles 3 unsettled 3\nlast sync unreachable not-asked at 5\n"
        let session = try #require(SessionRecord(text: six))
        #expect(session.storage == nil && session.end == "powered-off")
    }

    @Test func anUnsettledLastOutcomeIsNotSettled() throws {
        let text = "storage 1\nsettles 1 unsettled 1\nlast sync unreachable not-asked at 1794114290\n"
        let slot = try #require(StorageRecord(text: text))
        #expect(slot.settles == 1 && slot.unsettled == 1 && slot.last?.settled == false)
        // A guest that was not running has nothing to settle.
        let none = try #require(
            StorageRecord(text: "storage 1\nsettles 1 unsettled 0\nlast sync no-guest not-asked at 1794114290\n"))
        #expect(none.last?.settled == true)
    }

    @Test func aStorageSlotOfAnotherVersionOrWithoutCountsIsNotRead() {
        #expect(StorageRecord(text: "") == nil)
        #expect(StorageRecord(text: "storage 2\nsettles 1 unsettled 0\n") == nil)
        #expect(StorageRecord(text: "settles 1 unsettled 0\n") == nil)
        #expect(StorageRecord(text: "storage 1\nsettles 1\n") == nil)
        // A last line this reader cannot read leaves the counts.
        let slot = StorageRecord(text: "storage 1\nsettles 2 unsettled 1\nlast sync\n")
        #expect(slot?.settles == 2 && slot?.last == nil)
        // A later runtime may add lines and fields.
        let later = StorageRecord(
            text: "storage 1\nsettles 2 unsettled 1\nlast quiesce refused not-suspended at 7 extra 1\nnew line\n")
        #expect(later?.last?.storage == "refused")
    }

    @Test func otherTextIsNoRecord() {
        #expect(SessionRecord(text: "") == nil)
        #expect(SessionRecord(text: "uml-session 8\nstate ended\nend powered-off\n") == nil)
        #expect(SessionRecord(text: "uml-session 2\nstatus 0\n") == nil)
        #expect(SessionRecord.read(at: URL(fileURLWithPath: "/nonexistent/session.txt")) == nil)
    }

    @Test func redactionTakesTheContainerPathsOut() {
        let container = "/private/var/mobile/Containers/Data/Application/0A1B2C3D-0000-4000-8000-0123456789AB"
        let text = """
            [    0.36] Not locking "\(container)/Library/Application Support/android/data.img" on the host
            opened /var/mobile/Containers/Data/Application/0A1B2C3D-0000-4000-8000-0123456789AB/tmp/x
            bundle /private/var/containers/Bundle/Application/11111111-2222-4333-8444-555555555555/Andarium.app
            mac /Users/NAME/Library/Containers/app/Data/x
            """
        let redacted = DiagnosticRedaction.redact(text, containers: [container])
        #expect(!redacted.contains("0A1B2C3D") && !redacted.contains("11111111") && !redacted.contains("/Users/NAME"))
        #expect(redacted.contains("Not locking \"<container>/Library/Application Support/android/data.img\""))
        #expect(redacted.contains("opened <container>/tmp/x"))
        #expect(redacted.contains("bundle <container>/Andarium.app"))
        #expect(redacted.contains("mac /Users/<user>/Library/Containers/app/Data/x"))
    }
}
