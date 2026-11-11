// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import Foundation

/// What the person has written and attached, and what each control may do:
/// every rule of the sheet's enabling in one place.
nonisolated struct SendFeedbackForm: Equatable, Sendable {
    var problem = ""
    var details = ""
    var attachments = SendFeedbackAttachments()
    /// The system can send mail (a mail account exists and mail is not
    /// restricted). Nothing can be edited or attached while it cannot.
    var mailAvailable: Bool
    /// A virtual machine runs: the sheet was opened from the session's
    /// Settings. Without one no bug report or screenshot can be made.
    var running: Bool
    /// A bug report is being made, under the progress overlay.
    var makingBugReport = false
    /// A screenshot is being taken.
    var takingScreenshot = false
    /// Picked photos and videos are being copied into the sheet's folder.
    var importingMedia = false

    init(mailAvailable: Bool, running: Bool) {
        self.mailAvailable = mailAvailable
        self.running = running
    }

    var trimmedProblem: String { problem.trimmingCharacters(in: .whitespacesAndNewlines) }

    /// Something is under way that the sheet waits for.
    var isBusy: Bool { makingBugReport || takingScreenshot || importingMedia }

    /// The fields can be edited.
    var fieldsEnabled: Bool { mailAvailable && !makingBugReport }

    var canMakeBugReport: Bool { mailAvailable && running && !isBusy && attachments.bugReport == nil }
    var canTakeScreenshot: Bool { mailAvailable && running && !isBusy }
    var canAddMedia: Bool { mailAvailable && !isBusy }
    var canSend: Bool { mailAvailable && !isBusy && !trimmedProblem.isEmpty }

    /// Where a bug report would be, the way to one: only a running virtual
    /// machine can make it.
    var showsHowToReportBug: Bool { !running }

    /// Something is typed or attached: closing asks first, and the sheet
    /// cannot be swiped away.
    var hasContent: Bool {
        !trimmedProblem.isEmpty
            || !details.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            || !attachments.isEmpty
    }
}

#endif
