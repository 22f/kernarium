// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

/// A defaults database of its own for one test, a file in a directory that
/// is removed after it, so the test leaves nothing in the user's preferences.
private func withDefaults<Result>(_ body: (UserDefaults) throws -> Result) rethrows -> Result {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("andarium-nogoogle-\(UUID().uuidString)")
    try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    // An absolute path as the suite name keeps the preferences file there.
    let name = directory.appendingPathComponent("defaults").path
    let defaults = UserDefaults(suiteName: name)!
    defer {
        defaults.removePersistentDomain(forName: name)
        try? FileManager.default.removeItem(at: directory)
    }
    return try body(defaults)
}

private let everyKind: [BootKind] = [.normal, .firstStart, .upgrade, .recovery, .restart]

struct NoGoogleNoticeTests {
    // MARK: The panel on the boot screen

    @Test func aFirstStartShowsThePanelWhateverIsKept() {
        withDefaults { defaults in
            let notice = NoGoogleNotice(defaults: defaults)
            #expect(notice.showsOnBoot(.firstStart))
            // Nothing kept: a start of an installation that has not seen it shows it too, but never a restart or recovery.
            #expect(everyKind.filter(notice.showsOnBoot) == [.normal, .firstStart, .upgrade])
        }
    }

    @Test func aFirstStartThatHasBeenSeenIsSeenAgainAfterErase() {
        withDefaults { defaults in
            let notice = NoGoogleNotice(defaults: defaults)
            notice.keepBootShown()
            // The next first start, a new virtual machine or one after Erase, is a first start again.
            #expect(notice.showsOnBoot(.firstStart))
        }
    }

    @Test func theNoticeIsSeenOnceAndNotShownAgain() {
        withDefaults { defaults in
            // The installation that finished its first start on an earlier build: its next start shows it.
            #expect(NoGoogleNotice(defaults: defaults).showsOnBoot(.normal))
            NoGoogleNotice(defaults: defaults).keepBootShown()
            // The next launch reads the same database and shows it no more.
            for kind in [BootKind.normal, .upgrade, .recovery, .restart] {
                #expect(!NoGoogleNotice(defaults: defaults).showsOnBoot(kind), "\(kind)")
            }
        }
    }

    @Test func anExistingInstallationSeesItOnItsNextNormalOrUpdateStartOnce() {
        for kind in [BootKind.normal, .upgrade] {
            withDefaults { defaults in
                let notice = NoGoogleNotice(defaults: defaults)
                #expect(notice.showsOnBoot(kind), "\(kind)")
                notice.keepBootShown()
                #expect(!notice.showsOnBoot(kind), "\(kind) a second time")
            }
        }
    }

    @Test func aRestartAndRecoveryNeverShowIt() {
        withDefaults { defaults in
            let notice = NoGoogleNotice(defaults: defaults)
            #expect(!notice.showsOnBoot(.restart) && !notice.showsOnBoot(.recovery))
            // And one of them does not use the notice up for the start that follows.
            #expect(notice.showsOnBoot(.normal))
        }
    }

    // MARK: A launch that keeps nothing

    @Test func aLaunchThatKeepsNothingShowsThePanelOnAFirstStartOnly() {
        let notice = NoGoogleNotice()
        #expect(everyKind.filter(notice.showsOnBoot) == [.firstStart])
        notice.keepBootShown()
        #expect(everyKind.filter(notice.showsOnBoot) == [.firstStart])
    }

    // MARK: The line in Send feedback

    @Test func theLineShowsUntilItHasBeenOpenedOrClosed() {
        withDefaults { defaults in
            #expect(NoGoogleNotice(defaults: defaults).showsInFeedback)
            // Opening the sheet and leaving it alone leaves the line for the next one.
            #expect(NoGoogleNotice(defaults: defaults).showsInFeedback)
            NoGoogleNotice(defaults: defaults).keepFeedbackAnswered()
            #expect(!NoGoogleNotice(defaults: defaults).showsInFeedback)
        }
    }

    @Test func theTwoNoticesAreKeptApart() {
        withDefaults { defaults in
            let notice = NoGoogleNotice(defaults: defaults)
            notice.keepBootShown()
            #expect(notice.showsInFeedback)
            notice.keepFeedbackAnswered()
            #expect(NoGoogleNotice(defaults: defaults).showsOnBoot(.firstStart))
            #expect(Set([NoGoogleNotice.bootKey, NoGoogleNotice.feedbackKey]).count == 2)
        }
    }

    @Test func aLaunchThatKeepsNothingAlwaysShowsTheLine() {
        let notice = NoGoogleNotice()
        notice.keepFeedbackAnswered()
        #expect(notice.showsInFeedback)
    }
}
