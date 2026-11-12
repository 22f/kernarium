// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumReviewLogic

/// A storage in memory that counts its writes.
private final class MemoryStore: ReviewAskPersistence, @unchecked Sendable {
    var snapshot = ReviewAskSnapshot()
    var history: ReviewAskHistory? = ReviewAskHistory()
    var snapshotWrites = 0
    var historyWrites = 0

    func loadSnapshot() -> ReviewAskSnapshot { snapshot }
    func save(_ snapshot: ReviewAskSnapshot) {
        self.snapshot = snapshot
        snapshotWrites += 1
    }
    func loadHistory() -> ReviewAskHistory? { history }
    func save(_ history: ReviewAskHistory) {
        self.history = history
        historyWrites += 1
    }
}

/// A clock that moves when a test says.
private final class Clock: @unchecked Sendable {
    var now: TimeInterval = 1000
    func advance(_ seconds: TimeInterval) { now += seconds }
}

private let hour: TimeInterval = 3600
private let day: TimeInterval = 24 * 3600

/// A situation in which the request is due; a test breaks one thing at a time.
private func due() -> ReviewAskRule.Situation {
    ReviewAskRule.Situation(activeSeconds: ReviewAskRule.threshold, onHome: true)
}

@Suite struct ReviewAskCounterTests {
    @Test func onlyTimeSpentCountingAdds() {
        var counter = ReviewAskCounter()
        #expect(counter.total(at: 50) == 0)
        counter.set(counting: true, at: 100)
        #expect(counter.total(at: 160) == 60)
        counter.set(counting: false, at: 200)
        // Time away (Home, the background, a boot) adds nothing.
        #expect(counter.total(at: 5000) == 100)
        counter.set(counting: true, at: 5000)
        counter.set(counting: false, at: 5030)
        #expect(counter.total(at: 9000) == 130)
    }

    @Test func settingTheStateItHasChangesNothing() {
        var counter = ReviewAskCounter()
        let stopsNothing = counter.set(counting: false, at: 0)
        let starts = counter.set(counting: true, at: 0)
        let startsAgain = counter.set(counting: true, at: 50)
        #expect(counter.total(at: 80) == 80)
        let stops = counter.set(counting: false, at: 80)
        #expect(!stopsNothing && starts && !startsAgain && stops)
    }

    @Test func aClockThatGoesBackNeverTakesTimeAway() {
        var counter = ReviewAskCounter(banked: 500)
        counter.set(counting: true, at: 1000)
        #expect(counter.total(at: 900) == 500)
        counter.set(counting: false, at: 800)
        #expect(counter.total(at: 2000) == 500)
    }

    @Test func aBankKeepsCounting() {
        var counter = ReviewAskCounter()
        counter.set(counting: true, at: 0)
        counter.bank(at: 100)
        #expect(counter.banked == 100 && counter.isCounting)
        #expect(counter.total(at: 150) == 150)
        var idle = ReviewAskCounter(banked: 7)
        idle.bank(at: 99)
        #expect(idle.banked == 7 && !idle.isCounting)
    }

    @Test func aDamagedValueIsHeldToWhatACounterCanBe() {
        #expect(ReviewAskCounter(banked: -5).banked == 0)
        #expect(ReviewAskCounter(banked: .nan).banked == 0)
        #expect(ReviewAskCounter(banked: .infinity).banked == 0)
        #expect(ReviewAskCounter(banked: 1e30).banked == ReviewAskCounter.ceiling)
    }
}

@Suite struct ReviewAskRuleTests {
    @Test func theThresholdIsFiveHoursAndTheSettleASecond() {
        #expect(ReviewAskRule.threshold == 5 * hour)
        #expect(ReviewAskRule.settle == .seconds(1))
        #expect(ReviewAskRule.checkpoint == .seconds(60))
    }

    @Test func itAsksAtExactlyTheThresholdAndNotBefore() {
        var situation = due()
        #expect(ReviewAskRule.isEligible(situation))
        situation.activeSeconds = ReviewAskRule.threshold - 1
        #expect(ReviewAskRule.blockers(situation) == [.belowThreshold])
        situation.activeSeconds = ReviewAskRule.threshold + 1
        #expect(ReviewAskRule.isEligible(situation))
    }

    @Test func eachConditionAloneHoldsItBack() {
        let cases: [(ReviewAskRule.Blocker, (inout ReviewAskRule.Situation) -> Void)] = [
            (.belowThreshold, { $0.activeSeconds = 4 * hour }),
            (.done, { $0.done = true }),
            (.tooSoon, { $0.tooSoon = true }),
            (.notOnHome, { $0.onHome = false }),
            (.noGuest, { $0.hasGuest = false }),
            (.lastRunNotNormal, { $0.lastRunEndedNormally = false }),
            (.noticeShown, { $0.noticeShown = true }),
            (.somethingPresented, { $0.somethingPresented = true }),
            (.sceneInactive, { $0.sceneActive = false }),
            (.notSettled, { $0.settled = false }),
        ]
        #expect(Set(cases.map(\.0)) == Set(ReviewAskRule.Blocker.allCases))
        for (blocker, change) in cases {
            var situation = due()
            change(&situation)
            #expect(ReviewAskRule.blockers(situation) == [blocker], "\(blocker)")
            #expect(!ReviewAskRule.isEligible(situation))
        }
    }

    @Test func theConditionsTogetherNeedAllOfThem() {
        var situation = ReviewAskRule.Situation(
            activeSeconds: 0, done: true, tooSoon: true, onHome: false, hasGuest: false, lastRunEndedNormally: false,
            noticeShown: true, somethingPresented: true, sceneActive: false, settled: false)
        #expect(Set(ReviewAskRule.blockers(situation)) == Set(ReviewAskRule.Blocker.allCases))
        situation = due()
        #expect(ReviewAskRule.blockers(situation).isEmpty)
    }

    @Test func waitingForTheSettleStartsOnlyWhenNothingElseHoldsItBack() {
        var situation = due()
        situation.settled = false
        #expect(ReviewAskRule.isEligibleOnceSettled(situation))
        situation.noticeShown = true
        #expect(!ReviewAskRule.isEligibleOnceSettled(situation))
    }
}

@Suite struct ReviewAskTrackerTests {
    private func make(
        _ store: MemoryStore = MemoryStore(), _ clock: Clock = Clock()
    ) -> (ReviewAskTracker, MemoryStore, Clock) {
        (ReviewAskTracker(persistence: store, uptime: { clock.now }), store, clock)
    }

    @Test func aVirtualMachineInTheForegroundCountsAndNothingElseDoes() {
        var (tracker, _, clock) = make()
        // Home, then a boot that has not reached the guest, then the guest.
        clock.advance(600)
        tracker.count(false)
        clock.advance(120)
        tracker.count(true)
        clock.advance(2 * hour)
        // The application leaves the foreground, with the guest paused.
        tracker.count(false)
        clock.advance(10 * hour)
        tracker.count(true)
        clock.advance(3 * hour)
        tracker.count(false)
        #expect(tracker.activeSeconds == 5 * hour)
        clock.advance(day)
        #expect(tracker.activeSeconds == 5 * hour)
    }

    @Test func readingTheCounterWritesNothing() {
        var (tracker, store, clock) = make()
        tracker.count(true)
        let writes = store.snapshotWrites
        for _ in 0..<10_000 {
            clock.advance(0.016)
            _ = tracker.activeSeconds
        }
        #expect(store.snapshotWrites == writes)
    }

    @Test func itWritesAtStateChangesAndAtCheckpointsOnly() {
        var (tracker, store, clock) = make()
        tracker.count(false)
        #expect(store.snapshotWrites == 0)
        tracker.count(true)
        #expect(store.snapshotWrites == 1)
        tracker.count(true)
        #expect(store.snapshotWrites == 1)
        clock.advance(60)
        tracker.checkpoint()
        #expect(store.snapshotWrites == 2 && store.snapshot.activeSeconds == 60)
        tracker.count(false)
        #expect(store.snapshotWrites == 3 && store.snapshot.activeSeconds == 60)
        // A checkpoint with nothing running writes nothing.
        tracker.checkpoint()
        #expect(store.snapshotWrites == 3)
    }

    @Test func theCounterSurvivesARelaunch() {
        var (tracker, store, clock) = make()
        tracker.count(true)
        clock.advance(2 * hour)
        tracker.count(false)
        // A new process: its clock starts from its own boot.
        let relaunched = ReviewAskTracker(persistence: store, uptime: { 5 })
        #expect(relaunched.activeSeconds == 2 * hour)
    }

    @Test func aCrashLosesAtMostTheCheckpointInterval() {
        var (tracker, store, clock) = make()
        tracker.count(true)
        var lastSeen = 0.0
        // A checkpoint each minute for an hour, then the application ends
        // 59 seconds into the next minute without writing anything.
        for _ in 0..<60 {
            clock.advance(60)
            tracker.checkpoint()
            let relaunched = ReviewAskTracker(persistence: store, uptime: { 0 })
            // Never decreases from one checkpoint to the next.
            #expect(relaunched.activeSeconds >= lastSeen)
            lastSeen = relaunched.activeSeconds
        }
        clock.advance(59)
        #expect(tracker.activeSeconds == hour + 59)
        let afterCrash = ReviewAskTracker(persistence: store, uptime: { 0 })
        #expect(afterCrash.activeSeconds == hour)
        #expect(tracker.activeSeconds - afterCrash.activeSeconds <= 60)
    }

    @Test func aRunInProgressIsNotANormalEndAndAnEndSays() {
        var (tracker, store, _) = make()
        #expect(tracker.lastRunEndedNormally)
        tracker.runBegan()
        #expect(!tracker.lastRunEndedNormally)
        // The application ends in the middle of the run: the next launch finds it so.
        #expect(!ReviewAskTracker(persistence: store, uptime: { 0 }).lastRunEndedNormally)
        tracker.runEnded(normally: true)
        #expect(ReviewAskTracker(persistence: store, uptime: { 0 }).lastRunEndedNormally)
        tracker.runBegan()
        tracker.runEnded(normally: false)
        #expect(!ReviewAskTracker(persistence: store, uptime: { 0 }).lastRunEndedNormally)
    }

    @Test func theSessionRecordDecidesWhenItIsThere() {
        // The run ended normally while the application was in the background and
        // it was ended before it could say so: the record says it did.
        var (tracker, store, _) = make()
        tracker.runBegan()
        var launched = ReviewAskTracker(persistence: store, uptime: { 0 })
        launched.runEnded(normally: true)
        #expect(ReviewAskTracker(persistence: store, uptime: { 0 }).lastRunEndedNormally)
        // And the other way: the record says the run did not end well.
        launched.runEnded(normally: false)
        #expect(!ReviewAskTracker(persistence: store, uptime: { 0 }).lastRunEndedNormally)
    }

    @Test func writingTheSameRunStateAgainWritesNothing() {
        var (tracker, store, _) = make()
        tracker.runEnded(normally: true)
        #expect(store.snapshotWrites == 0)
        tracker.runBegan()
        tracker.runBegan()
        #expect(store.snapshotWrites == 1)
    }

    @Test func aHistoryThatCannotBeReadAsksNothing() {
        let store = MemoryStore()
        store.history = nil
        let tracker = ReviewAskTracker(persistence: store, uptime: { 0 })
        #expect(tracker.history.done)
    }

    @Test func theDebugSetterIsInDebugBuildsOnly() {
        var (tracker, store, _) = make()
        tracker.beginAttempt(at: Date(timeIntervalSince1970: 0))
        tracker.debugSet(activeSeconds: 6 * hour, clearHistory: true)
        #expect(tracker.activeSeconds == 6 * hour && store.snapshot.activeSeconds == 6 * hour)
        #expect(tracker.history == ReviewAskHistory() && store.history == ReviewAskHistory())
        tracker.debugSet(activeSeconds: nil, clearHistory: false)
        #expect(tracker.activeSeconds == 6 * hour)
    }
}

@Suite struct ReviewAskHistoryTests {
    private let start = Date(timeIntervalSince1970: 1_800_000_000)

    @Test func threeAttemptsAreTheMostWhateverTheyShowed() {
        var history = ReviewAskHistory()
        for attempt in 1...3 {
            #expect(!history.done)
            history.beginAttempt(at: start.addingTimeInterval(Double(attempt) * 8 * day))
            history.conclude(.notSeen)
            #expect(history.attempts == attempt)
        }
        #expect(history.done && !history.finished)
    }

    @Test func theThirdAttemptEndsItEvenWhenItIsNotJudgedOrSeenLater() {
        var history = ReviewAskHistory(attempts: 2, lastAttempt: start)
        history.beginAttempt(at: start.addingTimeInterval(8 * day))
        // Recorded before the request: the end of the application loses nothing.
        #expect(history.done)
    }

    @Test func aSeenAttemptEndsItForGood() {
        var history = ReviewAskHistory()
        history.beginAttempt(at: start)
        history.conclude(.seen)
        #expect(history.finished && history.done && history.attempts == 1)
    }

    @Test func anotherAttemptWaitsSevenDays() {
        var history = ReviewAskHistory()
        #expect(!history.isTooSoon(at: start))
        history.beginAttempt(at: start)
        history.conclude(.notSeen)
        #expect(history.isTooSoon(at: start))
        #expect(history.isTooSoon(at: start.addingTimeInterval(7 * day - 1)))
        #expect(!history.isTooSoon(at: start.addingTimeInterval(7 * day)))
        #expect(!history.isTooSoon(at: start.addingTimeInterval(30 * day)))
    }

    @Test func aClockSetBackDoesNotHoldItForever() {
        let history = ReviewAskHistory(attempts: 1, lastAttempt: start.addingTimeInterval(400 * day))
        #expect(!history.isTooSoon(at: start))
    }

    @Test func anAttemptTheApplicationOverlappedIsGivenBackAndRetriedUnderTheSameSpacing() {
        var history = ReviewAskHistory()
        history.beginAttempt(at: start)
        history.conclude(.notJudged)
        #expect(history.attempts == 0 && !history.done)
        #expect(history.isTooSoon(at: start.addingTimeInterval(day)))
        #expect(!history.isTooSoon(at: start.addingTimeInterval(7 * day)))
        history.conclude(.notJudged)
        #expect(history.attempts == 0)
    }

    @Test func openingReviewOnTheAppStoreEndsIt() {
        var (tracker, store) = (ReviewAskTracker(persistence: MemoryStore(), uptime: { 0 }), MemoryStore())
        tracker = ReviewAskTracker(persistence: store, uptime: { 0 })
        #expect(!tracker.history.done)
        tracker.openedReview()
        #expect(tracker.history.done && store.history?.finished == true)
        // Once: the second time writes nothing.
        let writes = store.historyWrites
        tracker.openedReview()
        #expect(store.historyWrites == writes)
        #expect(ReviewAskTracker(persistence: store, uptime: { 0 }).history.done)
    }

    @Test func theAttemptsSurviveARelaunch() {
        let store = MemoryStore()
        var tracker = ReviewAskTracker(persistence: store, uptime: { 0 })
        tracker.beginAttempt(at: start)
        tracker.conclude(.notSeen)
        let relaunched = ReviewAskTracker(persistence: store, uptime: { 0 })
        #expect(relaunched.history == tracker.history)
        #expect(relaunched.isTooSoon(at: start.addingTimeInterval(day)))
    }

    @Test func twoReadingsOfTheHistoryMergeToTheFurthest() {
        let early = ReviewAskHistory(attempts: 1, lastAttempt: start)
        let late = ReviewAskHistory(attempts: 2, lastAttempt: start.addingTimeInterval(day), finished: false)
        let merged = early.merged(with: late)
        #expect(merged.attempts == 2 && merged.lastAttempt == late.lastAttempt && !merged.finished)
        #expect(early.merged(with: ReviewAskHistory(finished: true)).finished)
        #expect(ReviewAskHistory().merged(with: ReviewAskHistory()) == ReviewAskHistory())
    }

    @Test func aDamagedHistoryIsHeldToWhatAHistoryCanBe() {
        #expect(ReviewAskHistory(attempts: -4).sanitized().attempts == 0)
        #expect(ReviewAskHistory(attempts: 4000).sanitized().attempts == 99)
        #expect(ReviewAskHistory(attempts: 1, lastAttempt: Date(timeIntervalSinceReferenceDate: .nan)).sanitized().lastAttempt == nil)
    }

    @Test func theHistoryRoundTripsThroughJSON() throws {
        let history = ReviewAskHistory(attempts: 2, lastAttempt: start, finished: true)
        let data = try JSONEncoder().encode(history)
        #expect(try JSONDecoder().decode(ReviewAskHistory.self, from: data) == history)
    }
}

@Suite struct ReviewPromptJudgementTests {
    private typealias Observation = ReviewPromptObservation

    @Test func theSceneLeavingOrTheKeyWindowResigningIsEvidence() {
        #expect(ReviewPromptJudgement.evidence(Observation(sceneLeftForeground: true), appPresented: false) == .seen)
        #expect(ReviewPromptJudgement.evidence(Observation(keyWindowResigned: true), appPresented: false) == .seen)
        #expect(ReviewPromptJudgement.evidence(Observation(sceneLeftForeground: true, keyWindowResigned: true), appPresented: false) == .seen)
    }

    @Test func nothingHappeningIsNotEvidence() {
        #expect(ReviewPromptJudgement.evidence(Observation(), appPresented: false) == .notSeen)
    }

    @Test func aPresentationOfTheApplicationsOwnIsNoVerdictEitherWay() {
        #expect(ReviewPromptJudgement.evidence(Observation(sceneLeftForeground: true), appPresented: true) == .notJudged)
        #expect(ReviewPromptJudgement.evidence(Observation(), appPresented: true) == .notJudged)
    }
}

@Suite struct ReviewAskSequenceTests {
    /// The decision as the application makes it, from a tracker and a date.
    private func situation(_ tracker: ReviewAskTracker, at date: Date, onHome: Bool = true) -> ReviewAskRule.Situation {
        ReviewAskRule.Situation(
            activeSeconds: tracker.activeSeconds, done: tracker.history.done, tooSoon: tracker.isTooSoon(at: date),
            onHome: onHome, lastRunEndedNormally: tracker.lastRunEndedNormally)
    }

    @Test func fiveHoursThenOneNormalStopAsksOnceAndASeenPromptIsTheEnd() {
        let store = MemoryStore()
        let clock = Clock()
        var tracker = ReviewAskTracker(persistence: store, uptime: { clock.now })
        var date = Date(timeIntervalSince1970: 1_800_000_000)
        tracker.runBegan()
        tracker.count(true)
        clock.advance(4.9 * hour)
        #expect(!ReviewAskRule.isEligible(situation(tracker, at: date, onHome: false)))
        clock.advance(0.1 * hour + 1)
        tracker.count(false)
        // Still running: not on Home.
        #expect(ReviewAskRule.blockers(situation(tracker, at: date, onHome: false)).contains(.lastRunNotNormal))
        tracker.runEnded(normally: true)
        #expect(ReviewAskRule.isEligible(situation(tracker, at: date)))
        tracker.beginAttempt(at: date)
        tracker.conclude(.seen)
        date.addTimeInterval(365 * day)
        #expect(ReviewAskRule.blockers(situation(tracker, at: date)) == [.done])
    }

    @Test func anUnseenPromptIsRetriedAfterAWeekThreeTimesAtMost() {
        var tracker = ReviewAskTracker(persistence: MemoryStore(), uptime: { 0 })
        tracker.debugSet(activeSeconds: 6 * hour, clearHistory: false)
        var date = Date(timeIntervalSince1970: 1_800_000_000)
        var attempts = 0
        for _ in 0..<40 {
            if ReviewAskRule.isEligible(situation(tracker, at: date)) {
                tracker.beginAttempt(at: date)
                tracker.conclude(.notSeen)
                attempts += 1
            }
            date.addTimeInterval(day)
        }
        #expect(attempts == 3)
        #expect(tracker.history.done)
    }

    @Test func anAbnormalEndHoldsItBackUntilTheNextNormalOne() {
        var tracker = ReviewAskTracker(persistence: MemoryStore(), uptime: { 0 })
        tracker.debugSet(activeSeconds: 6 * hour, clearHistory: false)
        let date = Date(timeIntervalSince1970: 1_800_000_000)
        tracker.runBegan()
        tracker.runEnded(normally: false)
        #expect(ReviewAskRule.blockers(situation(tracker, at: date)) == [.lastRunNotNormal])
        tracker.runBegan()
        tracker.runEnded(normally: true)
        #expect(ReviewAskRule.isEligible(situation(tracker, at: date)))
    }
}
