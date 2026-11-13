// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// When the boot screen says that a start is taking long, and what the person
/// has asked of it since.
///
/// A start is timed from the moment the person (or a launch) asks for it, to
/// the boot that reaches Android or Recovery. A guest that restarts itself in
/// place while the start is unfinished is the same start, so a slow loop of
/// restarts reaches the notice before the runtime's guard ends it; a restart
/// of a session that was already running is a new boot, timed from its own
/// beginning. The notice never ends the boot: it offers Stop, and the boot
/// goes on behind it until the person stops it or it completes.
///
/// The type holds dates and nothing else: whoever owns it passes the time, so
/// a test needs no wait.
public struct SlowStart: Equatable, Sendable {
    /// How long one start runs before the boot screen says so.
    public static let threshold: TimeInterval = 180

    /// When the start under way began; nil when none is.
    public private(set) var began: Date?
    /// The notice is up.
    public private(set) var showing = false
    /// The person asked to stop the start, and the run has not ended yet.
    public private(set) var stopping = false
    /// How many times the guest restarted itself in this start.
    public private(set) var restarts = 0

    public init() {}

    /// A start begins: nothing has been said of it.
    public mutating func begin(at date: Date) {
        self = SlowStart()
        began = date
    }

    /// Android restarted the guest in place. The start continues when it was
    /// unfinished (`startUnfinished`); a running session's restart begins a
    /// boot of its own.
    public mutating func guestRestarted(at date: Date, startUnfinished: Bool) {
        if startUnfinished, began != nil {
            restarts += 1
        } else {
            begin(at: date)
        }
    }

    /// The boot reached Android or Recovery, or the run ended.
    public mutating func finish() {
        self = SlowStart()
    }

    /// Seconds into the start at `date`; zero before it began or when the
    /// clock went back.
    public func elapsedSeconds(at date: Date) -> Int {
        guard let began else { return 0 }
        return Int(max(date.timeIntervalSince(began), 0))
    }

    /// Looks at the clock. Returns true on the call that puts the notice up,
    /// once for each start.
    @discardableResult
    public mutating func update(at date: Date) -> Bool {
        guard began != nil, !showing,
            date.timeIntervalSince(began ?? date) >= Self.threshold
        else { return false }
        showing = true
        return true
    }

    /// The person chose Stop. Returns false when it was chosen before, which
    /// changes nothing.
    @discardableResult
    public mutating func requestStop() -> Bool {
        guard began != nil, !stopping else { return false }
        stopping = true
        return true
    }
}
