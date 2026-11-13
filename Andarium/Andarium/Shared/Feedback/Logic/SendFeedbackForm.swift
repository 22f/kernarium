// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import Foundation

/// What Send still lacks: the mail needs a problem (its subject) and details
/// (its body), and a file or the start log is neither.
nonisolated enum SendFeedbackMissing: Equatable, Sendable {
    case problem, details, both

    /// The Problem line is empty, so it takes the focus first.
    var focusesProblem: Bool { self != .details }
}

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
    /// The app can start a bug report now: a normal session whose runtime
    /// makes one, with the guest's adb channel and the progress overlay free
    /// (no file transfer or install is under way).
    var bugReportPossible = true
    /// A bug report is being made, under the progress overlay.
    var makingBugReport = false
    /// A screenshot is being taken.
    var takingScreenshot = false
    /// Picked photos and videos are being copied into the sheet's folder.
    var importingMedia = false
    /// Picked files are being looked at, or one that cannot be used where it
    /// is is being copied.
    var addingFiles = false

    init(mailAvailable: Bool, running: Bool) {
        self.mailAvailable = mailAvailable
        self.running = running
    }

    var trimmedProblem: String { problem.trimmingCharacters(in: .whitespacesAndNewlines) }
    var trimmedDetails: String { details.trimmingCharacters(in: .whitespacesAndNewlines) }

    /// What is empty of the two fields Send needs (white space and line breaks
    /// are empty); nil when both are written. Send stays enabled while one is
    /// missing, so that its tap can say which.
    var missing: SendFeedbackMissing? {
        switch (trimmedProblem.isEmpty, trimmedDetails.isEmpty) {
        case (true, true): .both
        case (true, false): .problem
        case (false, true): .details
        case (false, false): nil
        }
    }

    /// Something is under way that the sheet waits for.
    var isBusy: Bool { makingBugReport || takingScreenshot || importingMedia || addingFiles }

    /// The fields can be edited.
    var fieldsEnabled: Bool { mailAvailable && !makingBugReport }

    var canMakeBugReport: Bool {
        mailAvailable && running && bugReportPossible && !isBusy && attachments.bugReport == nil
    }
    var canTakeScreenshot: Bool { mailAvailable && running && !isBusy }
    var canAddMedia: Bool { mailAvailable && !isBusy }
    var canAddFiles: Bool { mailAvailable && !isBusy }
    /// Send can be tapped: it opens the mail when `missing` is nil and says what is missing otherwise.
    var canSend: Bool { mailAvailable && !isBusy }
    /// A tap on Send would open the mail. Send looks dimmed until then, so
    /// that nobody has to tap it to learn that a field is still empty.
    var readyToSend: Bool { canSend && missing == nil }

    /// Where a bug report would be, the way to one: only a running virtual
    /// machine can make it.
    var showsHowToReportBug: Bool { !running }

    /// Something is typed or attached: closing asks first, and the sheet
    /// cannot be swiped away.
    var hasContent: Bool {
        !trimmedProblem.isEmpty || !trimmedDetails.isEmpty || !attachments.isEmpty
    }
}

#endif
