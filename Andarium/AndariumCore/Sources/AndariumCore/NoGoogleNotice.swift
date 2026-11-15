// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What Andarium keeps of the notice that the virtual machine's Android has no
/// Google apps, on purpose: a panel on the boot screen and, where Send
/// feedback is compiled in, a line at the top of its mail.
///
/// The panel is part of the first start: every first start shows it, a new
/// virtual machine and one after Erase alike. An installation that finished
/// its first start before the notice existed sees it once more, on its next
/// normal or update start; a start that only restarts the running guest or
/// boots recovery never shows it. The line in Send feedback shows until the
/// person has opened or closed it once.
///
/// Each is one value in the defaults database, set when the person has been
/// shown the notice. A launch that is not the person's (a harness) keeps
/// nothing: its boot screen shows the panel on a first start only, and its
/// feedback sheet always shows the line.
public struct NoGoogleNotice {
    /// The defaults key of the panel: set once it has been shown.
    public static let bootKey = "andarium.noGoogleNotice.boot"
    /// The defaults key of the line: set once it has been opened or closed.
    public static let feedbackKey = "andarium.noGoogleNotice.feedback"

    /// The database the values are kept in; nil keeps nothing.
    private let defaults: UserDefaults?

    public init(defaults: UserDefaults? = nil) {
        self.defaults = defaults
    }

    /// Whether the boot screen of a start of this kind carries the panel.
    public func showsOnBoot(_ kind: BootKind) -> Bool {
        switch kind {
        case .firstStart:
            true
        case .normal, .upgrade:
            defaults.map { !$0.bool(forKey: Self.bootKey) } ?? false
        case .recovery, .restart:
            false
        }
    }

    /// The panel has been shown: a start that is not a first one shows it no more.
    public func keepBootShown() {
        defaults?.set(true, forKey: Self.bootKey)
    }

    /// Whether Send feedback opens with the line.
    public var showsInFeedback: Bool {
        defaults.map { !$0.bool(forKey: Self.feedbackKey) } ?? true
    }

    /// The person opened the line or closed it: the next sheet opens without it.
    public func keepFeedbackAnswered() {
        defaults?.set(true, forKey: Self.feedbackKey)
    }
}
