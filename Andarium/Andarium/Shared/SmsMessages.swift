// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// How sending a text message to the guest ended, in the words shown under
/// the message box.
enum SmsSendResult: Equatable {
    case delivered(String)
    case refused(String)
}

/// The words of sending a text message to the virtual machine: Settings' row,
/// the fields, what is wrong with them, and how a send ended. "Text message"
/// is Android's own name for it, and the person's. Each string goes through
/// the String Catalog.
nonisolated enum SmsMessages {
    // MARK: Names

    /// The feature's name, and the placeholder of its message box.
    static var name: String { String(localized: "Text message") }
    /// Settings' row, which closes Settings and raises the bar over Android.
    static var row: String { String(localized: "Send text message\u{2026}") }
    /// The sender field's label for VoiceOver, and the label before it.
    static var from: String { String(localized: "From") }
    static var fromLabel: String { String(localized: "From:") }
    static var senderPlaceholder: String { String(localized: "Number or name") }
    /// What a sender can be: VoiceOver reads it as the field's hint.
    static var senderHint: String {
        String(localized: "A phone number, or a name of up to \(GuestSms.maximumNameLength) letters A\u{2013}Z, digits and spaces.")
    }
    /// The message box's label for VoiceOver.
    static var message: String { String(localized: "Message") }
    static var send: String { String(localized: "Send") }
    static var sending: String { String(localized: "Sending\u{2026}") }
    static var close: String { String(localized: "Close") }

    // MARK: What is wrong

    static var senderEmpty: String { String(localized: "Enter who the message is from.") }
    static var senderCharacters: String { String(localized: "A name can use only letters A\u{2013}Z, digits and spaces.") }
    static var senderLongName: String {
        String(localized: "A name can have at most \(GuestSms.maximumNameLength) characters.")
    }
    static var senderLongNumber: String {
        String(localized: "A number can have at most \(GuestSms.maximumNumberDigits) digits.")
    }
    /// The text holds a character that cannot be sent (`GuestSms.Refusal.unsendableCharacter`).
    static var unsendableCharacter: String { String(localized: "The message has a character that can\u{2019}t be sent.") }

    /// The room left in the message box, said once it runs short, and how far
    /// over the limit it is.
    static func charactersLeft(_ count: Int) -> String { String(localized: "\(count) characters left") }
    static func charactersOver(_ count: Int) -> String { String(localized: "\(count) characters too many") }

    // MARK: How a send ended

    /// Android took the message. That is all that is known of it, so that is
    /// all these words say.
    static var sentToAndroid: String { String(localized: "Sent to Android") }
    static var notConfirmed: String { String(localized: "Not confirmed") }
    /// What a send that was not confirmed says: the message may have arrived.
    static var notConfirmedAdvice: String {
        String(localized: "Android didn\u{2019}t confirm the message. Check whether it arrived before you send it again.")
    }
    static var unsupported: String { String(localized: "Android on this system image can\u{2019}t receive text messages.") }
    static var notRunning: String { String(localized: "Android isn\u{2019}t running.") }
    static var paused: String { String(localized: "Android is paused for lack of space.") }

    /// The words under the message box for a refusal that the model reports.
    static func refusal(_ refusal: GuestSms.Refusal, text: String) -> String {
        switch refusal {
        case .noSender: senderEmpty
        case .badSender: senderHint
        case .noText: message
        case .tooLong: charactersOver(text.utf16.count - GuestSms.maximumUnits)
        case .unsendableCharacter: unsendableCharacter
        }
    }
}
