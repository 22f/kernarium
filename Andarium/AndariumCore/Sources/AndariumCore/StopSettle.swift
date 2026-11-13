// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What the person's Stop does with the guest before the host stops it: asks
/// it to write its storage back and waits for its answer, for as long as a
/// lifecycle edge's work may take (`PauseController.edgeBudget`) and no
/// longer. A guest that cannot answer (no console yet, a runtime that is not
/// there, a guest that does not reply) costs the bound at most, and the host's
/// stop follows it either way.
public enum StopSettle {
    /// How long the stop waits for the guest.
    public static let bound = PauseController.edgeBudget

    /// Runs `work`, the runtime's write-back, on a thread of its own and waits
    /// for it at most `bound`. Returns whether it finished in time. A call
    /// that outlasts the bound is left to end by itself: the caller goes on
    /// without it.
    @discardableResult
    public static func run(within bound: DispatchTimeInterval = bound, _ work: @escaping @Sendable () -> Void) -> Bool {
        let finished = DispatchSemaphore(value: 0)
        DispatchQueue.global(qos: .userInitiated).async {
            work()
            finished.signal()
        }
        return finished.wait(timeout: .now() + bound) == .success
    }
}
