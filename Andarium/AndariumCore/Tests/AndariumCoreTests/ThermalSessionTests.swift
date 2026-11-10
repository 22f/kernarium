// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing

@testable import AndariumCore

/// The thermal session against a fake HAL, a clock moved by hand and a timer
/// the test fires: what the guest is told, when the cap is announced, and when
/// the attempts to connect stop.
struct ThermalSessionTests {
    private final class Timer: @unchecked Sendable {
        private let lock = NSLock()
        private var asked: [(nanoseconds: UInt64, work: @Sendable () -> Void)] = []
        var delays: [UInt64] { lock.withLock { asked.map(\.nanoseconds) } }
        func schedule(_ nanoseconds: UInt64, _ work: @escaping @Sendable () -> Void) {
            lock.withLock { asked.append((nanoseconds, work)) }
        }
        /// Runs the work the latest request asked for.
        func fireLatest() {
            let work = lock.withLock { asked.last?.work }
            work?()
        }
        /// Runs the work of the first request, which a newer one has replaced.
        func fireFirst() {
            let work = lock.withLock { asked.first?.work }
            work?()
        }
    }

    private final class Counter: @unchecked Sendable {
        private let lock = NSLock()
        private var value = 0
        func add() { lock.withLock { value += 1 } }
        var count: Int { lock.withLock { value } }
    }

    private struct Rig {
        var session: ThermalSession
        var guest: FakeThermalGuest
        var clock: ThermalTestClock
        var timer: Timer
        var caps: Counter
    }

    private func rig() -> Rig {
        let guest = FakeThermalGuest()
        let clock = ThermalTestClock()
        let timer = Timer()
        let caps = Counter()
        let session = ThermalSession(
            opener: guest, retryDelay: .milliseconds(10), clock: { clock.read() },
            schedule: { timer.schedule($0, $1) }, capStarted: { caps.add() })
        return Rig(session: session, guest: guest, clock: clock, timer: timer, caps: caps)
    }

    private func frame(_ name: String) throws -> [UInt8] { try ThermalFixture.bytes(name) }

    /// A rig in a run, connected, with the initial record read.
    private func connected(booted: Bool = true) throws -> Rig {
        let rig = rig()
        rig.session.update(running: true, booted: booted)
        #expect(thermalEventually { rig.session.isConnected })
        #expect(rig.guest.record() == (try frame("none")))
        return rig
    }

    // MARK: What the guest is told

    @Test func aRunConnectsAtOnceAndStartsWithNone() throws {
        let rig = rig()
        rig.session.update(running: true, booted: false)
        #expect(rig.guest.record() == (try frame("none")))
        #expect(rig.session.severity == .none)
    }

    @Test func eachChangeOfTheSeverityIsOneRecord() throws {
        let rig = try connected()
        rig.session.observe(.fair)
        #expect(rig.guest.record() == (try frame("light")))
        rig.session.observe(.serious)
        #expect(rig.guest.record() == (try frame("severe")))
        rig.session.observe(.serious)
        rig.session.observe(.critical)
        #expect(rig.guest.record() == (try frame("critical")))
        rig.session.settle()
        #expect(!rig.guest.hasPending(), "a repeated reading was sent")
        #expect(rig.session.severity == .critical)
    }

    @Test func aReadingFromBeforeTheRunIsTheStateItBeginsIn() throws {
        let rig = rig()
        rig.session.observe(.serious)
        rig.session.update(running: true, booted: false)
        #expect(rig.guest.record() == (try frame("severe")))
        #expect(thermalEventually { rig.caps.count == 1 })
    }

    @Test func theFallComesOnTheTimerAfterOneHundredTwentySeconds() throws {
        let rig = try connected()
        rig.session.observe(.serious)
        #expect(rig.guest.record() == (try frame("severe")))
        rig.clock.advance(seconds: 100)
        rig.session.observe(.nominal)
        rig.session.settle()
        #expect(rig.timer.delays.last == 120_000_000_000)
        #expect(!rig.guest.hasPending(), "the fall was not held")
        // The timer fires early by a second: the session waits for the rest.
        rig.clock.advance(seconds: 119)
        rig.timer.fireLatest()
        rig.session.settle()
        #expect(rig.session.severity == .severe)
        #expect(rig.timer.delays.last == 1_000_000_000)
        rig.clock.advance(seconds: 1)
        rig.timer.fireLatest()
        #expect(rig.guest.record() == (try frame("none")))
        #expect(rig.session.severity == .none)
    }

    @Test func aTimerRequestThatANewerOneReplacedChangesNothing() throws {
        let rig = try connected()
        rig.session.observe(.serious)
        rig.session.observe(.nominal)
        rig.session.settle()
        rig.session.observe(.serious)
        rig.session.observe(.nominal)
        rig.session.settle()
        #expect(rig.timer.delays.count == 2, "each fall asks for a timer")
        rig.clock.advance(seconds: 60)
        rig.timer.fireFirst()
        rig.session.settle()
        #expect(rig.session.severity == .severe, "an old timer lowered the severity")
    }

    @Test func aReadingOnReturningToTheForegroundFindsTheFallDue() throws {
        let rig = try connected()
        rig.session.observe(.serious)
        _ = rig.guest.record()
        rig.session.observe(.nominal)
        rig.session.settle()
        // The application was away for longer than the wait, and the timer
        // could not run.
        rig.clock.advance(seconds: 600)
        rig.session.observe(.nominal)
        #expect(rig.guest.record() == (try frame("none")))
    }

    @Test func aLostConnectionIsMadeAgainWithTheCurrentSeverity() throws {
        let rig = try connected()
        rig.session.observe(.serious)
        _ = rig.guest.record()
        rig.guest.dropLatest()
        #expect(thermalEventually { rig.guest.connections == 2 && rig.session.isConnected })
        #expect(rig.guest.record() == (try frame("severe")))
    }

    @Test func aGuestThatRestartsItselfIsToldAgainAndTheCapIsNotAnnouncedAgain() throws {
        let rig = try connected()
        rig.session.observe(.serious)
        _ = rig.guest.record()
        #expect(thermalEventually { rig.caps.count == 1 })
        rig.session.restart()
        rig.session.update(running: true, booted: false)
        #expect(thermalEventually { rig.guest.connections == 2 && rig.session.isConnected })
        #expect(rig.guest.record() == (try frame("severe")))
        rig.session.settle()
        #expect(rig.caps.count == 1)
    }

    // MARK: The announcement

    @Test func theCapIsAnnouncedOnceWhenItStarts() throws {
        let rig = try connected()
        rig.session.observe(.fair)
        rig.session.settle()
        #expect(rig.caps.count == 0, "announced at light")
        rig.session.observe(.serious)
        #expect(thermalEventually { rig.caps.count == 1 })
        rig.session.observe(.critical)
        rig.session.settle()
        #expect(rig.caps.count == 1)
    }

    @Test func theCapIsNotAnnouncedAgainAfterCoolingAndHeatingAgain() throws {
        let rig = try connected()
        rig.session.observe(.serious)
        #expect(thermalEventually { rig.caps.count == 1 })
        rig.session.observe(.nominal)
        rig.session.settle()
        rig.clock.advance(seconds: 130)
        rig.timer.fireLatest()
        rig.session.settle()
        #expect(rig.session.severity == .none)
        rig.session.observe(.serious)
        rig.session.settle()
        #expect(rig.session.severity == .severe)
        #expect(rig.caps.count == 1, "announced twice in a run")
    }

    @Test func aRunThatNeverReachesSevereAnnouncesNothing() throws {
        let rig = try connected()
        rig.session.observe(.fair)
        rig.session.observe(.nominal)
        rig.session.observe(.fair)
        rig.session.settle()
        rig.clock.advance(seconds: 300)
        rig.timer.fireLatest()
        rig.session.settle()
        #expect(rig.caps.count == 0)
    }

    @Test func aNewRunAnnouncesAgain() throws {
        let rig = try connected()
        rig.session.observe(.serious)
        #expect(thermalEventually { rig.caps.count == 1 })
        rig.session.update(running: false, booted: false)
        rig.session.settle()
        #expect(rig.session.severity == .none, "a run's severity outlived it")
        rig.session.update(running: true, booted: false)
        #expect(thermalEventually { rig.guest.connections == 2 && rig.session.isConnected })
        #expect(rig.guest.record() == (try frame("severe")), "the state read before the run is the state it begins in")
        #expect(thermalEventually { rig.caps.count == 2 })
    }

    @Test func nothingIsAnnouncedWhileTheGuestHasNoHAL() throws {
        let rig = rig()
        rig.guest.accepting = false
        rig.session.update(running: true, booted: false)
        rig.session.observe(.serious)
        usleep(100_000)
        rig.session.settle()
        #expect(rig.caps.count == 0, "the cap was announced with no HAL to apply it")
        // The HAL starts late in boot: the cap is announced when it connects.
        rig.guest.accepting = true
        #expect(thermalEventually { rig.caps.count == 1 })
        #expect(rig.guest.record() == (try frame("severe")))
    }

    // MARK: Giving up

    @Test func fiveAttemptsThatFailAfterBootEndTheAttemptsForThatBoot() {
        let rig = rig()
        rig.guest.accepting = false
        rig.session.update(running: true, booted: false)
        #expect(thermalEventually { rig.guest.attempts >= 4 })
        #expect(!rig.session.hasGivenUp, "gave up before Android booted")
        rig.session.update(running: true, booted: true)
        #expect(thermalEventually { rig.session.hasGivenUp })
        rig.session.observe(.serious)
        rig.session.settle()
        let attempts = rig.guest.attempts
        usleep(100_000)
        #expect(rig.guest.attempts == attempts, "tried again after giving up")
        #expect(rig.caps.count == 0)
        #expect(rig.session.severity == .severe, "the policy stopped with the connection")
    }

    @Test func aRestartBeginsTheAttemptsAgain() throws {
        let rig = rig()
        rig.guest.accepting = false
        rig.session.update(running: true, booted: true)
        #expect(thermalEventually { rig.session.hasGivenUp })
        rig.guest.accepting = true
        rig.session.restart()
        rig.session.update(running: true, booted: false)
        #expect(thermalEventually { rig.session.isConnected })
        #expect(rig.guest.record() == (try frame("none")))
    }

    @Test func recoveryNeverConnects() {
        let rig = rig()
        rig.session.update(running: false, booted: true)
        rig.session.observe(.critical)
        usleep(100_000)
        rig.session.settle()
        #expect(rig.guest.attempts == 0)
        #expect(rig.caps.count == 0)
    }

    @Test func endingTheRunClosesTheConnection() throws {
        let rig = try connected()
        rig.session.update(running: false, booted: false)
        rig.session.settle()
        #expect(!rig.session.isConnected)
        let peer = try #require(rig.guest.latest)
        var descriptor = pollfd(fd: peer, events: Int16(POLLIN), revents: 0)
        #expect(poll(&descriptor, 1, 3000) > 0)
        var byte: UInt8 = 0
        #expect(recv(peer, &byte, 1, MSG_DONTWAIT) == 0)
    }
}
