// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import AndariumCore
import MessageUI
import UIKit

/// How a mail left the composer.
enum SendFeedbackMailResult: Equatable {
    case sent, saved, cancelled
    /// The system's own words for what went wrong.
    case failed(String)
}

/// What Send feedback asks of the system and of the guest, in one place a test
/// or the Simulator preview replaces: whether mail can be sent, the composer,
/// the guest's picture and the clock of the run. A launch of the application
/// has the live ones.
struct SendFeedbackServices {
    /// The receiver, from Info.plist; nil when it is absent or not well formed,
    /// which hides both entrances.
    var address: @MainActor () -> String? = { SendFeedbackConfiguration.address }
    /// `MFMailComposeViewController.canSendMail()`: a mail account exists and
    /// mail is not restricted.
    var canSendMail: @MainActor () -> Bool = { MFMailComposeViewController.canSendMail() }
    /// Presents the composer over whatever is on screen and reports how the
    /// person left it, once; false when there is nothing to present from.
    var presentMail: @MainActor (SendFeedbackMailRequest, @escaping @MainActor (SendFeedbackMailResult) -> Void) -> Bool =
        SendFeedbackComposer.present
    /// The virtual machine's display as it is now.
    var captureScreen: @MainActor (AppModel) -> UIImage? = { $0.driver?.captureGuestDisplay() }
    /// Seconds the virtual machine has run.
    var runningSeconds: @MainActor (AppModel) -> Int? = { $0.driver?.runningSeconds() }
    /// Opens the sheet once Info or Settings has settled, as a launch of the
    /// Simulator preview asks; never in the application.
    var opensAtLaunch = false
    /// Runs when the sheet appears: the Simulator preview fills a draft with it.
    var prepare: @MainActor (SendFeedbackModel) async -> Void = { _ in }
}

/// Where the receiver's address is read from.
enum SendFeedbackConfiguration {
    /// The Info.plist key the build writes the address to, only where Send
    /// feedback is compiled in (`Config/Andarium.xcconfig`).
    static let key = "AndariumFeedbackAddress"

    static var address: String? {
        guard let value = Bundle.main.object(forInfoDictionaryKey: key) as? String,
              SendFeedbackMail.isWellFormed(address: value) else { return nil }
        return value
    }
}

#endif
