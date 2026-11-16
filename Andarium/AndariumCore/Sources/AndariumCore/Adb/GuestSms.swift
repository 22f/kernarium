// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// A text message sent to the guest as if from the network. The guest's
/// KernariumSms service (android/lineage-24/overlay/sms) takes it from the adb
/// shell and delivers it as the radio stack would; the rules here are that
/// service's, so the guest refuses nothing the host lets through.
public enum GuestSms {
    /// The longest text, in UTF-16 units.
    public static let maximumUnits = 670
    /// The most parts a text of `maximumUnits` takes. The service puts 67
    /// units in a part, gives one back for the U+FEFF it adds before a part
    /// that starts with one, and ends a part a unit early rather than split
    /// a surrogate pair: every part but the last takes at least 65 units, so
    /// 670 take at most 11. Message.MAXIMUM_PARTS is the service's own count.
    public static let maximumParts = 11
    /// The authority of the guest's service.
    public static let authority = "android.kernarium.sms"

    public enum Refusal: Equatable, Sendable {
        case noSender, badSender, noText, tooLong
        /// The text holds U+FFFE, which Android's decoder cannot return: it
        /// swaps the bytes after it or replaces it (see the service's
        /// DeliverPdu), so the text would not arrive as typed.
        case unsendableCharacter
    }

    /// The sender as the guest takes it: a phone number of at most 20 digits,
    /// international with a leading +, with the spaces, dashes and brackets
    /// people type left out; or a name of at most 11 letters, digits and
    /// spaces, as a network sends a service's. nil for anything else.
    public static func sender(_ typed: String) -> String? {
        let trimmed = typed.trimmingCharacters(in: .whitespaces)
        let digits = trimmed.filter { !" -().".contains($0) }
        if digits.wholeMatch(of: /\+?[0-9]{1,20}/) != nil { return digits }
        if trimmed.wholeMatch(of: /[A-Za-z0-9 ]{1,11}/) != nil { return trimmed }
        return nil
    }

    public static func refusal(sender typed: String, text: String) -> Refusal? {
        if typed.trimmingCharacters(in: .whitespaces).isEmpty { return .noSender }
        if sender(typed) == nil { return .badSender }
        if text.isEmpty { return .noText }
        if text.utf16.count > maximumUnits { return .tooLong }
        if text.unicodeScalars.contains("\u{FFFE}") { return .unsendableCharacter }
        return nil
    }

    /// An identifier for one message: 128 random bits in lower-case hex. The
    /// service hands a message over once for each identifier it is given.
    public static func newIdentifier() -> String {
        (0..<16).map { _ in String(format: "%02x", UInt8.random(in: .min ... .max)) }.joined()
    }

    public static func isIdentifier(_ text: String) -> Bool {
        text.wholeMatch(of: /[0-9a-f]{32}/) != nil
    }

    /// The shell command that delivers the message; nil when it is refused
    /// or the identifier is not one. The sender and the text travel as base64
    /// of UTF-8, so the shell reads no character of theirs.
    public static func command(sender typed: String, text: String, identifier: String) -> String? {
        guard refusal(sender: typed, text: text) == nil, let sender = sender(typed), isIdentifier(identifier) else { return nil }
        let argument = [Data(sender.utf8).base64EncodedString(), Data(text.utf8).base64EncodedString(), identifier]
            .joined(separator: ".")
        return "content call --uri content://\(authority) --method deliver --arg '\(argument)'"
    }

    /// The identifiers of drafts whose send was not confirmed. When the
    /// person sends the same draft again the service may already have it, so
    /// the send carries the identifier of the first one and the service
    /// answers it without handing the message over twice. A send that was
    /// confirmed, or could not have been taken, forgets its draft: the same
    /// words sent again are a new message.
    public struct Retries: Sendable {
        /// The drafts kept; the oldest is forgotten first.
        public static let capacity = 16

        private struct Draft: Hashable, Sendable {
            let sender: [UInt8]
            let text: [UInt8]
        }

        private var unconfirmed: [Draft: String] = [:]
        private var order: [Draft] = []
        private let make: @Sendable () -> String

        public init(makeIdentifier: @escaping @Sendable () -> String = GuestSms.newIdentifier) {
            make = makeIdentifier
        }

        private static func draft(_ sender: String, _ text: String) -> Draft {
            Draft(sender: Array((GuestSms.sender(sender) ?? sender).utf8), text: Array(text.utf8))
        }

        /// The identifier to send this draft with now.
        public func identifier(sender: String, text: String) -> String {
            unconfirmed[Self.draft(sender, text)] ?? make()
        }

        /// Records how the send with `identifier` ended.
        public mutating func settle(sender: String, text: String, identifier: String, confirmed: Bool) {
            let draft = Self.draft(sender, text)
            order.removeAll { $0 == draft }
            guard !confirmed else {
                unconfirmed[draft] = nil
                return
            }
            unconfirmed[draft] = identifier
            order.append(draft)
            while order.count > Self.capacity { unconfirmed[order.removeFirst()] = nil }
        }
    }

    public enum Outcome: Equatable, Sendable {
        /// Android took the message, in this many parts. That is all the
        /// service reports: it does not wait for an application to read it.
        case handedToAndroid(parts: Int)
        /// The system image has no SMS service.
        case unsupported
        /// Anything else: the message may or may not have reached Android.
        case notConfirmed
    }

    /// What the command did. The service's answer is its result bundle on
    /// stdout, alone, after a command that exited 0 and wrote nothing to
    /// stderr, and its part count is one a text can take. `content` reports a
    /// failure on stderr whatever its exit status, and a stream can end
    /// part way, so anything short of that whole is not confirmed.
    public static func outcome(_ result: ShellResult) -> Outcome {
        let output = String(decoding: result.stdout, as: UTF8.self)
        let errors = String(decoding: result.stderr, as: UTF8.self)
        let silent = errors.allSatisfy(\.isWhitespace)
        if silent, result.exitCode == 0, let parts = resultParts(output), (1...maximumParts).contains(parts) {
            return .handedToAndroid(parts: parts)
        }
        if output.allSatisfy(\.isWhitespace), errors.contains("Could not find provider") || errors.contains("Unknown authority") {
            return .unsupported
        }
        return .notConfirmed
    }

    /// The part count of stdout when stdout is exactly the result bundle
    /// `content call` prints, and nil for any other text.
    private static func resultParts(_ output: String) -> Int? {
        var line = Substring(output)
        // "\r\n" is one Character in Swift.
        if let last = line.last, last == "\n" || last == "\r\n" { line = line.dropLast() }
        guard let match = line.wholeMatch(of: /Result: Bundle\[\{parts=([0-9]+)\}\]/) else { return nil }
        return Int(match.1)
    }
}
