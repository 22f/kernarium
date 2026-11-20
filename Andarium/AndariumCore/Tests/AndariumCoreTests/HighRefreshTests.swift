// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// Settings' "120 Hz": what the setting is offered on, what the boot asks for,
/// how the choice is kept, and what the thermal session tells Android while
/// Low Power Mode holds the 120 Hz display to 60.
struct HighRefreshPolicyTests {
    @Test func theSettingIsOfferedOnAScreenThatShowsMoreThanSixty() {
        #expect(!HighRefresh.isOffered(maximumFramesPerSecond: 60))
        #expect(!HighRefresh.isOffered(maximumFramesPerSecond: 0))
        #expect(HighRefresh.isOffered(maximumFramesPerSecond: 120))
        #expect(HighRefresh.isOffered(maximumFramesPerSecond: 90))
    }

    @Test func theBootAsksOnlyWhenTheSettingIsOnAndTheScreenCanShowIt() {
        #expect(HighRefresh.asksAtBoot(isOn: true, maximumFramesPerSecond: 120))
        #expect(!HighRefresh.asksAtBoot(isOn: false, maximumFramesPerSecond: 120))
        // A device that cannot never asks, whatever was stored or stated.
        #expect(!HighRefresh.asksAtBoot(isOn: true, maximumFramesPerSecond: 60))
        #expect(!HighRefresh.asksAtBoot(isOn: false, maximumFramesPerSecond: 60))
    }

    @Test func lowPowerModeHoldsOnlyTheHundredAndTwentyHertzDisplay() {
        #expect(HighRefresh.holdsAtSixty(displayIs120: true, lowPowerMode: true))
        #expect(!HighRefresh.holdsAtSixty(displayIs120: true, lowPowerMode: false))
        #expect(!HighRefresh.holdsAtSixty(displayIs120: false, lowPowerMode: true))
        #expect(!HighRefresh.holdsAtSixty(displayIs120: false, lowPowerMode: false))
    }

    @Test func theWordsAreFixedEnglish() {
        #expect(HighRefresh.state(isOn: true) == "on")
        #expect(HighRefresh.state(isOn: false) == "off")
    }
}

struct HighRefreshPreferenceTests {
    private func withDefaults<Result>(_ body: (UserDefaults) -> Result) -> Result {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-high-refresh-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let name = directory.appendingPathComponent("defaults").path
        let defaults = UserDefaults(suiteName: name)!
        defer {
            defaults.removePersistentDomain(forName: name)
            try? FileManager.default.removeItem(at: directory)
        }
        return body(defaults)
    }

    @Test func aFreshInstallIsOff() {
        withDefaults { defaults in
            let preference = HighRefreshPreference(defaults: defaults)
            #expect(!preference.isOn)
            let start = preference.launch(automated: false)
            #expect(!start.isOn)
            #expect(start.keepsChanges)
        }
    }

    @Test func theChoiceSurvivesANewSessionUntilTurnedBackOff() {
        withDefaults { defaults in
            HighRefreshPreference(defaults: defaults).keep(true)
            let again = HighRefreshPreference(defaults: defaults)
            #expect(again.isOn)
            let person = again.launch(automated: false)
            #expect(person.isOn && person.keepsChanges)
            again.keep(false)
            #expect(!HighRefreshPreference(defaults: defaults).isOn)
        }
    }

    @Test func aHarnessLaunchRunsWithTheDefaultAndLeavesTheChoiceAlone() {
        withDefaults { defaults in
            let preference = HighRefreshPreference(defaults: defaults)
            preference.keep(true)
            let harness = preference.launch(automated: true)
            #expect(!harness.isOn && !harness.keepsChanges)
            let stated = preference.launch(automated: true, stated: true)
            #expect(stated.isOn && !stated.keepsChanges)
            let statedOff = preference.launch(automated: false, stated: false)
            #expect(!statedOff.isOn && !statedOff.keepsChanges)
            #expect(preference.isOn, "a launch changed what the person chose")
        }
    }
}

struct SixtyHertzHoldTests {
    private final class Timer: @unchecked Sendable {
        private let lock = NSLock()
        private var asked: [@Sendable () -> Void] = []
        func schedule(_ work: @escaping @Sendable () -> Void) { lock.withLock { asked.append(work) } }
        func fireLatest() {
            let work = lock.withLock { asked.last }
            work?()
        }
    }

    private final class Log: @unchecked Sendable {
        private let lock = NSLock()
        private var caps = 0
        func cap() { lock.withLock { caps += 1 } }
        var capCount: Int { lock.withLock { caps } }
    }

    private struct Rig {
        var session: ThermalSession
        var guest: FakeThermalGuest
        var clock: ThermalTestClock
        var timer: Timer
        var log: Log
    }

    private func rig() -> Rig {
        let guest = FakeThermalGuest()
        let clock = ThermalTestClock()
        let timer = Timer()
        let log = Log()
        let session = ThermalSession(
            opener: guest, retryDelay: .milliseconds(10), clock: { clock.read() },
            schedule: { _, work in timer.schedule(work) }, capStarted: { log.cap() })
        return Rig(session: session, guest: guest, clock: clock, timer: timer, log: log)
    }

    private func record(_ severity: AndroidThermalSeverity) -> [UInt8] {
        Array(ThermalProtocol.record(severity))
    }

    private func connected() throws -> Rig {
        let rig = rig()
        rig.session.update(running: true, booted: true)
        #expect(thermalEventually { rig.session.isConnected })
        #expect(rig.guest.record() == record(.none))
        return rig
    }

    @Test func theHoldTellsAndroidModerateAndLiftingItTellsNone() throws {
        let rig = try connected()
        rig.session.setSixtyHertzHold(true)
        #expect(rig.guest.record() == record(.moderate))
        #expect(rig.session.severity == .moderate)
        rig.session.setSixtyHertzHold(false)
        #expect(rig.guest.record() == record(.none))
        #expect(rig.session.severity == .none)
        // Nothing here is a cap: no notice, and the device is not hot.
        rig.session.settle()
        #expect(rig.log.capCount == 0)
        #expect(!rig.session.isHot)
    }

    @Test func aHoldThatIsAlreadyInPlaceSendsNothingAgain() throws {
        let rig = try connected()
        rig.session.setSixtyHertzHold(true)
        _ = rig.guest.record()
        rig.session.setSixtyHertzHold(true)
        rig.session.settle()
        #expect(!rig.guest.hasPending())
    }

    @Test func aHoldBeforeTheRunIsTheFirstRecord() throws {
        let rig = rig()
        rig.session.setSixtyHertzHold(true)
        rig.session.update(running: true, booted: false)
        #expect(rig.guest.record() == record(.moderate))
    }

    @Test func theHeatSeverityWinsAndTheHoldComesBackWhenTheCapEnds() throws {
        let rig = try connected()
        rig.session.setSixtyHertzHold(true)
        _ = rig.guest.record()
        // Critical is above MODERATE: Android hears critical, then, when
        // the cool-down ends, MODERATE again and not none.
        rig.session.observe(.critical)
        #expect(rig.guest.record() == record(.critical))
        #expect(rig.session.isHot)
        rig.session.observe(.nominal)
        rig.session.settle()
        #expect(!rig.guest.hasPending(), "the hold of the cool-down was sent early")
        rig.clock.advance(seconds: 130)
        rig.timer.fireLatest()
        #expect(rig.guest.record() == record(.moderate))
        #expect(rig.session.severity == .moderate)
    }

    @Test func theHoldAppliesWithTheHeatLimitOffAndHeatStillTellsNothing() throws {
        let rig = try connected()
        rig.session.setLimit(false)
        // The choice itself is told again, as it always is: still none.
        #expect(rig.guest.record() == record(.none))
        rig.session.setSixtyHertzHold(true)
        #expect(rig.guest.record() == record(.moderate))
        // The limit is off: critical tells Android nothing hotter, and the
        // hold stays as it was told.
        rig.session.observe(.critical)
        rig.session.settle()
        #expect(!rig.guest.hasPending())
        #expect(rig.session.severity == .moderate)
        #expect(rig.log.capCount == 0)
        // Turning the limit on tells the hotter severity at once.
        rig.session.setLimit(true)
        #expect(rig.guest.record() == record(.critical))
    }

    @Test func aGuestThatRestartsHearsTheHoldAgain() throws {
        let rig = try connected()
        rig.session.setSixtyHertzHold(true)
        _ = rig.guest.record()
        rig.session.restart()
        rig.session.update(running: true, booted: false)
        #expect(thermalEventually { rig.session.isConnected })
        #expect(rig.guest.record() == record(.moderate))
    }

    @Test func aHoldOutsideARunIsNothing() {
        let rig = rig()
        rig.session.setSixtyHertzHold(true)
        #expect(rig.session.severity == .none)
        rig.session.settle()
        #expect(!rig.guest.hasPending())
    }
}
