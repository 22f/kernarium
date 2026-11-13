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
/// The first delivery of a run also carries the guest's write-back (`settle`):
/// the guest is asked once, before the host stops it, and the host's stop
/// follows it whatever it came to.
///
/// The type holds state and nothing else: whoever owns it runs the delivery,
/// off the main actor, because the request may wait on the runtime.
public final class RunStop: @unchecked Sendable {
    private let condition = NSCondition()
    private var requested = false
    private var entered = false
    private var ended = false
    private var delivering = false
    private var settleAsked = false

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
    ///
    /// `settle`, when there is one, runs first and once in the run's whole
    /// delivery, however many deliveries follow, and only where the thread has
    /// entered the runtime: a run that never did has no guest to write back.
    /// It is bounded by whoever supplies it (`StopSettle`), and `end` waits for
    /// it as for the request.
    public func deliver(settle: (() -> Void)? = nil, _ request: () -> Void) -> Bool {
        enum Step {
            case ended
            case wait
            case deliver(settling: Bool)
        }
        let step: Step = condition.withLock {
            guard !ended else { return .ended }
            guard requested, entered else { return .wait }
            delivering = true
            let settling = settle != nil && !settleAsked
            if settling { settleAsked = true }
            return .deliver(settling: settling)
        }
        switch step {
        case .ended:
            return false
        case .wait:
            return true
        case let .deliver(settling):
            if settling { settle?() }
            request()
            condition.withLock {
                delivering = false
                condition.broadcast()
            }
            return true
        }
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
