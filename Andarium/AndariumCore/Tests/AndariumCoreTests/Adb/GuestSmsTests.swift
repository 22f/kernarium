// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing

struct GuestSmsTests {
    @Test func sendersAreNumbersOrShortNames() {
        #expect(GuestSms.sender("+98 (912) 123-4567") == "+989121234567")
        #expect(GuestSms.sender("12345") == "12345")
        #expect(GuestSms.sender(" My Bank ") == "My Bank")
        #expect(GuestSms.sender("Twelve Chars") == nil)
        #expect(GuestSms.sender("Bänk") == nil)
        #expect(GuestSms.sender("+123456789012345678901") == nil)
        #expect(GuestSms.sender("") == nil)
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

    @Test func theCommandCarriesOnlyBase64() throws {
        let command = try #require(GuestSms.command(sender: "+1 555", text: "it's $HOME; `x`\nسلام"))
        let argument = try #require(command.firstMatch(of: /--arg '([A-Za-z0-9+\/=.]+)'$/)?.1)
        let fields = argument.split(separator: ".", omittingEmptySubsequences: false).map(String.init)
        #expect(fields.count == 2)
        #expect(String(decoding: try #require(Data(base64Encoded: fields[0])), as: UTF8.self) == "+1555")
        #expect(String(decoding: try #require(Data(base64Encoded: fields[1])), as: UTF8.self) == "it's $HOME; `x`\nسلام")
        #expect(command.hasPrefix("content call --uri content://android.kernarium.sms --method deliver "))
        #expect(GuestSms.command(sender: "", text: "hi") == nil)
    }

    @Test func outcomesComeFromWhatContentPrinted() {
        #expect(GuestSms.outcome(ShellResult(stdout: Data("Result: Bundle[{parts=2}]\n".utf8), exitCode: 0)) == .delivered(parts: 2))
        #expect(GuestSms.outcome(ShellResult(stderr: Data("Error while accessing provider:android.kernarium.sms\njava.lang.IllegalArgumentException: Unknown authority android.kernarium.sms\n".utf8), exitCode: 0)) == .unsupported)
        #expect(GuestSms.outcome(ShellResult(stderr: Data("java.lang.SecurityException: no\n".utf8), exitCode: 0)) == .failed)
        #expect(GuestSms.outcome(ShellResult(exitCode: 1)) == .failed)
    }
}
