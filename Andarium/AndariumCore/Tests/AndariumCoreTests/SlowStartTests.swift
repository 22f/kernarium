// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// When the boot screen says a start is slow: three minutes of one start,
/// counted across the guest's restarts while the start is unfinished.
struct SlowStartTests {
    private let t0 = Date(timeIntervalSince1970: 1_800_000_000)

    @Test func theNoticeComesAtThreeMinutesAndOnlyOnce() {
        var start = SlowStart()
        start.begin(at: t0)
        let result1 = start.update(at: t0.addingTimeInterval(179.9))
        #expect(!result1)
        #expect(!start.showing)
        let result2 = start.update(at: t0.addingTimeInterval(180))
        #expect(result2)
        #expect(start.showing)
        // Looking again, later, says nothing new.
        let result3 = start.update(at: t0.addingTimeInterval(181))
        #expect(!result3)
        #expect(start.showing)
        #expect(SlowStart.threshold == 180)
    }

    @Test func noStartIsNeverSlow() {
        var start = SlowStart()
        let result4 = start.update(at: t0.addingTimeInterval(10_000))
        #expect(!result4)
        #expect(!start.showing && start.elapsedSeconds(at: t0) == 0)
        let result5 = start.requestStop()
        #expect(!result5)
    }

    @Test func aClockThatGoesBackNeitherShowsTheNoticeNorCountsNegativeTime() {
        var start = SlowStart()
        start.begin(at: t0)
        #expect(start.elapsedSeconds(at: t0.addingTimeInterval(-50)) == 0)
        let result6 = start.update(at: t0.addingTimeInterval(-50))
        #expect(!result6)
    }

    @Test func aRestartOfAnUnfinishedStartKeepsTheTime() {
        var start = SlowStart()
        start.begin(at: t0)
        start.guestRestarted(at: t0.addingTimeInterval(100), startUnfinished: true)
        #expect(start.began == t0 && start.restarts == 1)
        #expect(start.elapsedSeconds(at: t0.addingTimeInterval(150)) == 150)
        // So a slow loop of restarts reaches the notice.
        start.guestRestarted(at: t0.addingTimeInterval(170), startUnfinished: true)
        let result7 = start.update(at: t0.addingTimeInterval(181))
        #expect(result7)
        // The notice, once up, and the stop asked for, survive the next restart.
        let result8 = start.requestStop()
        #expect(result8)
        start.guestRestarted(at: t0.addingTimeInterval(200), startUnfinished: true)
        #expect(start.showing && start.stopping && start.began == t0)
    }

    @Test func aRestartOfARunningSessionIsANewBoot() {
        var start = SlowStart()
        start.begin(at: t0)
        start.finish()
        #expect(start.began == nil)
        // The session was running, so the restart's boot begins its own time.
        start.guestRestarted(at: t0.addingTimeInterval(500), startUnfinished: false)
        #expect(start.began == t0.addingTimeInterval(500) && start.restarts == 0)
        let result9 = start.update(at: t0.addingTimeInterval(600))
        #expect(!result9)
        let result10 = start.update(at: t0.addingTimeInterval(680))
        #expect(result10)
    }

    @Test func aRestartWithNoStartBeginsOne() {
        var start = SlowStart()
        start.guestRestarted(at: t0, startUnfinished: true)
        #expect(start.began == t0)
    }

    @Test func finishingTheBootTakesTheNoticeAway() {
        var start = SlowStart()
        start.begin(at: t0)
        start.update(at: t0.addingTimeInterval(200))
        let result11 = start.requestStop()
        #expect(result11)
        start.finish()
        #expect(start == SlowStart())
        let result12 = start.update(at: t0.addingTimeInterval(400))
        #expect(!result12)
    }

    @Test func aNewStartForgetsTheLastOnesNotice() {
        var start = SlowStart()
        start.begin(at: t0)
        start.update(at: t0.addingTimeInterval(200))
        start.begin(at: t0.addingTimeInterval(1000))
        #expect(!start.showing && !start.stopping)
        #expect(start.elapsedSeconds(at: t0.addingTimeInterval(1010)) == 10)
    }

    @Test func stopIsAskedForOnce() {
        var start = SlowStart()
        start.begin(at: t0)
        let result13 = start.requestStop()
        #expect(result13)
        let result14 = start.requestStop()
        #expect(!result14)
        #expect(start.stopping)
    }

    @Test func theReportNamesTheStepInEnglish() {
        let names = (0..<BootSteps.count).map { BootSteps.reportName(ofStep: $0, recovery: false) }
        #expect(names == ["System image", "Linux kernel", "Storage", "Startup"])
        #expect(BootSteps.reportName(ofStep: BootSteps.count - 1, recovery: true) == "Recovery")
    }
}
