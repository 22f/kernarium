// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// What the guest is told from iOS's thermal state: it rises at once, falls
/// 120 s after the hotter state ended, and a state that comes back restarts
/// the wait. The policy is pure, so every test gives it a clock of its own.
struct ThermalPolicyTests {
    private func at(_ seconds: Double) -> UInt64 { UInt64(seconds * 1_000_000_000) }

    /// A policy the tests can call inside `#expect`.
    private final class Watch {
        private var policy = ThermalPolicy()
        var severity: AndroidThermalSeverity { policy.severity }
        var nextExpiry: UInt64? { policy.nextExpiry }
        @discardableResult func observe(_ state: HostThermalState, at now: UInt64) -> Bool {
            policy.observe(state, at: now)
        }
        @discardableResult func advance(to now: UInt64) -> Bool { policy.advance(to: now) }
        func claimAnnouncement() -> Bool { policy.claimAnnouncement() }
    }

    // MARK: The mapping

    @Test func eachHostStateHasItsAndroidSeverity() {
        #expect(HostThermalState.nominal.severity == .none)
        #expect(HostThermalState.fair.severity == .light)
        #expect(HostThermalState.serious.severity == .severe)
        #expect(HostThermalState.critical.severity == .critical)
        #expect(ProcessInfoMapping.all == [.nominal, .fair, .serious, .critical])
    }

    private enum ProcessInfoMapping {
        static var all: [HostThermalState] {
            [ProcessInfo.ThermalState.nominal, .fair, .serious, .critical].map { HostThermalState($0) }
        }
    }

    @Test func aSessionStartsAtNone() {
        let policy = Watch()
        #expect(policy.severity == .none)
        #expect(policy.nextExpiry == nil)
        #expect(!policy.observe(.nominal, at: at(1)))
        #expect(policy.severity == .none)
    }

    // MARK: Rising and falling

    @Test func aRiseIsReportedAtOnceAtEveryStep() {
        let policy = Watch()
        #expect(policy.observe(.fair, at: at(10)))
        #expect(policy.severity == .light)
        #expect(policy.observe(.serious, at: at(11)))
        #expect(policy.severity == .severe)
        #expect(policy.observe(.critical, at: at(12)))
        #expect(policy.severity == .critical)
        #expect(!policy.observe(.critical, at: at(13)))
    }

    @Test func aStateThatBeginsHotIsCappedFromItsFirstReading() {
        let policy = Watch()
        #expect(policy.observe(.serious, at: at(0)))
        #expect(policy.severity == .severe)
    }

    @Test func theSeverityFallsOneHundredTwentySecondsAfterTheHotterStateEnded() {
        let policy = Watch()
        policy.observe(.serious, at: at(10))
        policy.observe(.nominal, at: at(100))
        #expect(policy.severity == .severe, "the fall was not held")
        #expect(policy.nextExpiry == at(220))
        #expect(!policy.advance(to: at(219.999)))
        #expect(policy.severity == .severe)
        #expect(policy.advance(to: at(220)))
        #expect(policy.severity == .none)
        #expect(policy.nextExpiry == nil)
    }

    @Test func aFallToACoolerStateWaitsAsWell() {
        let policy = Watch()
        policy.observe(.serious, at: at(10))
        policy.observe(.fair, at: at(100))
        #expect(policy.severity == .severe)
        policy.advance(to: at(219))
        #expect(policy.severity == .severe)
        #expect(policy.advance(to: at(220)))
        #expect(policy.severity == .light)
    }

    @Test func criticalIsHeldAndThenWhatIsStillHot() {
        let policy = Watch()
        policy.observe(.serious, at: at(0))
        policy.observe(.critical, at: at(30))
        policy.observe(.serious, at: at(60))
        #expect(policy.severity == .critical)
        // Critical ended at 60; serious is current, so the fall is to it.
        policy.advance(to: at(179))
        #expect(policy.severity == .critical)
        #expect(policy.advance(to: at(180)))
        #expect(policy.severity == .severe)
    }

    @Test func aColderStateThatEndedLaterOutlastsAHotterOne() {
        let policy = Watch()
        policy.observe(.serious, at: at(0))
        policy.observe(.fair, at: at(100))
        policy.observe(.nominal, at: at(150))
        #expect(policy.severity == .severe)
        policy.advance(to: at(219.9))
        #expect(policy.severity == .severe)
        #expect(policy.advance(to: at(220)))
        #expect(policy.severity == .light, "the fair that ended at 150 still counts")
        policy.advance(to: at(269.9))
        #expect(policy.severity == .light)
        #expect(policy.advance(to: at(270)))
        #expect(policy.severity == .none)
    }

    @Test func aStateThatComesBackRestartsTheWait() {
        let policy = Watch()
        policy.observe(.serious, at: at(0))
        policy.observe(.nominal, at: at(50))
        // The first wait would end at 170.
        policy.observe(.serious, at: at(100))
        #expect(policy.severity == .severe)
        policy.observe(.nominal, at: at(130))
        policy.advance(to: at(170))
        #expect(policy.severity == .severe, "the first wait still ran")
        policy.advance(to: at(249.9))
        #expect(policy.severity == .severe)
        #expect(policy.advance(to: at(250)))
        #expect(policy.severity == .none)
    }

    @Test func aReadingAfterALongGapFindsTheExpiryAlreadyPassed() {
        let policy = Watch()
        policy.observe(.serious, at: at(0))
        policy.observe(.nominal, at: at(10))
        // The application was away longer than the wait.
        #expect(policy.observe(.nominal, at: at(1000)))
        #expect(policy.severity == .none)
    }

    @Test func aRepeatedReadingChangesNothing() {
        let policy = Watch()
        policy.observe(.serious, at: at(0))
        for second in stride(from: 1.0, to: 60.0, by: 7) {
            #expect(!policy.observe(.serious, at: at(second)))
        }
        #expect(policy.nextExpiry == nil)
    }

    // MARK: Against a definition

    /// The severity by its definition, from the whole history: the hottest
    /// state in effect at any moment of the last 120 s.
    private func reference(history: [(start: Double, state: HostThermalState)], now: Double) -> AndroidThermalSeverity {
        var hottest = HostThermalState.nominal
        for (index, entry) in history.enumerated() {
            let end = index + 1 < history.count ? history[index + 1].start : Double.infinity
            // In effect during [start, end); the window is (now - 120, now].
            if entry.start <= now, end > now - 120, entry.state > hottest { hottest = entry.state }
        }
        return hottest.severity
    }

    private struct Generator {
        var state: UInt64
        mutating func next() -> UInt64 {
            state &+= 0x9E37_79B9_7F4A_7C15
            var value = state
            value = (value ^ (value >> 30)) &* 0xBF58_476D_1CE4_E5B9
            value = (value ^ (value >> 27)) &* 0x94D0_49BB_1331_11EB
            return value ^ (value >> 31)
        }
    }

    @Test func randomWalksAgreeWithTheDefinitionAndNeverPassCritical() {
        var expiriesChecked = 0
        for seed in UInt64(1)...40 {
            var random = Generator(state: seed)
            let policy = Watch()
            var history: [(start: Double, state: HostThermalState)] = [(0, .nominal)]
            var clock = 0.0
            for _ in 0..<60 {
                clock += Double(random.next() % 90) + 1
                let state = HostThermalState.allCases[Int(random.next() % 4)]
                // Look at every expiry that passed before this reading.
                while let expiry = policy.nextExpiry, Double(expiry) / 1e9 <= clock {
                    let moment = Double(expiry) / 1e9
                    policy.advance(to: expiry)
                    expiriesChecked += 1
                    #expect(policy.severity == reference(history: history, now: moment), "seed \(seed) at expiry \(moment)")
                }
                if history.last?.state != state { history.append((clock, state)) }
                policy.observe(state, at: at(clock))
                #expect(policy.severity == reference(history: history, now: clock), "seed \(seed) at \(clock)")
                #expect(policy.severity.rawValue <= 4)
            }
        }
        #expect(expiriesChecked > 100, "the walks never reached an expiry")
    }

    // MARK: The announcement

    @Test func theCapIsAnnouncedOnceAtTheFirstSevere() {
        let policy = Watch()
        policy.observe(.fair, at: at(0))
        #expect(!policy.claimAnnouncement(), "announced at light")
        #expect(!policy.claimAnnouncement(), "a refusal consumed the announcement")
        policy.observe(.serious, at: at(10))
        #expect(policy.claimAnnouncement())
        #expect(!policy.claimAnnouncement())
    }

    @Test func theCapIsNotAnnouncedAgainAfterCoolingAndHeatingAgain() {
        let policy = Watch()
        policy.observe(.serious, at: at(0))
        #expect(policy.claimAnnouncement())
        policy.observe(.nominal, at: at(30))
        policy.advance(to: at(150))
        #expect(policy.severity == .none)
        policy.observe(.critical, at: at(200))
        #expect(!policy.claimAnnouncement())
        policy.observe(.fair, at: at(210))
        policy.observe(.serious, at: at(400))
        #expect(!policy.claimAnnouncement())
    }

    @Test func aSessionThatNeverReachesSevereAnnouncesNothing() {
        let policy = Watch()
        for (second, state) in [(0.0, HostThermalState.fair), (30, .nominal), (500, .fair), (600, .nominal)] {
            policy.observe(state, at: at(second))
            #expect(!policy.claimAnnouncement())
        }
        policy.advance(to: at(1000))
        #expect(!policy.claimAnnouncement())
    }

    @Test func aJumpStraightToCriticalIsTheFirstSevere() {
        let policy = Watch()
        policy.observe(.critical, at: at(5))
        #expect(policy.claimAnnouncement())
    }

    @Test func aSessionThatStartsHotAnnouncesAtOnce() {
        let policy = Watch()
        policy.observe(.serious, at: at(0))
        #expect(policy.claimAnnouncement())
    }

    @Test func aLaterAnnouncementWaitsForTheCallThatCanMakeItWhileTheCapHolds() {
        let policy = Watch()
        policy.observe(.serious, at: at(0))
        // The caller could not announce yet and the state cooled: the cap
        // still counts for 120 s, so it is still to be announced.
        policy.observe(.nominal, at: at(20))
        #expect(policy.claimAnnouncement())
    }
}
