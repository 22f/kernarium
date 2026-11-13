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

    // MARK: The guest's write-back before the stop

    /// What a delivery did, in order.
    private final class Calls: @unchecked Sendable {
        private let lock = NSLock()
        private var calls: [String] = []
        func add(_ call: String) { lock.withLock { calls.append(call) } }
        var all: [String] { lock.withLock { calls } }
    }

    @Test func theGuestIsAskedToWriteBackOnceBeforeTheFirstStop() {
        let stop = RunStop()
        let calls = Calls()
        #expect(stop.enter())
        stop.request()
        for _ in 0..<3 {
            #expect(stop.deliver(settle: { calls.add("settle") }) { calls.add("stop") })
        }
        // The runtime drops a stop that arrives early, so the stop is made
        // again and again; the guest is asked once.
        #expect(calls.all == ["settle", "stop", "stop", "stop"])
    }

    @Test func nothingIsAskedOfAGuestWhoseRunWasNeverEnteredOrHasEnded() {
        let calls = Calls()
        let early = RunStop()
        early.request()
        #expect(!early.enter())
        #expect(early.deliver(settle: { calls.add("settle") }) { calls.add("stop") })
        early.end()
        #expect(!early.deliver(settle: { calls.add("settle") }) { calls.add("stop") })

        // Entered but not stopped: nothing yet; stopped and then ended: nothing after.
        let late = RunStop()
        #expect(late.enter())
        #expect(late.deliver(settle: { calls.add("settle") }) { calls.add("stop") })
        late.end()
        late.request()
        #expect(!late.deliver(settle: { calls.add("settle") }) { calls.add("stop") })
        #expect(calls.all.isEmpty)
    }

    @Test func aStopWithoutAWriteBackIsAsBeforeAndDoesNotUseUpTheOne() {
        let stop = RunStop()
        let calls = Calls()
        #expect(stop.enter())
        stop.request()
        #expect(stop.deliver { calls.add("stop") })
        #expect(stop.deliver(settle: { calls.add("settle") }) { calls.add("stop") })
        #expect(calls.all == ["stop", "settle", "stop"])
    }

    @Test func endingWaitsForTheWriteBackToo() {
        let stop = RunStop()
        #expect(stop.enter())
        stop.request()
        let inside = DispatchSemaphore(value: 0)
        let release = DispatchSemaphore(value: 0)
        let ended = DispatchSemaphore(value: 0)
        let calls = Calls()
        Thread {
            _ = stop.deliver(settle: {
                inside.signal()
                release.wait()
                calls.add("settle")
            }) { calls.add("stop") }
        }.start()
        inside.wait()
        Thread {
            stop.end()
            ended.signal()
        }.start()
        Thread.sleep(forTimeInterval: 0.1)
        #expect(ended.wait(timeout: .now()) == .timedOut)
        release.signal()
        #expect(ended.wait(timeout: .now() + 10) == .success)
        #expect(calls.all == ["settle", "stop"], "the run ended inside the stop's own write-back")
    }

    @Test func theWriteBackIsBoundedByTheEdgeBudget() {
        #expect(StopSettle.bound == PauseController.edgeBudget)
    }

    @Test func aWriteBackThatFinishesReturnsAtOnce() {
        let ran = ResultBox()
        let before = ContinuousClock.now
        #expect(StopSettle.run(within: .seconds(30)) { ran.set() })
        #expect(ran.value)
        #expect(ContinuousClock.now - before < .seconds(5))
    }

    @Test func aWriteBackThatNeverReturnsIsLeftAtTheBound() {
        let release = DispatchSemaphore(value: 0)
        let before = ContinuousClock.now
        let finished = StopSettle.run(within: .milliseconds(200)) { release.wait() }
        let took = ContinuousClock.now - before
        #expect(!finished)
        #expect(took >= .milliseconds(150) && took < .seconds(5), "the wait was \(took)")
        release.signal()
    }

    private final class ResultBox: @unchecked Sendable {
        private let lock = NSLock()
        private var done = false
        func set() { lock.withLock { done = true } }
        var value: Bool { lock.withLock { done } }
    }
}
