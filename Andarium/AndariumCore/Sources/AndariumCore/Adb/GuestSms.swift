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

    /// The shell command that delivers the message; nil when it is refused.
    /// The sender and the text travel as base64 of UTF-8, so the shell reads
    /// no character of theirs.
    public static func command(sender typed: String, text: String) -> String? {
        guard refusal(sender: typed, text: text) == nil, let sender = sender(typed) else { return nil }
        let argument = Data(sender.utf8).base64EncodedString() + "." + Data(text.utf8).base64EncodedString()
        return "content call --uri content://\(authority) --method deliver --arg '\(argument)'"
    }

    public enum Outcome: Equatable, Sendable {
        /// Delivered in this many parts.
        case delivered(parts: Int)
        /// The system image has no SMS service.
        case unsupported
        case failed
    }

    /// What the command did. `content` prints the provider's result bundle,
    /// and reports a failure on stderr whatever its exit status.
    public static func outcome(_ result: ShellResult) -> Outcome {
        let output = String(decoding: result.stdout, as: UTF8.self) + String(decoding: result.stderr, as: UTF8.self)
        if let match = output.firstMatch(of: /Result: Bundle\[\{parts=([0-9]+)\}\]/), let parts = Int(match.1) {
            return .delivered(parts: parts)
        }
        if output.contains("Could not find provider") || output.contains("Unknown authority") {
            return .unsupported
        }
        return .failed
    }
}
