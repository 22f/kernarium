// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The person's stop of one run, across the hand-over from the application to
/// the runtime's own thread.
///
/// The runtime drops a stop asked for before its run begins, which is some
/// time after the thread that runs it has started (it prepares the guest's
/// disk first). Without this a Stop in that gap is lost and the guest boots.
/// The latch keeps the request for the whole run: the thread asks it before it
/// enters the runtime (`enter`), and the request is delivered, again and again,
/// until the run has ended (`deliver`), so one that landed before the runtime
/// looked is made again after it did.
///
/// The type holds state and nothing else: whoever owns it runs the delivery,
/// off the main actor, because the request may wait on the runtime.
public final class RunStop: @unchecked Sendable {
    private let condition = NSCondition()
    private var requested = false
    private var entered = false
    private var ended = false
    private var delivering = false

    public init() {}

    /// The person chose Stop. Returns false when it was chosen before, which
    /// changes nothing.
    @discardableResult
    public func request() -> Bool {
        condition.withLock {
            guard !requested else { return false }
            requested = true
            return true
        }
    }

    public var isRequested: Bool {
        condition.withLock { requested }
    }

    /// The run's thread is about to enter the runtime. Returns false when the
    /// person stopped first: the thread must not enter, and must `end`.
    public func enter() -> Bool {
        condition.withLock {
            guard !requested else { return false }
            entered = true
            return true
        }
    }

    /// Runs `request` when a stop was asked for, the thread has entered the
    /// runtime and the run has not ended: the delivery of a stop, which the
    /// runtime may wait on. Returns whether to deliver again later (the run
    /// has not ended); a run that has ended is never asked anything.
    public func deliver(_ request: () -> Void) -> Bool {
        let wanted: Bool? = condition.withLock {
            guard !ended else { return nil }
            guard requested, entered else { return false }
            delivering = true
            return true
        }
        guard let wanted else { return false }
        if wanted {
            request()
            condition.withLock {
                delivering = false
                condition.broadcast()
            }
        }
        return true
    }

    /// The run's thread is done with the runtime, however it ended. Waits for
    /// a delivery under way, so no request reaches a later run.
    public func end() {
        condition.withLock {
            ended = true
            while delivering { condition.wait() }
        }
    }
}
