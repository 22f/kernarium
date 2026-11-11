// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// The sweep of the partial files that earlier runs left in the guest: once
/// per run, after the storage is up, off the main thread, one console line at
/// most, and not again while the guest stays up. The guest's own end of it is
/// `GuestFilesGuestTests`; here the guest is a set of closures.
@Suite(.serialized, .timeLimit(.minutes(2)))
struct PartialFileSweepTests {
    /// What the sweep asked of the guest, in order, and what it printed.
    private nonisolated final class Guest: @unchecked Sendable {
        private let lock = NSLock()
        private var readings: [Bool?]
        private var sweepBody: @Sendable () async throws -> Int
        private var reads = 0
        private var sweeps = 0
        private var waits: [Duration] = []
        private var lines: [String] = []
        private var main = false

        var readCount: Int { lock.withLock { reads } }
        var sweepCount: Int { lock.withLock { sweeps } }
        var pauses: [Duration] { lock.withLock { waits } }
        var printed: [String] { lock.withLock { lines } }
        var ranOnMain: Bool { lock.withLock { main } }

        init(readings: [Bool?] = [true], sweep: @escaping @Sendable () async throws -> Int = { 0 }) {
            self.readings = readings
            sweepBody = sweep
        }

        func setSweep(_ body: @escaping @Sendable () async throws -> Int) { lock.withLock { sweepBody = body } }

        func read() -> Bool? {
            lock.withLock {
                if Thread.isMainThread { main = true }
                reads += 1
                return readings.count > 1 ? readings.removeFirst() : readings.first ?? nil
            }
        }

        func sweep() async throws -> Int {
            let body = lock.withLock { () -> @Sendable () async throws -> Int in
                if Thread.isMainThread { main = true }
                sweeps += 1
                return sweepBody
            }
            return try await body()
        }

        func pause(_ duration: Duration) async throws {
            lock.withLock { waits.append(duration) }
            try Task.checkCancellation()
            await Task.yield()
        }

        func print(_ text: String) { lock.withLock { lines.append(text) } }

        func sweeper() -> PartialFileSweep {
            PartialFileSweep(
                storageReady: { self.read() }, sweep: { try await self.sweep() },
                pause: { try await self.pause($0) }, report: { self.print($0) })
        }
    }

    /// Waits, up to a bound, for `condition`.
    private func eventually(_ what: String, _ condition: () -> Bool) async throws {
        let deadline = ContinuousClock.now + .seconds(20)
        while ContinuousClock.now < deadline {
            if condition() { return }
            try await Task.sleep(for: .milliseconds(5))
        }
        Issue.record("timed out waiting for \(what)")
        throw CancellationError()
    }

    @Test func theFirstBootOfARunSweepsOnceAndLaterBootsDoNothing() async throws {
        let guest = Guest(sweep: { 3 })
        let sweep = guest.sweeper()
        #expect(sweep.currentPhase == .waiting && sweep.attempts == 0)
        // Android's boot line can come more than once, and from any thread.
        await withTaskGroup(of: Void.self) { group in
            for _ in 0..<8 { group.addTask { sweep.guestBooted() } }
        }
        try await eventually("the sweep settles") { sweep.currentPhase == .settled }
        #expect(guest.sweepCount == 1 && sweep.attempts == 1)
        #expect(guest.printed == ["Andarium: removed 3 partial files that earlier imports left"])
        sweep.guestBooted()
        sweep.guestBooted()
        try await Task.sleep(for: .milliseconds(50))
        #expect(guest.sweepCount == 1 && sweep.attempts == 1 && guest.printed.count == 1)
        // A restart after it finished leaves it finished.
        sweep.guestRestarting()
        sweep.guestBooted()
        try await Task.sleep(for: .milliseconds(50))
        #expect(guest.sweepCount == 1 && sweep.currentPhase == .settled)
    }

    @Test func aSweepThatFindsNothingSaysNothing() async throws {
        let guest = Guest(sweep: { 0 })
        let sweep = guest.sweeper()
        sweep.guestBooted()
        try await eventually("the sweep settles") { sweep.currentPhase == .settled }
        #expect(guest.sweepCount == 1 && guest.printed.isEmpty)
        let one = Guest(sweep: { 1 })
        let single = one.sweeper()
        single.guestBooted()
        try await eventually("the sweep settles") { single.currentPhase == .settled }
        #expect(one.printed == ["Andarium: removed 1 partial file that earlier imports left"])
    }

    @Test func nothingIsSweptBeforeTheStorageIsUp() async throws {
        // Android reports its boot done before the storage is mounted, and a walk
        // of the empty folder there would find nothing and settle.
        let guest = Guest(readings: [nil, false, false, nil, true], sweep: { 2 })
        let sweep = guest.sweeper()
        sweep.guestBooted()
        try await eventually("the sweep settles") { sweep.currentPhase == .settled }
        #expect(guest.readCount == 5 && guest.sweepCount == 1)
        #expect(guest.pauses == Array(repeating: PartialFileSweep.storageInterval, count: 4))
        #expect(guest.printed.count == 1)
    }

    @Test func aStorageThatNeverComesUpEndsAsOneLineAndIsNotTriedAgain() async throws {
        let guest = Guest(readings: [false])
        let sweep = guest.sweeper()
        sweep.guestBooted()
        try await eventually("the attempt settles") { sweep.currentPhase == .settled }
        #expect(guest.sweepCount == 0)
        #expect(guest.readCount == PartialFileSweep.storagePolls)
        #expect(guest.pauses.count == PartialFileSweep.storagePolls - 1)
        #expect(guest.printed.count == 1 && guest.printed[0].hasPrefix("Andarium: could not remove the partial files"))
        sweep.guestBooted()
        sweep.guestRestarting()
        sweep.guestBooted()
        try await Task.sleep(for: .milliseconds(50))
        #expect(guest.readCount == PartialFileSweep.storagePolls && sweep.attempts == 1 && guest.printed.count == 1)
    }

    @Test func aFailedSweepIsOneLineAndNotATightLoop() async throws {
        struct Refused: Error, CustomStringConvertible { var description: String { "the guest said no" } }
        let guest = Guest(sweep: { throw Refused() })
        let sweep = guest.sweeper()
        sweep.guestBooted()
        try await eventually("the sweep settles") { sweep.currentPhase == .settled }
        #expect(guest.printed == ["Andarium: could not remove the partial files that earlier imports left: the guest said no"])
        sweep.guestBooted()
        try await Task.sleep(for: .milliseconds(100))
        #expect(guest.sweepCount == 1 && sweep.attempts == 1 && guest.printed.count == 1)
    }

    @Test func aRestartThatCutsTheSweepOffGivesTheNextBootOneMoreAttempt() async throws {
        let guest = Guest(sweep: {
            // The guest's sweep, running for long: ended by the task's cancellation.
            try await Task.sleep(for: .seconds(60))
            return 99
        })
        let sweep = guest.sweeper()
        sweep.guestBooted()
        try await eventually("the first attempt is removing") { guest.sweepCount == 1 }
        #expect(sweep.currentPhase == .running)
        sweep.guestRestarting()
        #expect(sweep.currentPhase == .waiting)
        // What the cut-off attempt does as it ends neither prints nor settles.
        try await Task.sleep(for: .milliseconds(100))
        #expect(sweep.currentPhase == .waiting && guest.printed.isEmpty)
        guest.setSweep { 4 }
        sweep.guestBooted()
        try await eventually("the second attempt settles") { sweep.currentPhase == .settled }
        #expect(guest.sweepCount == 2 && sweep.attempts == 2)
        #expect(guest.printed == ["Andarium: removed 4 partial files that earlier imports left"])
    }

    @Test func aRestartBeforeAnythingBootedChangesNothing() async throws {
        let guest = Guest()
        let sweep = guest.sweeper()
        sweep.guestRestarting()
        #expect(sweep.currentPhase == .waiting && sweep.attempts == 0)
        sweep.guestBooted()
        try await eventually("the sweep settles") { sweep.currentPhase == .settled }
        #expect(guest.sweepCount == 1)
    }

    @Test func theEndOfTheRunEndsTheSweepAndNothingStartsAfterIt() async throws {
        let guest = Guest(sweep: {
            try await Task.sleep(for: .seconds(60))
            return 99
        })
        let sweep = guest.sweeper()
        sweep.guestBooted()
        try await eventually("the sweep is removing") { guest.sweepCount == 1 }
        sweep.runEnded()
        #expect(sweep.currentPhase == .settled)
        try await Task.sleep(for: .milliseconds(100))
        #expect(guest.printed.isEmpty)
        sweep.guestBooted()
        try await Task.sleep(for: .milliseconds(50))
        #expect(guest.sweepCount == 1 && sweep.attempts == 1)
        // A run that ended before its first boot sweeps nothing.
        let early = Guest()
        let never = early.sweeper()
        never.runEnded()
        never.guestBooted()
        try await Task.sleep(for: .milliseconds(50))
        #expect(early.readCount == 0 && early.sweepCount == 0 && never.attempts == 0)
    }

    @Test func theWorkIsNeverOnTheMainThreadAndTheCallerIsNotHeldUp() async throws {
        let guest = Guest(readings: [false, true], sweep: {
            try await Task.sleep(for: .milliseconds(200))
            return 1
        })
        let sweep = guest.sweeper()
        let began = ContinuousClock.now
        sweep.guestBooted()
        // The call returns at once: the boot's event is not held for the sweep.
        #expect(ContinuousClock.now - began < .milliseconds(100))
        #expect(sweep.currentPhase == .running)
        try await eventually("the sweep settles") { sweep.currentPhase == .settled }
        #expect(!guest.ranOnMain)
    }

    @Test func eachRunHasItsOwnMarkAndItsOwnSweep() {
        let first = GuestRunMark.random()
        let second = GuestRunMark.random()
        #expect(first != second)
        let one = Guest().sweeper()
        let two = Guest().sweeper()
        one.guestBooted()
        #expect(two.attempts == 0 && two.currentPhase == .waiting)
        one.runEnded()
    }
}
