// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// How short of memory iOS says the device is, as the runtime reports the
/// levels of the system's memory pressure.
public enum MemoryPressureLevel: Int, Comparable, Sendable {
    case normal = 0
    /// iOS asked Andarium to free memory; the runtime answers it itself.
    case warning
    /// iOS may close Andarium.
    case critical

    public static func < (lhs: MemoryPressureLevel, rhs: MemoryPressureLevel) -> Bool {
        lhs.rawValue < rhs.rawValue
    }
}

/// The session's memory notice: what the system reports, and whether the person
/// has put the notice away. Only the critical level has a notice; the warning
/// level is answered by the runtime and has none, but the state follows it, for
/// a notice that is up ends when the level leaves critical.
///
/// There is one showing for each critical episode, from the level reaching
/// critical to its leaving it:
///
/// - It is up only while a guest session is on screen: not on Home, not while
///   the boot screen shows, not with Andarium in the background. An episode
///   that began out of sight shows when the session comes back only if the
///   level is still critical then.
/// - It appears when the level reaches critical, and stays while it is critical
///   and for at least `minimumStay` from its appearance. When the level leaves
///   critical, for warning or for normal, the notice keeps its words and goes
///   `lingerFor` later, or when the minimum stay has passed, whichever is later.
///   A level that reaches critical again meanwhile continues the same showing.
/// - A dismissal holds until the level has left critical and reached it again:
///   repeated critical reports show nothing. A notice dismissed after the level
///   left critical has nothing to hold.
/// - The end of a run clears everything.
///
/// The moments come from the caller, so a test sets its own.
public struct MemoryPressureNoticeState: Equatable, Sendable {
    /// The least a notice stays from its appearance, so a level that leaves
    /// critical in a moment is still readable.
    public static let minimumStay = Duration.seconds(6)
    /// How long a notice stays after the level left critical.
    public static let lingerFor = Duration.seconds(3)

    /// The level the system last reported.
    public private(set) var level = MemoryPressureLevel.normal

    private var sessionOnScreen = false
    /// The person put the notice away in this critical episode.
    private var dismissed = false
    private var showingNow: Showing?

    /// A notice that is up.
    private struct Showing: Equatable, Sendable {
        /// When it appeared.
        var since: ContinuousClock.Instant
        /// When the level left critical, while it has not reached it again.
        var endingSince: ContinuousClock.Instant?
    }

    public init() {}

    /// The runtime reports a level the system raised, repeats and the return to
    /// normal included.
    public mutating func levelChanged(_ level: MemoryPressureLevel, at now: ContinuousClock.Instant) {
        expire(at: now)
        self.level = level
        if level != .critical { dismissed = false }
        reconcile(at: now)
    }

    /// Whether a guest session is on screen now.
    public mutating func screenChanged(onScreen: Bool, at now: ContinuousClock.Instant) {
        expire(at: now)
        sessionOnScreen = onScreen
        reconcile(at: now)
    }

    /// Takes away a notice that outstayed its welcome: the level left critical
    /// `lingerFor` ago and the notice has been up for `minimumStay`.
    public mutating func expire(at now: ContinuousClock.Instant) {
        guard let showing = showingNow, let end = end(of: showing) else { return }
        if now >= end { showingNow = nil }
    }

    /// How long from `now` the notice that is up goes without another report,
    /// or nil when it stays until one comes (the level is critical) or none is
    /// up.
    public func staysFor(at now: ContinuousClock.Instant) -> Duration? {
        guard let showing = showingNow, let end = end(of: showing) else { return nil }
        return max(.zero, now.duration(to: end))
    }

    /// The person puts the notice that is up away.
    public mutating func dismiss() {
        guard showingNow != nil else { return }
        if level == .critical { dismissed = true }
        showingNow = nil
    }

    /// The run ended: the next one starts clean.
    public mutating func runEnded() {
        self = MemoryPressureNoticeState()
    }

    /// Whether the critical notice is up now.
    public var isShowing: Bool { showingNow != nil }

    /// When a notice that is on its way out goes.
    private func end(of showing: Showing) -> ContinuousClock.Instant? {
        guard let endingSince = showing.endingSince else { return nil }
        return max(endingSince + Self.lingerFor, showing.since + Self.minimumStay)
    }

    private mutating func reconcile(at now: ContinuousClock.Instant) {
        let wanted = sessionOnScreen && level == .critical && !dismissed
        if wanted {
            if showingNow == nil {
                showingNow = Showing(since: now, endingSince: nil)
            } else {
                showingNow?.endingSince = nil
            }
        } else if showingNow != nil {
            if !sessionOnScreen || dismissed {
                showingNow = nil
            } else if showingNow?.endingSince == nil {
                // The level left critical, since nothing else leaves a notice up
                // that is not wanted: it lingers.
                showingNow?.endingSince = now
            }
        }
    }
}
