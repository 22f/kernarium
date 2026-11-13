// SPDX-License-Identifier: GPL-2.0-or-later

import Testing

@testable import AndariumCore

/// The session's memory notice, for the critical level only: when it is up, how
/// long it stays, and what a dismissal holds, on moments the test sets.
struct MemoryPressureNoticeTests {
    private let t0 = ContinuousClock.Instant.now

    private func at(_ seconds: Double) -> ContinuousClock.Instant {
        t0 + .milliseconds(Int((seconds * 1000).rounded()))
    }

    /// A session on screen from the start.
    private func session() -> MemoryPressureNoticeState {
        var state = MemoryPressureNoticeState()
        state.screenChanged(onScreen: true, at: at(0))
        return state
    }

    /// Whether the notice is up once its time has come at `seconds`.
    private func shown(_ state: MemoryPressureNoticeState, at seconds: Double) -> Bool {
        var later = state
        later.expire(at: at(seconds))
        return later.isShowing
    }

    @Test func theLevelsAreOrdered() {
        #expect(MemoryPressureLevel.normal < .warning)
        #expect(MemoryPressureLevel.warning < .critical)
    }

    @Test func theTimingsAreTheOnesTheNoticeIsBuiltOn() {
        #expect(MemoryPressureNoticeState.minimumStay == .seconds(6))
        #expect(MemoryPressureNoticeState.lingerFor == .seconds(3))
    }

    @Test func nothingShowsBeforeTheFirstReport() {
        let state = session()
        #expect(!state.isShowing)
        #expect(state.level == .normal)
        #expect(state.staysFor(at: at(1)) == nil)
    }

    // MARK: Only the critical level has a notice

    @Test func aWarningShowsNothing() {
        var state = session()
        state.levelChanged(.warning, at: at(1))
        #expect(!state.isShowing)
        #expect(state.level == .warning, "the state follows the level, for the notice that ends with it")
        state.levelChanged(.warning, at: at(40))
        #expect(!state.isShowing)
    }

    @Test func theNoticeAppearsWhenTheLevelReachesCriticalWhileASessionIsOnScreen() {
        var state = session()
        state.levelChanged(.warning, at: at(1))
        state.levelChanged(.critical, at: at(2))
        #expect(state.isShowing)
        #expect(state.level == .critical)
    }

    @Test func aCriticalLevelOutsideASessionShowsNothingUntilTheSessionIsOnScreen() {
        // On Home, on the boot screen and in the background the session is not
        // on screen; the model says so with the same call.
        var state = MemoryPressureNoticeState()
        state.levelChanged(.critical, at: at(1))
        #expect(!state.isShowing)
        state.screenChanged(onScreen: true, at: at(5))
        #expect(state.isShowing, "the level is still critical: the person returns to it")
    }

    @Test func theNoticeLeavesWhenTheSessionLeavesTheScreen() {
        var state = session()
        state.levelChanged(.critical, at: at(1))
        #expect(state.isShowing)
        state.screenChanged(onScreen: false, at: at(2))
        #expect(!state.isShowing, "backgrounded, or back on a screen that is not the session")
        #expect(state.staysFor(at: at(2)) == nil)
        state.screenChanged(onScreen: true, at: at(30))
        #expect(state.isShowing, "back, with the level still critical")
    }

    @Test func anEpisodeThatLeftCriticalOutOfSightShowsNothingOnReturn() {
        var state = session()
        state.levelChanged(.critical, at: at(1))
        state.screenChanged(onScreen: false, at: at(2))
        state.levelChanged(.warning, at: at(4))
        state.screenChanged(onScreen: true, at: at(20))
        #expect(!state.isShowing, "the level is no longer critical")
        state.screenChanged(onScreen: false, at: at(21))
        state.levelChanged(.normal, at: at(22))
        state.screenChanged(onScreen: true, at: at(30))
        #expect(!state.isShowing)
    }

    @Test func aReturnToTheSessionStartsANewMinimumStay() {
        var state = session()
        state.levelChanged(.critical, at: at(0))
        state.screenChanged(onScreen: false, at: at(4))
        state.screenChanged(onScreen: true, at: at(100))
        state.levelChanged(.normal, at: at(101))
        #expect(state.staysFor(at: at(101)) == .seconds(5), "six seconds from the return, not from the first showing")
    }

    // MARK: How long it stays

    @Test func theNoticeStaysAsLongAsTheLevelIsCritical() {
        var state = session()
        state.levelChanged(.critical, at: at(1))
        #expect(state.staysFor(at: at(1)) == nil, "nothing ends it but the level leaving critical")
        #expect(shown(state, at: 600))
        // Reports that repeat in the episode change nothing.
        state.levelChanged(.critical, at: at(40))
        state.levelChanged(.critical, at: at(80))
        #expect(state.isShowing)
        #expect(shown(state, at: 600))
    }

    @Test func theNoticeGoesAFewSecondsAfterTheLevelReturnsToNormal() {
        var state = session()
        state.levelChanged(.critical, at: at(1))
        state.levelChanged(.normal, at: at(20))
        #expect(state.isShowing, "it lingers")
        #expect(state.staysFor(at: at(20)) == .seconds(3))
        #expect(shown(state, at: 22.999))
        #expect(!shown(state, at: 23))
    }

    @Test func theNoticeGoesTheSameWayWhenTheLevelFallsToWarning() {
        var state = session()
        state.levelChanged(.critical, at: at(1))
        state.levelChanged(.warning, at: at(20))
        #expect(state.isShowing, "a warning has no notice of its own, and the one that is up lingers")
        #expect(state.staysFor(at: at(20)) == .seconds(3))
        #expect(shown(state, at: 22.999))
        #expect(!shown(state, at: 23))
        // A repeated warning does not move the end.
        state.levelChanged(.warning, at: at(21))
        #expect(state.staysFor(at: at(21)) == .seconds(2))
    }

    @Test func aCriticalThatEndsAtOnceStaysAtLeastSixSecondsFromItsAppearance() {
        var state = session()
        state.levelChanged(.critical, at: at(10))
        state.levelChanged(.warning, at: at(10.5))
        // The linger would end at 13.5; the minimum stay ends at 16.
        #expect(state.staysFor(at: at(10.5)) == .seconds(5.5))
        #expect(shown(state, at: 15.999))
        #expect(!shown(state, at: 16))
    }

    @Test func aFallToWarningAfterADismissedCriticalShowsNothing() {
        // A notice put away is not shown again by the fall.
        var state = session()
        state.levelChanged(.critical, at: at(0))
        state.dismiss()
        state.levelChanged(.warning, at: at(0.5))
        #expect(!state.isShowing)
    }

    @Test func aNoticeThatAppearsWhenTheSessionComesBackCountsItsStayFromThen() {
        var state = MemoryPressureNoticeState()
        state.levelChanged(.critical, at: at(0))
        state.screenChanged(onScreen: true, at: at(5))
        #expect(state.isShowing)
        state.levelChanged(.normal, at: at(6))
        // Six seconds from the appearance at 5, not from the report at 0.
        #expect(state.staysFor(at: at(6)) == .seconds(5))
    }

    @Test func stayingForIsNeverNegative() {
        var state = session()
        state.levelChanged(.critical, at: at(0))
        state.levelChanged(.normal, at: at(1))
        #expect(state.staysFor(at: at(50)) == .zero)
    }

    @Test func aLevelThatReachesCriticalAgainWhileTheNoticeLingersContinuesTheShowing() {
        var state = session()
        state.levelChanged(.critical, at: at(0))
        state.levelChanged(.warning, at: at(10))
        state.levelChanged(.critical, at: at(11))
        #expect(state.isShowing)
        #expect(state.staysFor(at: at(11)) == nil, "it is not on its way out")
        state.levelChanged(.normal, at: at(30))
        #expect(state.staysFor(at: at(30)) == .seconds(3))
    }

    @Test func aCriticalAfterTheNoticeHasGoneIsANewShowing() {
        var state = session()
        state.levelChanged(.critical, at: at(0))
        state.levelChanged(.normal, at: at(10))
        #expect(!shown(state, at: 13))
        state.expire(at: at(13))
        state.levelChanged(.critical, at: at(40))
        #expect(state.isShowing)
        state.levelChanged(.normal, at: at(41))
        #expect(state.staysFor(at: at(41)) == .seconds(5), "a stay of its own")
    }

    // MARK: Dismissal

    @Test func aDismissalPutsTheNoticeAway() {
        var state = session()
        state.levelChanged(.critical, at: at(0))
        state.dismiss()
        #expect(!state.isShowing)
        #expect(state.staysFor(at: at(1)) == nil)
    }

    @Test func repeatedCriticalReportsInTheEpisodeNeverShowADismissedNotice() {
        var state = session()
        state.levelChanged(.critical, at: at(0))
        state.dismiss()
        state.levelChanged(.critical, at: at(40))
        state.levelChanged(.critical, at: at(80))
        #expect(!state.isShowing)
        // Leaving the screen and coming back does not undo it either.
        state.screenChanged(onScreen: false, at: at(90))
        state.screenChanged(onScreen: true, at: at(100))
        #expect(!state.isShowing)
    }

    @Test func aDismissalHoldsUntilTheLevelHasLeftCriticalAndReachedItAgain() {
        var state = session()
        state.levelChanged(.critical, at: at(0))
        state.dismiss()
        state.levelChanged(.warning, at: at(10))
        #expect(!state.isShowing, "the dismissed notice does not linger")
        state.levelChanged(.critical, at: at(20))
        #expect(state.isShowing, "a new episode")
    }

    @Test func aDismissalIsClearedByTheNormalLevelToo() {
        var state = session()
        state.levelChanged(.critical, at: at(0))
        state.dismiss()
        state.levelChanged(.normal, at: at(10))
        state.levelChanged(.critical, at: at(20))
        #expect(state.isShowing)
    }

    @Test func aNoticeDismissedWhileItLingersHasNothingToHold() {
        var state = session()
        state.levelChanged(.critical, at: at(0))
        state.levelChanged(.normal, at: at(10))
        #expect(state.isShowing)
        state.dismiss()
        #expect(!state.isShowing)
        state.levelChanged(.critical, at: at(11))
        #expect(state.isShowing, "the level had already left critical: this is a new episode")
    }

    @Test func aDismissalWithNothingUpHoldsNothing() {
        var state = MemoryPressureNoticeState()
        state.levelChanged(.critical, at: at(0))
        state.dismiss()
        state.screenChanged(onScreen: true, at: at(1))
        #expect(state.isShowing)
    }

    // MARK: Runs

    @Test func theEndOfARunStartsTheNextOneClean() {
        var state = session()
        state.levelChanged(.critical, at: at(0))
        state.dismiss()
        state.runEnded()
        #expect(!state.isShowing)
        #expect(state.level == .normal)
        state.screenChanged(onScreen: true, at: at(100))
        state.levelChanged(.critical, at: at(101))
        #expect(state.isShowing, "the dismissal was for the run that ended")
    }

    @Test func aReportThatChangesNothingLeavesTheStateEqual() {
        // The model stores the state only when it differs, so the views are
        // not redrawn for a repeated report or a screen that did not change.
        var state = session()
        state.levelChanged(.critical, at: at(0))
        var repeated = state
        repeated.levelChanged(.critical, at: at(40))
        #expect(repeated == state)
        repeated.screenChanged(onScreen: true, at: at(41))
        repeated.expire(at: at(42))
        #expect(repeated == state)
        var quiet = session()
        quiet.levelChanged(.warning, at: at(1))
        var repeatedWarning = quiet
        repeatedWarning.levelChanged(.warning, at: at(40))
        #expect(repeatedWarning == quiet)
    }
}
