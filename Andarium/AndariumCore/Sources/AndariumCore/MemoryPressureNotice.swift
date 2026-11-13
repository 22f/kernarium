// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// How short of memory iOS says the device is, as the runtime reports the
/// levels of the system's memory pressure.
public enum MemoryPressureLevel: Int, Comparable, Sendable {
    case normal = 0
    /// iOS asked Andarium to free memory.
    case warning
    /// iOS may close Andarium.
    case critical

    public static func < (lhs: MemoryPressureLevel, rhs: MemoryPressureLevel) -> Bool {
        lhs.rawValue < rhs.rawValue
    }
}

/// A notice the session shows about the device's memory, over the guest. The
/// two kinds are one notice that says more when the level rises.
public enum MemoryPressureNotice: Hashable, Sendable {
    /// iOS asked Andarium to free memory, and the virtual machine may slow
    /// down for a moment.
    case low
    /// iOS may close Andarium.
    case critical

    /// The notice a level raises; none for the normal level.
    public init?(level: MemoryPressureLevel) {
        switch level {
        case .normal: return nil
        case .warning: self = .low
        case .critical: self = .critical
        }
    }
}

/// The session's memory notice: what the system reports, and whether the person
/// has put the notice away.
///
/// There is one showing for each pressure episode, from the level leaving
/// normal to its return:
///
/// - It is up only while a virtual machine session is on screen: not on Home,
///   not while the boot screen shows, not with Andarium in the background. An
///   episode that began or lasts out of sight is shown when the session comes
///   back.
/// - It appears at the first warning, stays while the pressure lasts and for
///   at least `minimumStay` from its appearance, and goes `lingerFor` after
///   the level returns to normal. A rise from warning to critical, or a fall
///   back, changes the words of the same notice; it never stacks another. A
///   level that rises again while the notice lingers continues the showing.
/// - A dismissal holds until the level has returned to normal and risen again:
///   further warnings in the episode show nothing, and a rise to critical after
///   a dismissed warning shows once more. A notice dismissed after the level
///   returned to normal has nothing to hold.
///
/// The moments come from the caller, so a test sets its own.
public struct MemoryPressureNoticeState: Equatable, Sendable {
    /// The least a notice stays from its appearance, so a warning that is over
    /// in a moment is still readable.
    public static let minimumStay = Duration.seconds(6)
    /// How long a notice stays after the level returned to normal.
    public static let lingerFor = Duration.seconds(3)

    /// The level the system last reported.
    public private(set) var level = MemoryPressureLevel.normal

    private var sessionOnScreen = false
    /// The highest level the person has put away in this episode.
    private var dismissed: MemoryPressureLevel?
    private var showingNow: Showing?

    /// A notice that is up.
    private struct Showing: Equatable, Sendable {
        /// When it appeared.
        var since: ContinuousClock.Instant
        /// The level it states: the last one that was not normal.
        var level: MemoryPressureLevel
        /// When the level returned to normal, while it has not risen again.
        var normalSince: ContinuousClock.Instant?
    }

    public init() {}

    /// The runtime reports a level the system raised, repeats and the return to
    /// normal included.
    public mutating func levelChanged(_ level: MemoryPressureLevel, at now: ContinuousClock.Instant) {
        expire(at: now)
        self.level = level
        if level == .normal { dismissed = nil }
        reconcile(at: now)
    }

    /// Whether a virtual machine session is on screen now.
    public mutating func screenChanged(onScreen: Bool, at now: ContinuousClock.Instant) {
        expire(at: now)
        sessionOnScreen = onScreen
        reconcile(at: now)
    }

    /// Takes away a notice that outstayed its welcome: the level returned to
    /// normal `lingerFor` ago and the notice has been up for `minimumStay`.
    public mutating func expire(at now: ContinuousClock.Instant) {
        guard let showing = showingNow, let normalSince = showing.normalSince else { return }
        if now >= normalSince + Self.lingerFor, now >= showing.since + Self.minimumStay {
            showingNow = nil
        }
    }

    /// How long from `now` the notice that is up goes without another report,
    /// or nil when it stays until one comes (the pressure lasts) or none is up.
    public func staysFor(at now: ContinuousClock.Instant) -> Duration? {
        guard let showing = showingNow, let normalSince = showing.normalSince else { return nil }
        let end = max(normalSince + Self.lingerFor, showing.since + Self.minimumStay)
        return max(.zero, now.duration(to: end))
    }

    /// The person puts the notice that is up away.
    public mutating func dismiss() {
        guard showingNow != nil else { return }
        if level != .normal { dismissed = level }
        showingNow = nil
    }

    /// The run ended: the next one starts clean.
    public mutating func runEnded() {
        self = MemoryPressureNoticeState()
    }

    /// The notice to show now.
    public var showing: MemoryPressureNotice? {
        showingNow.flatMap { MemoryPressureNotice(level: $0.level) }
    }

    private mutating func reconcile(at now: ContinuousClock.Instant) {
        let covered = dismissed.map { level <= $0 } ?? false
        let wanted = sessionOnScreen && level != .normal && !covered
        if wanted {
            if showingNow == nil {
                showingNow = Showing(since: now, level: level, normalSince: nil)
            } else {
                showingNow?.level = level
                showingNow?.normalSince = nil
            }
        } else if showingNow != nil {
            if !sessionOnScreen || covered {
                showingNow = nil
            } else if showingNow?.normalSince == nil {
                // The level is normal, since nothing else leaves a notice up
                // that is not wanted: it lingers.
                showingNow?.normalSince = now
            }
        }
    }
}
