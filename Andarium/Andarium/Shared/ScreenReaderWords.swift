// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// The words of Android's screen reader in the session's Settings, on the card
/// that asks about it and on Android's display. "TalkBack" is the title of the
/// control and of the card, which a person looks for by that name; every
/// explanation says "Android's screen reader". Each text is the one shown
/// and the one VoiceOver speaks.
nonisolated enum ScreenReaderWords {
    // MARK: Settings

    static var section: String { String(localized: "Accessibility") }
    /// The control's and the card's title.
    static var name: String { String(localized: "TalkBack") }
    static var androidSettings: String { String(localized: "Android\u{2019}s accessibility settings") }
    static var footer: String {
        String(localized: "Android\u{2019}s screen reader speaks what you touch. While it is on, VoiceOver can\u{2019}t read Android\u{2019}s screen, and your touches go to it. To come back to VoiceOver, touch the side keys.")
    }
    static var footerUnavailable: String { noReader }

    // MARK: The line under the switch

    static var checking: String { String(localized: "Checking\u{2026}") }
    static var couldntCheck: String { String(localized: "Couldn\u{2019}t check") }
    static var notInImage: String { String(localized: "Not in this system image") }
    static var waitsForTransfer: String { String(localized: "Available when the file transfer ends") }
    static var turningOn: String { String(localized: "Turning on\u{2026}") }
    static var turningOff: String { String(localized: "Turning off\u{2026}") }
    static var tryAgain: String { String(localized: "Try Again") }
    static var noReader: String { String(localized: "This system image has no screen reader for Android.") }
    static var notReady: String { String(localized: "The virtual machine isn\u{2019}t ready yet. Try again in a moment.") }

    static func didntChange(turningOn on: Bool) -> String {
        on ? String(localized: "Didn\u{2019}t turn on") : String(localized: "Didn\u{2019}t turn off")
    }

    // MARK: What VoiceOver says of a change

    static func announcement(on: Bool) -> String {
        on ? String(localized: "Android\u{2019}s screen reader is on.")
            : String(localized: "Android\u{2019}s screen reader is off.")
    }

    /// The failure as it is said: what did not happen, then why.
    static func changeFailure(turningOn on: Bool, reason: String) -> String {
        let title = on ? String(localized: "Android\u{2019}s screen reader didn\u{2019}t turn on.")
            : String(localized: "Android\u{2019}s screen reader didn\u{2019}t turn off.")
        return "\(title) \(reason)"
    }

    // MARK: The card

    static var askTitle: String { String(localized: "Turn on TalkBack?") }
    static var cantRead: String { String(localized: "VoiceOver can\u{2019}t read Android\u{2019}s screen.") }
    static var canRead: String {
        String(localized: "Android\u{2019}s screen reader can: it speaks in its own voice, and your touches on Android\u{2019}s screen go to it.")
    }
    static var wayBack: String { String(localized: "To come back to VoiceOver, touch the side keys.") }
    static var turnOn: String { String(localized: "Turn On") }
    static var notNow: String { String(localized: "Not Now") }
    static var ok: String { String(localized: "OK") }

    // MARK: Android's screen under VoiceOver

    static var displayLabel: String { String(localized: "Android screen") }
    static var displayHintDirect: String { String(localized: "Touches go to Android.") }
    static var displayHintOff: String {
        String(localized: "Turn on Android\u{2019}s screen reader in Settings, under the side keys.")
    }

    /// What the display says of the screen reader, from the session's state.
    @MainActor static func displayValue(_ reader: ScreenReaderSession) -> String {
        if let on = reader.changing {
            return on ? String(localized: "Turning on Android\u{2019}s screen reader\u{2026}")
                : String(localized: "Turning off Android\u{2019}s screen reader\u{2026}")
        }
        guard let state = reader.state else {
            return reader.readFailed ? String(localized: "Couldn\u{2019}t check Android\u{2019}s screen reader")
                : String(localized: "Checking Android\u{2019}s screen reader\u{2026}")
        }
        if !state.isInstalled { return String(localized: "No screen reader for Android in this system image") }
        return state.isOn ? String(localized: "Android\u{2019}s screen reader on")
            : String(localized: "Android\u{2019}s screen reader off")
    }

    /// The display's hint: what touching it does now, or where to turn the
    /// screen reader on; nothing where the image has none.
    @MainActor static func displayHint(_ reader: ScreenReaderSession) -> String? {
        guard let state = reader.state, state.isInstalled else { return nil }
        return state.isOn ? displayHintDirect : displayHintOff
    }
}
