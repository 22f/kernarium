// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The person's stop of a run across the hand-over to the runtime's thread:
/// kept until the run ends, delivered after the thread entered the runtime.
struct RunStopTests {
    @Test func aStopIsAskedForOnce() {
        let stop = RunStop()
        #expect(!stop.isRequested)
        #expect(stop.request() && stop.isRequested)
        #expect(!stop.request())
    }

    @Test func aStopBeforeTheThreadEntersKeepsItOutOfTheRuntime() {
        let stop = RunStop()
        stop.request()
        #expect(!stop.enter(), "the thread entered the runtime after the stop")
        var asked = 0
        // Nothing is delivered to a runtime that was never entered; the
        // delivery goes on until the run ends.
        #expect(stop.deliver { asked += 1 } && asked == 0)
        stop.end()
        #expect(!stop.deliver { asked += 1 } && asked == 0)
    }

    @Test func aStopAfterTheThreadEnteredIsDeliveredAgainAndAgainUntilTheRunEnds() {
        let stop = RunStop()
        #expect(stop.enter())
        var asked = 0
        // Nothing asked for yet: nothing delivered.
        #expect(stop.deliver { asked += 1 } && asked == 0)
        stop.request()
        // The runtime drops a stop made before its run begins, so the same one
        // is delivered once more, as often as the owner schedules it.
        #expect(stop.deliver { asked += 1 } && asked == 1)
        #expect(stop.deliver { asked += 1 } && asked == 2)
        stop.end()
        #expect(!stop.deliver { asked += 1 } && asked == 2, "a run that ended was asked to stop")
    }

    @Test func endingWaitsForADeliveryUnderWay() {
        let stop = RunStop()
        #expect(stop.enter())
        stop.request()
        let inside = DispatchSemaphore(value: 0)
        let release = DispatchSemaphore(value: 0)
        let ended = DispatchSemaphore(value: 0)
        let delivered = ResultBox()
        let endedWith = ResultBox()
        Thread {
            _ = stop.deliver {
                inside.signal()
                release.wait()
                delivered.set()
            }
        }.start()
        inside.wait()
        Thread {
            stop.end()
            if delivered.value { endedWith.set() }
            ended.signal()
        }.start()
        // The end cannot return while the request is still being made.
        Thread.sleep(forTimeInterval: 0.1)
        #expect(ended.wait(timeout: .now()) == .timedOut)
        release.signal()
        #expect(ended.wait(timeout: .now() + 10) == .success)
        #expect(endedWith.value, "the run ended with a request still on its way to the runtime")
    }

    private final class ResultBox: @unchecked Sendable {
        private let lock = NSLock()
        private var done = false
        func set() { lock.withLock { done = true } }
        var value: Bool { lock.withLock { done } }
    }
}
