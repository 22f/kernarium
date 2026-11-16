// SPDX-License-Identifier: GPL-2.0-or-later

import UIKit

/// What Andarium tells VoiceOver of its own accord: a new screen, an operation
/// that began or ended, TalkBack turning on or off. Each call does nothing
/// while VoiceOver is off.
enum VoiceOver {
    /// Speaks `text`. A queued announcement waits for what VoiceOver is
    /// saying; otherwise it interrupts it.
    static func announce(_ text: String, queued: Bool = false) {
        guard UIAccessibility.isVoiceOverRunning else { return }
        let words = NSAttributedString(
            string: text, attributes: [.accessibilitySpeechQueueAnnouncement: queued])
        UIAccessibility.post(notification: .announcement, argument: words)
    }

    /// The screen changed as a whole: VoiceOver moves to its first element,
    /// and says `text` instead when it is given.
    static func screenChanged(_ text: String? = nil) {
        guard UIAccessibility.isVoiceOverRunning else { return }
        UIAccessibility.post(notification: .screenChanged, argument: text)
    }
}
