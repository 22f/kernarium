// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The notice that the virtual machine's Android has no Google apps, on
/// purpose: a panel on the boot screen and, where Send feedback is compiled
/// in, a line at the top of its mail.
///
/// The panel belongs to a new installation's first Android boot, and nothing
/// is kept for it: a first start (a new virtual machine and one after Erase
/// alike) and the retry of one that ended before Android booted
/// (`InstanceFiles.firstBootPending`, the same condition as `SlowStart`'s).
/// A person who finished their first start before the notice existed meets it
/// only as the line. A start that restarts the running guest, boots recovery
/// or is any other normal or update start never shows the panel. The line in
/// Send feedback shows until the person has opened or closed it once, which is
/// one value in the defaults database; a launch that is not the person's (a
/// harness) keeps nothing, and its feedback sheet always shows the line.
public struct NoGoogleNotice {
    /// The defaults key of the line: set once it has been opened or closed.
    public static let feedbackKey = "andarium.noGoogleNotice.feedback"

    /// The database the value is kept in; nil keeps nothing.
    private let defaults: UserDefaults?

    public init(defaults: UserDefaults? = nil) {
        self.defaults = defaults
    }

    /// Whether the boot screen of a start of this kind carries the panel.
    /// `firstBootPending` is the guest's record when the start began.
    public static func showsOnBoot(_ kind: BootKind, firstBootPending: Bool) -> Bool {
        kind == .firstStart || (kind == .normal && firstBootPending)
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
