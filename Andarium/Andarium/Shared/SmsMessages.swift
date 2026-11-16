// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore

/// How sending a text message to the guest ended, in the words shown under
/// the message box.
enum SmsSendResult: Equatable {
    case delivered(String)
    case refused(String)
}

enum SmsMessages {
    static func delivered(parts: Int) -> String {
        parts > 1 ? "Delivered to Android as a \(parts)-part message." : "Delivered to Android."
    }

    static func refusal(_ refusal: GuestSms.Refusal) -> String {
        switch refusal {
        case .noSender: "Enter who the message is from."
        case .badSender: "The sender must be a phone number, or a name of up to 11 letters, digits and spaces."
        case .noText: "Enter the message."
        case .tooLong: "The message is longer than \(GuestSms.maximumUnits) characters."
        case .unsendableCharacter: "The message has a character that Android would change on the way."
        }
    }

    static let notRunning = "Android isn\u{2019}t running."
    static let unsupported = "This system image can\u{2019}t receive text messages. Update Andarium to get one that can."
    static let failed = "Android didn\u{2019}t take the message. Try again once it has finished starting."
}
