// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// What the guest's user network counted during a session, and whether the
/// application told the person that iOS denies it the network, as the runtime
/// wrote them into the session record's network slot
/// (darwin/runtime/session_record.hpp).
///
/// Every number is a count; the record holds no address, port, host name or
/// name that was queried. A line this reader does not know is skipped and a
/// line with more fields than it knows is read by field name, so a later
/// runtime can add fields.
public struct NetworkRecord: Equatable, Sendable {
    /// Queries to the system's resolver, by how they ended.
    public struct DNS: Equatable, Sendable {
        /// A missing name or a name without a record of the type is an answer.
        public var answered: UInt64
        /// The system refused the query: the mark of a network iOS denies.
        public var policyDenied: UInt64
        public var timedOut: UInt64
        public var failed: UInt64

        public var total: UInt64 { answered &+ policyDenied &+ timedOut &+ failed }
    }

    /// The guest's DHCP messages and the lease acknowledgements that answered.
    public struct DHCP: Equatable, Sendable {
        public var discovers: UInt64
        public var requests: UInt64
        public var acks: UInt64
    }

    /// The guest's attempts to open a TCP connection to an outside address,
    /// by what the guest was told.
    public struct TCP: Equatable, Sendable {
        public var attempts: UInt64
        public var established: UInt64
        /// An ICMP net or host unreachable: the stack's answer to most
        /// connections the system refuses or cannot route.
        public var unreachable: UInt64
        public var refused: UInt64

        /// Attempts nothing had answered when the slot was written.
        public var unanswered: UInt64 {
            let answered = established &+ unreachable &+ refused
            return attempts > answered ? attempts - answered : 0
        }
    }

    /// DNS, DHCP and TCP counts; nil in a slot written while the runtime had
    /// no network to count (before the user network exists).
    public var dns: DNS?
    public var dhcp: DHCP?
    public var tcp: TCP?
    /// How many denials of the network the application's notice was up for
    /// during the session; one that was already in force when the session
    /// began counts once.
    public var noticeShown: Int
    /// The denial the notice was up for when the slot was written, in the
    /// runtime's word: none, cellular or wifi.
    public var noticeNow: String

    /// Reads the network slot out of a session record's text (or the slot
    /// alone); nil when the text has none, as the records of the earlier
    /// layouts, or one of a version this reader does not know.
    public init?(text: String) {
        var header = false
        var dns: DNS?
        var dhcp: DHCP?
        var tcp: TCP?
        var shown: Int?
        var now: String?
        // The kernel's quoted output is not the record's own lines.
        let own = text.components(separatedBy: "\nkernel-output\n")[0]
        for line in own.split(separator: "\n", omittingEmptySubsequences: true) {
            let words = line.split(separator: " ", omittingEmptySubsequences: true)
            guard let first = words.first else { continue }
            switch first {
            case "network":
                header = words.count >= 2 && words[1] == "1"
            case "net-dns":
                let values = Self.values(words)
                if let answered = values["answered"], let denied = values["policy-denied"],
                    let timedOut = values["timed-out"], let failed = values["failed"]
                {
                    dns = DNS(answered: answered, policyDenied: denied, timedOut: timedOut, failed: failed)
                }
            case "net-dhcp":
                let values = Self.values(words)
                if let discovers = values["discovers"], let requests = values["requests"],
                    let acks = values["acks"]
                {
                    dhcp = DHCP(discovers: discovers, requests: requests, acks: acks)
                }
            case "net-tcp":
                let values = Self.values(words)
                if let attempts = values["attempts"], let established = values["established"],
                    let unreachable = values["unreachable"], let refused = values["refused"]
                {
                    tcp = TCP(
                        attempts: attempts, established: established, unreachable: unreachable,
                        refused: refused)
                }
            case "net-notice":
                // net-notice shown N now WORD
                if let at = words.firstIndex(of: "shown"), at + 1 < words.count, let count = Int(words[at + 1]) {
                    shown = count
                }
                if let at = words.firstIndex(of: "now"), at + 1 < words.count { now = String(words[at + 1]) }
            default:
                break
            }
        }
        guard header, let shown, let now else { return nil }
        self.dns = dns
        self.dhcp = dhcp
        self.tcp = tcp
        noticeShown = shown
        noticeNow = now
    }

    /// The `name value` pairs after the line's first word.
    private static func values(_ words: [Substring]) -> [String: UInt64] {
        var values: [String: UInt64] = [:]
        var index = 1
        while index + 1 < words.count {
            if let value = UInt64(words[index + 1]) { values[String(words[index])] = value }
            index += 2
        }
        return values
    }

    /// The record in words, for the bug report.
    public func report() -> String {
        var lines: [String] = []
        if let dns {
            lines.append(
                "DNS: \(dns.total) queries to the system's resolver: \(dns.answered) answered (a missing name is an "
                    + "answer), \(dns.policyDenied) refused by the system, \(dns.timedOut) timed out, "
                    + "\(dns.failed) failed otherwise.")
        } else {
            lines.append("DNS: not counted.")
        }
        if let dhcp {
            lines.append(
                "DHCP: the guest sent \(dhcp.discovers) discover and \(dhcp.requests) request messages; "
                    + "\(dhcp.acks) leases were acknowledged.")
        } else {
            lines.append("DHCP: not counted.")
        }
        if let tcp {
            lines.append(
                "TCP: \(tcp.attempts) outbound connection attempts: \(tcp.established) connected, "
                    + "\(tcp.unreachable) answered with ICMP unreachable (a connection the system refused is "
                    + "counted here), \(tcp.refused) reset, \(tcp.unanswered) with no answer yet.")
        } else {
            lines.append("TCP: not counted.")
        }
        let now = noticeNow == "none" ? "no denial in force" : "the \(noticeNow) denial in force"
        lines.append(
            noticeShown == 0
                ? "Notice that iOS denies Andarium the network: not shown; \(now)."
                : "Notice that iOS denies Andarium the network: shown for \(noticeShown) denial"
                    + "\(noticeShown == 1 ? "" : "s") during the session; \(now) at the last write.")
        return lines.joined(separator: "\n")
    }
}
