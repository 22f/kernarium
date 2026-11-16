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

    /// The words say these numbers, so they are the rule's.
    @Test func theSenderLimitsAreTheRules() {
        #expect(GuestSms.maximumNameLength == 11)
        #expect(GuestSms.maximumNumberDigits == 20)
        #expect(GuestSms.sender(String(repeating: "a", count: GuestSms.maximumNameLength)) != nil)
        #expect(GuestSms.sender(String(repeating: "a", count: GuestSms.maximumNameLength + 1)) == nil)
        #expect(GuestSms.sender(String(repeating: "1", count: GuestSms.maximumNumberDigits)) != nil)
        #expect(GuestSms.sender("+" + String(repeating: "1", count: GuestSms.maximumNumberDigits)) != nil)
        #expect(GuestSms.sender(String(repeating: "1", count: GuestSms.maximumNumberDigits + 1)) == nil)
    }

    /// What a number pasted from a web page or a message brings along is made plain before it is judged.
    @Test func aPastedSenderIsCleanedBeforeItIsJudged() {
        // Direction marks around it, no-break spaces and typographic hyphens inside it.
        #expect(GuestSms.sender("\u{200E}+98\u{00A0}912\u{2011}123\u{2013}4567\u{200F}") == "+989121234567")
        #expect(GuestSms.sender("\u{202A}+1 (555) 010\u{2212}0100\u{202C}") == "+15550100100")
        // Every hyphen and dash of U+2010 to U+2013 and the minus sign is a hyphen.
        #expect(GuestSms.cleaned("1\u{2010}2\u{2011}3\u{2012}4\u{2013}5\u{2212}6") == "1-2-3-4-5-6")
        // Full-width digits and plus.
        #expect(GuestSms.sender("\u{FF0B}\u{FF18}\u{FF16}\u{FF11}\u{FF12}\u{FF13}\u{FF14}") == "+861234")
        #expect(GuestSms.sender("\u{FF11}\u{FF12}\u{FF13}") == "123")
        // Full-width letters are the plain letters.
        #expect(GuestSms.sender("\u{FF22}ank") == "Bank")
        // A name keeps its plain letters and its single spaces; the marks around it go.
        #expect(GuestSms.sender("\u{200F}My\u{00A0}Bank\u{200E}") == "My Bank")
        // What no rule makes plain stays refused.
        #expect(GuestSms.sender("Ba\u{200B}nk") == nil)
        #expect(GuestSms.sender("Bank\u{200D}") == nil)
        #expect(GuestSms.sender("Ban\u{0301}k") == nil)
        #expect(GuestSms.sender("\u{0661}\u{0662}\u{0663}") == nil)
        // The refusal judges the cleaned sender, and says what is wrong with it.
        #expect(GuestSms.refusal(sender: "\u{200E}\u{00A0}", text: "hi") == .noSender)
        #expect(GuestSms.refusal(sender: "\u{200E}+98\u{00A0}912\u{2011}123\u{2013}4567", text: "hi") == nil)
    }

    /// A number typed on a Chinese or Japanese keyboard comes out in full-width forms and, on a kana
    /// keyboard, with the prolonged sound mark for a hyphen: it is the number its ASCII form is.
    @Test func aNumberTypedOnACjkKeyboardIsTheSameNumberAsItsAsciiForm() {
        let pairs: [(typed: String, ascii: String)] = [
            ("\u{FF10}\u{FF19}\u{FF10}\u{30FC}\u{FF11}\u{FF12}\u{FF13}\u{FF14}\u{30FC}\u{FF15}\u{FF16}\u{FF17}\u{FF18}", "090-1234-5678"),
            ("\u{FF0B}\u{FF18}\u{FF11}\u{3000}\u{FF19}\u{FF10}\u{FF08}\u{FF11}\u{FF12}\u{FF13}\u{FF14}\u{FF09}\u{FF15}\u{FF16}\u{FF17}\u{FF18}", "+81 90(1234)5678"),
            ("\u{FF0B}\u{FF18}\u{FF16}\u{FF0D}\u{FF11}\u{FF12}\u{FF0E}\u{FF13}\u{FF14}", "+86-12.34"),
            ("\u{FF11}\u{FF12}\u{FF13}\u{FF0D}\u{FF14}\u{FF15}\u{FF16}", "123-456"),
        ]
        for (typed, ascii) in pairs {
            let sender = GuestSms.sender(ascii)
            #expect(sender != nil, "\(ascii)")
            #expect(GuestSms.sender(typed) == sender, "\(typed.debugDescription)")
            #expect(GuestSms.refusal(sender: typed, text: "hi") == nil, "\(typed.debugDescription)")
        }
        #expect(GuestSms.sender("\u{FF10}\u{FF19}\u{FF10}\u{30FC}\u{FF11}\u{FF12}\u{FF13}\u{FF14}\u{30FC}\u{FF15}\u{FF16}\u{FF17}\u{FF18}") == "09012345678")
        // The mark is a hyphen only between two digits.
        for typed in ["\u{30FC}", "1\u{30FC}", "\u{30FC}1", "Ab\u{30FC}12", "1\u{30FC}\u{30FC}2", "\u{30FC}A", "\u{3042}\u{30FC}"] {
            #expect(GuestSms.sender(typed) == nil, "\(typed.debugDescription)")
        }
        // A name typed plainly is unchanged.
        #expect(GuestSms.sender("My Bank 12") == "My Bank 12")
        #expect(GuestSms.cleaned("My Bank 12") == "My Bank 12")
    }

    /// Each limit is stated once, as a constant, and the check and the words follow it.
    @Test func theLimitsOfASenderAreTheConstantsAtTheirEdges() {
        let name = GuestSms.maximumNameLength, digits = GuestSms.maximumNumberDigits
        #expect(GuestSms.senderRefusal(String(repeating: "x", count: name)) == nil)
        #expect(GuestSms.senderRefusal(String(repeating: "x", count: name + 1)) == .senderLongName)
        #expect(GuestSms.senderRefusal(String(repeating: "9", count: digits)) == nil)
        #expect(GuestSms.senderRefusal(String(repeating: "9", count: digits + 1)) == .senderLongNumber)
        // A name of digits and spaces counts its characters, not the digits it has.
        #expect(GuestSms.senderRefusal("1 2 3 4 5 6") == nil)
    }

    @Test func aSenderIsRefusedForTheRightReason() {
        #expect(GuestSms.senderRefusal("") == .noSender)
        #expect(GuestSms.senderRefusal("   ") == .noSender)
        #expect(GuestSms.senderRefusal("Shop & Go") == .senderCharacters)
        #expect(GuestSms.senderRefusal("B\u{00E4}nk") == .senderCharacters)
        #expect(GuestSms.senderRefusal("+") == .senderCharacters)
        #expect(GuestSms.senderRefusal("12+34") == .senderCharacters)
        #expect(GuestSms.senderRefusal("Twelve Chars") == .senderLongName)
        #expect(GuestSms.senderRefusal("123456789012 name") == .senderLongName)
        #expect(GuestSms.senderRefusal(String(repeating: "1", count: 21)) == .senderLongNumber)
        #expect(GuestSms.senderRefusal("+" + String(repeating: "1", count: 21)) == .senderLongNumber)
        #expect(GuestSms.senderRefusal("1234 5678 9012 3456 7890 1") == .senderLongNumber)
        #expect(GuestSms.senderRefusal("Bank") == nil)
        #expect(GuestSms.senderRefusal("+1 (555) 010-0100") == nil)
        #expect(GuestSms.textRefusal("") == .noText)
        #expect(GuestSms.textRefusal("hi") == nil)
        // The sender is judged before the text.
        #expect(GuestSms.refusal(sender: "", text: "") == .noSender)
        #expect(GuestSms.defaultSender == "Andarium")
        #expect(GuestSms.senderRefusal(GuestSms.defaultSender) == nil)
    }

    @Test func refusalsNameWhatIsWrong() {
        #expect(GuestSms.refusal(sender: " ", text: "hi") == .noSender)
        #expect(GuestSms.refusal(sender: "a@b", text: "hi") == .senderCharacters)
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

    /// The shell reads nothing of the sender or the text: whatever is typed or pasted travels as base64, so the
    /// command is the same shape for every message.
    @Test func hostileTextReachesTheShellOnlyAsBase64() throws {
        let texts = [
            "'; rm -rf /; '", "$(id)", "`id`", "\\'\"", "a\nb\r\nc", "tab\there", "nul\u{0}byte", "|&;<>(){}[]*?~#!",
            "--arg x", "\u{2028}\u{2029}", "\u{202E}reversed", "\u{FEFF}\u{FEFF}", "😀 and 日本語 and العربية",
            "\u{0301}\u{200D}\u{200B}", "%s %n %d", "../../etc/passwd", "content://other", " ",
        ]
        for text in texts {
            let command = try #require(GuestSms.command(sender: "Bank", text: text, identifier: Self.identifier), "\(text.debugDescription)")
            let argument = try #require(command.firstMatch(of: /--arg '([A-Za-z0-9+\/=.]+)'$/)?.1, "\(text.debugDescription)")
            // One quoted argument, three fields, and nothing outside the quotes but the fixed words.
            #expect(command.filter { $0 == "'" }.count == 2)
            let fields = argument.split(separator: ".", omittingEmptySubsequences: false).map(String.init)
            #expect(fields.count == 3)
            #expect(String(decoding: try #require(Data(base64Encoded: fields[1])), as: UTF8.self) == text)
        }
    }

    /// A sender is a number or a short name: nothing else becomes one, whatever it would mean to a shell.
    /// What cleans to a valid sender is listed with the exact value it is sent as; every other input is
    /// refused, and the row asserts that for each of them whether or not the validator agrees.
    @Test func hostileSendersAreRefusedAndNeverBuildACommand() throws {
        let cleaned: [(typed: String, sent: String)] = [
            ("\u{202E}Bank\u{200E}", "Bank"), ("Bank\u{202E}", "Bank"), ("Bank\u{200B}", "Bank"),
            ("\u{FF21}\u{FF22}", "AB"), ("\u{FF0B}\u{FF11}", "+1"),
        ]
        for (typed, sent) in cleaned {
            #expect(GuestSms.sender(typed) == sent, "\(typed.debugDescription)")
            #expect(GuestSms.senderRefusal(typed) == nil, "\(typed.debugDescription)")
            let command = try #require(GuestSms.command(sender: typed, text: "hi", identifier: Self.identifier))
            #expect(command == GuestSms.command(sender: sent, text: "hi", identifier: Self.identifier), "\(typed.debugDescription)")
        }
        let refused = [
            "'; id; '", "$(id)", "`id`", "a\nb", "Bank\u{0}", "a|b", "a&b", "\"Bank\"", "a;b", "a b\\", "../x", "a/b",
            "Ba\u{200B}nk", "\u{0661}\u{0662}", "+", "++1", "1+", "1\u{0}2", "a\tb", "😀",
            "\u{FF07}\u{FF1B}id\u{FF1B}\u{FF07}", "\u{FF04}\u{FF08}id\u{FF09}", "\u{FF5C}", "\u{FF1B}",
        ]
        for sender in refused {
            #expect(GuestSms.sender(sender) == nil, "\(sender.debugDescription)")
            #expect(GuestSms.senderRefusal(sender) != nil, "\(sender.debugDescription)")
            #expect(GuestSms.command(sender: sender, text: "hi", identifier: Self.identifier) == nil, "\(sender.debugDescription)")
        }
    }

    /// The edges of what Android takes, in the units the guest counts: the text that takes the most parts, an
    /// emoji's two units, and the one unit over.
    @Test func theTextLimitIsCountedInUtf16Units() {
        func refusal(_ text: String) -> GuestSms.Refusal? { GuestSms.refusal(sender: "Bank", text: text) }
        #expect(refusal(String(repeating: "x", count: GuestSms.maximumUnits)) == nil)
        #expect(refusal(String(repeating: "x", count: GuestSms.maximumUnits + 1)) == .tooLong)
        #expect(refusal(String(repeating: "😀", count: GuestSms.maximumUnits / 2)) == nil)
        #expect(refusal(String(repeating: "😀", count: GuestSms.maximumUnits / 2) + "x") == .tooLong)
        // A combining sequence is as many units as it has scalars.
        #expect(refusal(String(repeating: "e\u{0301}", count: GuestSms.maximumUnits / 2)) == nil)
        #expect(refusal(String(repeating: "e\u{0301}", count: GuestSms.maximumUnits / 2) + "x") == .tooLong)
        #expect(refusal("") == .noText)
    }

    /// The longest command is well inside the guest console's line, whatever the text is made of.
    @Test func theLongestCommandFitsTheConsoleLine() throws {
        // Three UTF-8 bytes for each unit is the most the units allow.
        let text = String(repeating: "\u{20AC}", count: GuestSms.maximumUnits)
        let sender = String(repeating: "1", count: GuestSms.maximumNumberDigits)
        let command = try #require(GuestSms.command(sender: sender, text: text, identifier: Self.identifier))
        #expect(command.utf8.count < 4096, "\(command.utf8.count)")
    }

    private static func shell(_ stdout: String = "", stderr: String = "", exit: Int? = 0) -> ShellResult {
        ShellResult(stdout: Data(stdout.utf8), stderr: Data(stderr.utf8), exitCode: exit)
    }

    @Test func aCompleteResultOnStdoutIsHandedToAndroid() {
        #expect(GuestSms.outcome(Self.shell("Result: Bundle[{parts=2}]\n")) == .handedToAndroid(parts: 2))
        #expect(GuestSms.outcome(Self.shell("Result: Bundle[{parts=1}]\r\n")) == .handedToAndroid(parts: 1))
        #expect(GuestSms.outcome(Self.shell("Result: Bundle[{parts=1}]")) == .handedToAndroid(parts: 1))
        #expect(GuestSms.outcome(Self.shell("Result: Bundle[{parts=\(GuestSms.maximumParts)}]\n"))
            == .handedToAndroid(parts: GuestSms.maximumParts))
        // A newline of stderr alone is not an error.
        #expect(GuestSms.outcome(Self.shell("Result: Bundle[{parts=3}]\n", stderr: "\n")) == .handedToAndroid(parts: 3))
    }

    /// Whatever is short of that is not confirmed: the message may have arrived, or not.
    @Test func anythingElseIsNotConfirmed() {
        let result = "Result: Bundle[{parts=2}]\n"
        let cases: [(String, ShellResult)] = [
            ("no output", Self.shell()),
            ("no exit status", Self.shell(result, exit: nil)),
            ("a failing exit", Self.shell(result, exit: 1)),
            ("success beside an error", Self.shell(result, stderr: "java.lang.SecurityException: no\n")),
            ("the result on stderr", Self.shell(stderr: result)),
            ("a line before", Self.shell("Starting\n" + result)),
            ("a line after", Self.shell(result + "Done\n")),
            ("two results", Self.shell(result + result)),
            ("a space before", Self.shell(" " + result)),
            ("a truncated result", Self.shell("Result: Bundle[{parts=2")),
            ("another key", Self.shell("Result: Bundle[{parts=2, x=1}]\n")),
            ("another key alone", Self.shell("Result: Bundle[{count=2}]\n")),
            ("no parts", Self.shell("Result: Bundle[{}]\n")),
            ("no part count", Self.shell("Result: Bundle[{parts=}]\n")),
            ("zero parts", Self.shell("Result: Bundle[{parts=0}]\n")),
            ("a sign", Self.shell("Result: Bundle[{parts=+2}]\n")),
            ("a negative count", Self.shell("Result: Bundle[{parts=-1}]\n")),
            ("more parts than a text takes", Self.shell("Result: Bundle[{parts=\(GuestSms.maximumParts + 1)}]\n")),
            ("a count that overflows", Self.shell("Result: Bundle[{parts=99999999999999999999}]\n")),
            ("full-width digits", Self.shell("Result: Bundle[{parts=\u{FF12}}]\n")),
            ("a null result", Self.shell("Result: null\n")),
            ("an error on stdout", Self.shell("Error while accessing provider:android.kernarium.sms\n")),
            ("a security exception", Self.shell(stderr: "java.lang.SecurityException: only the adb shell may deliver a message\n")),
            ("an illegal argument", Self.shell(stderr: "java.lang.IllegalArgumentException: not an identifier\n")),
            ("not UTF-8", ShellResult(stdout: Data([0xff, 0xfe]), exitCode: 0)),
            ("a failing exit alone", Self.shell(exit: 1)),
        ]
        for (name, result) in cases {
            #expect(GuestSms.outcome(result) == .notConfirmed, Comment(rawValue: name))
        }
    }

    @Test func noServiceIsKnownFromWhatContentSaysOfTheAuthority() {
        let missing = "Error while accessing provider:android.kernarium.sms\njava.lang.IllegalArgumentException: Unknown authority android.kernarium.sms\n"
        #expect(GuestSms.outcome(Self.shell(stderr: missing)) == .unsupported)
        #expect(GuestSms.outcome(Self.shell(stderr: "Could not find provider: android.kernarium.sms\n")) == .unsupported)
        // A result beside it, or a failing exit, says something else happened.
        #expect(GuestSms.outcome(Self.shell("Result: Bundle[{parts=2}]\n", stderr: missing)) == .notConfirmed)
        #expect(GuestSms.outcome(Self.shell("Starting\n", stderr: missing)) == .notConfirmed)
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

    @Test func theSenderIsKeptUntilTheVirtualMachineIsErased() throws {
        let suite = "GuestSmsTests-\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let memory = GuestSms.SenderMemory(defaults: defaults)
        #expect(memory.sender == nil)
        memory.keep("My Bank")
        // Another launch reads the same defaults.
        #expect(GuestSms.SenderMemory(defaults: defaults).sender == "My Bank")
        memory.keep("")
        #expect(memory.sender == nil)
        memory.keep("+15551234567")
        memory.forget()
        #expect(memory.sender == nil)
        #expect(defaults.object(forKey: GuestSms.SenderMemory.key) == nil)
    }
}
