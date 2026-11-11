// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// The session record's network slot on a real guest. Android takes its lease
/// from the user network when it boots, and the runtime, which rewrites the
/// record every two seconds from the counts of the user network's provider,
/// must show it: a DHCP discover and its acknowledgement. Runs alone with
/// ANDARIUM_NETWORK_RECORD_TESTS=1 and ANDARIUM_GUEST_INSTANCE=network-record,
/// on a Mac, an iPad or the Simulator, with an isolated guest. The test
/// installs nothing and deletes nothing: erase the guest with an
/// ANDARIUM_ERASE=1 launch (README).
///
/// The test prints the slot, which holds numbers only, on a NETWORK_RECORD
/// line.
@MainActor
struct NetworkRecordGuestTests {
    @Test(.timeLimit(.minutes(25)))
    func aBootedGuestHasItsLeaseInTheSessionRecord() async throws {
        guard ProcessInfo.processInfo.environment["ANDARIUM_NETWORK_RECORD_TESTS"] == "1" else { return }
        try #require(ProcessInfo.processInfo.environment["ANDARIUM_GUEST_INSTANCE"] == "network-record")
        let record = AppEntry.guestDirectory(instance: "network-record")
            .appendingPathComponent(BugReportAttachments.sessionRecord)
        try await withAndroidGuest { model, operations in
            // The slot is rewritten at each 2 s checkpoint; the lease was
            // taken before Android finished booting.
            var network: NetworkRecord?
            var text = ""
            let deadline = ContinuousClock.now + .seconds(60)
            while ContinuousClock.now < deadline {
                text = (try? String(contentsOf: record, encoding: .utf8)) ?? ""
                network = NetworkRecord(text: text)
                if (network?.dhcp?.acks ?? 0) >= 1 { break }
                try await Task.sleep(for: .seconds(2))
            }
            let slot = text.split(separator: "\n").filter { $0.hasPrefix("network ") || $0.hasPrefix("net-") }
            print("NETWORK_RECORD: \(slot.joined(separator: " | "))")
            let found = try #require(network, "the session record has no network slot")
            let dhcp = try #require(found.dhcp, "the slot has no DHCP counts: the runtime wired no reader")
            #expect(dhcp.discovers >= 1 && dhcp.requests >= 1 && dhcp.acks >= 1)
            #expect(found.dns != nil && found.tcp != nil)
            // The bug report reads the same numbers.
            #expect(found.report().contains("DHCP: the guest sent"))
            let alive = try await operations.shell("getprop sys.boot_completed", limit: .seconds(30))
            #expect(alive.text.trimmingCharacters(in: .whitespacesAndNewlines) == "1")
        }
    }
}
