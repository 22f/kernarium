// SPDX-License-Identifier: GPL-2.0-or-later

import Testing

@testable import AndariumCore

/// The session's memory notice: when it is up, how long it stays, and what a
/// dismissal holds, on moments the test sets.
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

    /// Where `state` stands once its time has come at `seconds`.
    private func shown(_ state: MemoryPressureNoticeState, at seconds: Double) -> MemoryPressureNotice? {
        var later = state
        later.expire(at: at(seconds))
        return later.showing
    }

    @Test func theLevelsAreOrderedAndRaiseTheirNotices() {
        #expect(MemoryPressureLevel.normal < .warning)
        #expect(MemoryPressureLevel.warning < .critical)
        #expect(MemoryPressureNotice(level: .normal) == nil)
        #expect(MemoryPressureNotice(level: .warning) == .low)
        #expect(MemoryPressureNotice(level: .critical) == .critical)
    }

    @Test func theTimingsAreTheOnesTheNoticeIsBuiltOn() {
        #expect(MemoryPressureNoticeState.minimumStay == .seconds(6))
        #expect(MemoryPressureNoticeState.lingerFor == .seconds(3))
    }

    @Test func nothingShowsBeforeTheFirstReport() {
        let state = session()
        #expect(state.showing == nil)
        #expect(state.level == .normal)
        #expect(state.staysFor(at: at(1)) == nil)
    }

    // MARK: Where it is shown

    @Test func theNoticeAppearsAtTheFirstWarningWhileASessionIsOnScreen() {
        var state = session()
        state.levelChanged(.warning, at: at(1))
        #expect(state.showing == .low)
        #expect(state.level == .warning)
    }

    @Test func aWarningOutsideASessionShowsNothingUntilTheSessionIsOnScreen() {
        // On Home, on the boot screen and in the background the session is not
        // on screen; the model says so with the same call.
        var state = MemoryPressureNoticeState()
        state.levelChanged(.warning, at: at(1))
        #expect(state.showing == nil)
        state.screenChanged(onScreen: true, at: at(5))
        #expect(state.showing == .low, "the pressure lasts: the person returns to it")
    }

    @Test func theNoticeLeavesWhenTheSessionLeavesTheScreen() {
        var state = session()
        state.levelChanged(.critical, at: at(1))
        #expect(state.showing == .critical)
        state.screenChanged(onScreen: false, at: at(2))
        #expect(state.showing == nil, "backgrounded, or back on a screen that is not the session")
        #expect(state.staysFor(at: at(2)) == nil)
        state.screenChanged(onScreen: true, at: at(30))
        #expect(state.showing == .critical, "back, with the pressure still on")
    }

    @Test func anEpisodeThatEndedOutOfSightShowsNothingOnReturn() {
        var state = session()
        state.levelChanged(.warning, at: at(1))
        state.screenChanged(onScreen: false, at: at(2))
        state.levelChanged(.normal, at: at(4))
        state.screenChanged(onScreen: true, at: at(20))
        #expect(state.showing == nil)
    }

    @Test func aReturnToTheSessionStartsANewMinimumStay() {
        var state = session()
        state.levelChanged(.warning, at: at(0))
        state.screenChanged(onScreen: false, at: at(4))
        state.screenChanged(onScreen: true, at: at(100))
        state.levelChanged(.normal, at: at(101))
        #expect(state.staysFor(at: at(101)) == .seconds(5), "six seconds from the return, not from the first showing")
    }

    // MARK: How long it stays

    @Test func theNoticeStaysAsLongAsThePressureLasts() {
        var state = session()
        state.levelChanged(.warning, at: at(1))
        #expect(state.staysFor(at: at(1)) == nil, "nothing ends it but the return to normal")
        #expect(shown(state, at: 600) == .low)
        // Warnings that repeat in the episode change nothing.
        state.levelChanged(.warning, at: at(40))
        state.levelChanged(.warning, at: at(80))
        #expect(state.showing == .low)
        #expect(shown(state, at: 600) == .low)
    }

    @Test func theNoticeGoesAFewSecondsAfterTheLevelReturnsToNormal() {
        var state = session()
        state.levelChanged(.warning, at: at(1))
        state.levelChanged(.normal, at: at(20))
        #expect(state.showing == .low, "it lingers")
        #expect(state.staysFor(at: at(20)) == .seconds(3))
        #expect(shown(state, at: 22.999) == .low)
        #expect(shown(state, at: 23) == nil)
    }

    @Test func aWarningThatEndsAtOnceStaysAtLeastSixSecondsFromItsAppearance() {
        var state = session()
        state.levelChanged(.warning, at: at(10))
        state.levelChanged(.normal, at: at(11))
        // The linger would end at 14; the minimum stay ends at 16.
        #expect(state.staysFor(at: at(11)) == .seconds(5))
        #expect(shown(state, at: 15.999) == .low)
        #expect(shown(state, at: 16) == nil)
    }

    @Test func aNoticeThatAppearsWhenTheSessionComesBackCountsItsStayFromThen() {
        var state = MemoryPressureNoticeState()
        state.levelChanged(.warning, at: at(0))
        state.screenChanged(onScreen: true, at: at(5))
        #expect(state.showing == .low)
        state.levelChanged(.normal, at: at(6))
        // Six seconds from the appearance at 5, not from the report at 0.
        #expect(state.staysFor(at: at(6)) == .seconds(5))
    }

    @Test func stayingForIsNeverNegative() {
        var state = session()
        state.levelChanged(.warning, at: at(0))
        state.levelChanged(.normal, at: at(1))
        #expect(state.staysFor(at: at(50)) == .zero)
    }

    // MARK: Warning and critical

    @Test func aRiseToCriticalChangesTheSameNoticeAndDoesNotRestartItsStay() {
        var state = session()
        state.levelChanged(.warning, at: at(0))
        state.levelChanged(.critical, at: at(3))
        #expect(state.showing == .critical)
        state.levelChanged(.normal, at: at(4))
        // One showing, since 0: the stay ends at 6, not 6 s after the rise
        // (9), and the linger (7) is what is left.
        #expect(state.staysFor(at: at(4)) == .seconds(3))
        #expect(shown(state, at: 6.999) == .critical)
        #expect(shown(state, at: 7) == nil)
    }

    @Test func aFallBackToWarningStatesTheWarning() {
        var state = session()
        state.levelChanged(.critical, at: at(0))
        state.levelChanged(.warning, at: at(10))
        #expect(state.showing == .low)
        #expect(shown(state, at: 100) == .low)
    }

    @Test func aLevelThatRisesAgainWhileTheNoticeLingersContinuesTheShowing() {
        var state = session()
        state.levelChanged(.warning, at: at(0))
        state.levelChanged(.normal, at: at(10))
        state.levelChanged(.warning, at: at(11))
        #expect(state.showing == .low)
        #expect(state.staysFor(at: at(11)) == nil, "it is not on its way out")
        state.levelChanged(.normal, at: at(30))
        #expect(state.staysFor(at: at(30)) == .seconds(3))
    }

    @Test func aWarningAfterTheNoticeHasGoneIsANewShowing() {
        var state = session()
        state.levelChanged(.warning, at: at(0))
        state.levelChanged(.normal, at: at(10))
        #expect(shown(state, at: 13) == nil)
        state.expire(at: at(13))
        state.levelChanged(.warning, at: at(40))
        #expect(state.showing == .low)
        state.levelChanged(.normal, at: at(41))
        #expect(state.staysFor(at: at(41)) == .seconds(5), "a stay of its own")
    }

    // MARK: Dismissal

    @Test func aDismissalPutsTheNoticeAway() {
        var state = session()
        state.levelChanged(.warning, at: at(0))
        state.dismiss()
        #expect(state.showing == nil)
        #expect(state.staysFor(at: at(1)) == nil)
    }

    @Test func repeatedWarningsInTheEpisodeNeverShowADismissedNotice() {
        var state = session()
        state.levelChanged(.warning, at: at(0))
        state.dismiss()
        state.levelChanged(.warning, at: at(40))
        state.levelChanged(.warning, at: at(80))
        #expect(state.showing == nil)
        // Leaving the screen and coming back does not undo it either.
        state.screenChanged(onScreen: false, at: at(90))
        state.screenChanged(onScreen: true, at: at(100))
        #expect(state.showing == nil)
    }

    @Test func aDismissalHoldsUntilTheLevelHasReturnedToNormalAndRisenAgain() {
        var state = session()
        state.levelChanged(.warning, at: at(0))
        state.dismiss()
        state.levelChanged(.normal, at: at(10))
        #expect(state.showing == nil, "the dismissed notice does not linger")
        state.levelChanged(.warning, at: at(20))
        #expect(state.showing == .low)
    }

    @Test func aRiseToCriticalAfterADismissedWarningShowsOnceMore() {
        var state = session()
        state.levelChanged(.warning, at: at(0))
        state.dismiss()
        state.levelChanged(.critical, at: at(5))
        #expect(state.showing == .critical)
        state.dismiss()
        #expect(state.showing == nil)
        // Nothing in the episode shows it again.
        state.levelChanged(.critical, at: at(6))
        state.levelChanged(.warning, at: at(7))
        state.levelChanged(.critical, at: at(8))
        #expect(state.showing == nil)
        // The next episode does.
        state.levelChanged(.normal, at: at(20))
        state.levelChanged(.warning, at: at(30))
        #expect(state.showing == .low)
    }

    @Test func aDismissedCriticalCoversTheWarningThatFollowsIt() {
        var state = session()
        state.levelChanged(.critical, at: at(0))
        state.dismiss()
        state.levelChanged(.warning, at: at(3))
        #expect(state.showing == nil)
    }

    @Test func aCriticalThatWasNeverDismissedFallingBackIsDismissedAsTheWarningItNowStates() {
        var state = session()
        state.levelChanged(.critical, at: at(0))
        state.levelChanged(.warning, at: at(3))
        state.dismiss()
        state.levelChanged(.warning, at: at(10))
        #expect(state.showing == nil)
        state.levelChanged(.critical, at: at(20))
        #expect(state.showing == .critical, "a rise past what was put away")
    }

    @Test func aNoticeDismissedWhileItLingersHasNothingToHold() {
        var state = session()
        state.levelChanged(.warning, at: at(0))
        state.levelChanged(.normal, at: at(10))
        #expect(state.showing == .low)
        state.dismiss()
        #expect(state.showing == nil)
        state.levelChanged(.warning, at: at(11))
        #expect(state.showing == .low, "the pressure had already ended: this is a new episode")
    }

    @Test func aDismissalWithNothingUpHoldsNothing() {
        var state = MemoryPressureNoticeState()
        state.levelChanged(.warning, at: at(0))
        state.dismiss()
        state.screenChanged(onScreen: true, at: at(1))
        #expect(state.showing == .low)
    }

    // MARK: Runs

    @Test func theEndOfARunStartsTheNextOneClean() {
        var state = session()
        state.levelChanged(.warning, at: at(0))
        state.dismiss()
        state.runEnded()
        #expect(state.showing == nil)
        #expect(state.level == .normal)
        state.screenChanged(onScreen: true, at: at(100))
        state.levelChanged(.warning, at: at(101))
        #expect(state.showing == .low, "the dismissal was for the run that ended")
    }

    @Test func aReportThatChangesNothingLeavesTheStateEqual() {
        // The model stores the state only when it differs, so the views are
        // not redrawn for a repeated warning or a screen that did not change.
        var state = session()
        state.levelChanged(.warning, at: at(0))
        var repeated = state
        repeated.levelChanged(.warning, at: at(40))
        #expect(repeated == state)
        repeated.screenChanged(onScreen: true, at: at(41))
        repeated.expire(at: at(42))
        #expect(repeated == state)
    }
}
