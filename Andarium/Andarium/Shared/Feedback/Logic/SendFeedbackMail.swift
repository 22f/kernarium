// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import Foundation

// Send feedback's logic: plain values and rules, no UIKit. The application
// compiles these files with the rest of Send feedback, and only where it is
// compiled in; the AndariumCore package compiles the same files for its tests
// (Andarium/README.md#send-feedback).

/// What the mail Send feedback hands to the system's composer says.
nonisolated enum SendFeedbackMail {
    /// What the subject starts with, so the receiver can filter on it.
    static let subjectPrefix = "Andarium feedback: "
    /// The line between what the person wrote and the information block.
    static let separator = "--"

    /// The subject: the prefix and the problem on one line.
    static func subject(problem: String) -> String {
        let words = problem.components(separatedBy: .whitespacesAndNewlines).filter { !$0.isEmpty }
        return subjectPrefix + words.joined(separator: " ")
    }

    /// The details, a separator, then the information block. Without details
    /// the body starts with an empty line, where the person can still type.
    static func body(details: String, information: SendFeedbackInformation) -> String {
        let text = details.trimmingCharacters(in: .whitespacesAndNewlines)
        let head = text.isEmpty ? "\n\n" : text + "\n\n"
        return head + separator + "\n" + information.text + "\n"
    }

    /// Whether `address` can be a receiver: one `@` between a local part and
    /// a dotted domain, no spaces.
    static func isWellFormed(address: String) -> Bool {
        let parts = address.split(separator: "@", omittingEmptySubsequences: false)
        guard parts.count == 2, !parts[0].isEmpty, !address.contains(where: \.isWhitespace) else { return false }
        let domain = parts[1].split(separator: ".", omittingEmptySubsequences: false)
        return domain.count >= 2 && domain.allSatisfy { !$0.isEmpty }
    }
}

/// What the composer is given: where the mail goes, what it says and the files
/// that go with it.
nonisolated struct SendFeedbackMailRequest: Equatable, Sendable {
    var address: String
    var subject: String
    var body: String
    var attachments: [SendFeedbackAttachment]
}

#endif
