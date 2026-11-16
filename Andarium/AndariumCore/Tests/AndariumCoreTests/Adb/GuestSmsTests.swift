// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing

/// Identifiers in sequence, for a `Retries` that must not draw at random.
private final class Counter: @unchecked Sendable {
    private var value = 0

    func next() -> String {
        value += 1
        return String(format: "%032x", value)
    }
}

struct GuestSmsTests {
    private static let identifier = "0123456789abcdef0123456789abcdef"

    @Test func sendersAreNumbersOrShortNames() {
        #expect(GuestSms.sender("+98 (912) 123-4567") == "+989121234567")
        #expect(GuestSms.sender("12345") == "12345")
        #expect(GuestSms.sender(" My Bank ") == "My Bank")
        #expect(GuestSms.sender("Twelve Chars") == nil)
        #expect(GuestSms.sender("Bänk") == nil)
        #expect(GuestSms.sender("+123456789012345678901") == nil)
        #expect(GuestSms.sender("") == nil)
    }

    /// The service states the same numbers once (Message.MAXIMUM_UNITS and MAXIMUM_PARTS), and its own test
    /// builds the text that takes the most parts.
    @Test func theLimitsAreTheServices() {
        #expect(GuestSms.maximumUnits == 670)
        #expect(GuestSms.maximumParts == 11)
        // Every part but the last takes at least 65 units of the text.
        #expect((GuestSms.maximumParts - 1) * 65 + 1 <= GuestSms.maximumUnits)
        #expect(GuestSms.maximumParts * 65 + 1 > GuestSms.maximumUnits)
    }

    @Test func refusalsNameWhatIsWrong() {
        #expect(GuestSms.refusal(sender: " ", text: "hi") == .noSender)
        #expect(GuestSms.refusal(sender: "a@b", text: "hi") == .badSender)
        #expect(GuestSms.refusal(sender: "Bank", text: "") == .noText)
        #expect(GuestSms.refusal(sender: "Bank", text: String(repeating: "x", count: 671)) == .tooLong)
        // An emoji is two UTF-16 units, as the guest counts them.
        #expect(GuestSms.refusal(sender: "Bank", text: String(repeating: "😀", count: 336)) == .tooLong)
        #expect(GuestSms.refusal(sender: "Bank", text: String(repeating: "x", count: 670)) == nil)
    }

    /// Android decodes each part as UTF-16 with a byte order mark: U+FEFF travels (the service doubles it
    /// at a part's start) and U+FFFE cannot, anywhere in the text.
    @Test func theReversedMarkIsRefusedAndTheMarkIsNot() {
        #expect(GuestSms.refusal(sender: "Bank", text: "\u{FFFE}") == .unsendableCharacter)
        #expect(GuestSms.refusal(sender: "Bank", text: "ab\u{FFFE}cd") == .unsendableCharacter)
        #expect(GuestSms.refusal(sender: "Bank", text: String(repeating: "x", count: 300) + "\u{FFFE}") == .unsendableCharacter)
        #expect(GuestSms.command(sender: "Bank", text: "a\u{FFFE}", identifier: Self.identifier) == nil)
        #expect(GuestSms.refusal(sender: "Bank", text: "\u{FEFF}abc") == nil)
        #expect(GuestSms.refusal(sender: "Bank", text: "\u{FEFF}") == nil)
        #expect(GuestSms.refusal(sender: "Bank", text: "ab\u{FEFF}") == nil)
        #expect(GuestSms.refusal(sender: "Bank", text: "\u{FFFF}") == nil)
    }

    @Test func theCommandCarriesOnlyBase64() throws {
        let command = try #require(GuestSms.command(sender: "+1 555", text: "it's $HOME; `x`\nسلام", identifier: Self.identifier))
        let argument = try #require(command.firstMatch(of: /--arg '([A-Za-z0-9+\/=.]+)'$/)?.1)
        let fields = argument.split(separator: ".", omittingEmptySubsequences: false).map(String.init)
        #expect(fields.count == 3)
        #expect(fields[2] == Self.identifier)
        #expect(String(decoding: try #require(Data(base64Encoded: fields[0])), as: UTF8.self) == "+1555")
        #expect(String(decoding: try #require(Data(base64Encoded: fields[1])), as: UTF8.self) == "it's $HOME; `x`\nسلام")
        #expect(command.hasPrefix("content call --uri content://android.kernarium.sms --method deliver "))
        #expect(GuestSms.command(sender: "", text: "hi", identifier: Self.identifier) == nil)
        // The identifier is 32 lower-case hex digits, nothing the shell could read.
        for identifier in ["", "0123", Self.identifier.uppercased(), Self.identifier + "0", String(Self.identifier.dropLast()) + "'", String(Self.identifier.dropLast()) + "g"] {
            #expect(GuestSms.command(sender: "Bank", text: "hi", identifier: identifier) == nil)
        }
    }

    @Test func outcomesComeFromWhatContentPrinted() {
        #expect(GuestSms.outcome(ShellResult(stdout: Data("Result: Bundle[{parts=2}]\n".utf8), exitCode: 0)) == .handedToAndroid(parts: 2))
        #expect(GuestSms.outcome(ShellResult(stderr: Data("Error while accessing provider:android.kernarium.sms\njava.lang.IllegalArgumentException: Unknown authority android.kernarium.sms\n".utf8), exitCode: 0)) == .unsupported)
        #expect(GuestSms.outcome(ShellResult(stderr: Data("java.lang.SecurityException: no\n".utf8), exitCode: 0)) == .failed)
        #expect(GuestSms.outcome(ShellResult(exitCode: 1)) == .failed)
    }

    @Test func identifiersAreRandomHex() {
        let identifiers = Set((0..<50).map { _ in GuestSms.newIdentifier() })
        #expect(identifiers.count == 50)
        #expect(identifiers.allSatisfy(GuestSms.isIdentifier))
    }

    /// The same draft sent again after an unconfirmed result keeps its identifier, so the service can answer
    /// it without handing the message over twice; two different messages never share one.
    @Test func anUnconfirmedDraftKeepsItsIdentifier() {
        let counter = Counter()
        var retries = GuestSms.Retries { counter.next() }
        let first = retries.identifier(sender: "Bank", text: "code 1")
        // Not settled yet: nothing is kept.
        #expect(retries.identifier(sender: "Bank", text: "code 1") != first)
        retries.settle(sender: "Bank", text: "code 1", identifier: first, confirmed: false)
        #expect(retries.identifier(sender: "Bank", text: "code 1") == first)
        // The sender as typed differs, the sender sent does not.
        #expect(retries.identifier(sender: " Bank ", text: "code 1") == first)
        // Another draft in flight has an identifier of its own, and the first stays.
        let other = retries.identifier(sender: "Bank", text: "code 2")
        #expect(other != first)
        retries.settle(sender: "Bank", text: "code 2", identifier: other, confirmed: false)
        #expect(retries.identifier(sender: "Bank", text: "code 1") == first)
        #expect(retries.identifier(sender: "Bank", text: "code 2") == other)
        #expect(retries.identifier(sender: "Other", text: "code 1") != first)
        // A confirmed send forgets its draft: the same words again are a new message.
        retries.settle(sender: "Bank", text: "code 1", identifier: first, confirmed: true)
        #expect(retries.identifier(sender: "Bank", text: "code 1") != first)
        #expect(retries.identifier(sender: "Bank", text: "code 2") == other)
    }

    @Test func theRetryMemoryIsBounded() {
        var retries = GuestSms.Retries()
        var first = ""
        for number in 0...GuestSms.Retries.capacity {
            let identifier = retries.identifier(sender: "Bank", text: "m\(number)")
            if number == 0 { first = identifier }
            retries.settle(sender: "Bank", text: "m\(number)", identifier: identifier, confirmed: false)
        }
        #expect(retries.identifier(sender: "Bank", text: "m0") != first)
        #expect(retries.identifier(sender: "Bank", text: "m\(GuestSms.Retries.capacity)") != "")
    }

    /// Two texts that Swift treats as equal can be different bytes on the wire.
    @Test func theDraftIsComparedByItsBytes() {
        var retries = GuestSms.Retries()
        let composed = "caf\u{E9}"
        let decomposed = "cafe\u{301}"
        #expect(composed == decomposed)
        let identifier = retries.identifier(sender: "Bank", text: composed)
        retries.settle(sender: "Bank", text: composed, identifier: identifier, confirmed: false)
        #expect(retries.identifier(sender: "Bank", text: decomposed) != identifier)
    }
}
