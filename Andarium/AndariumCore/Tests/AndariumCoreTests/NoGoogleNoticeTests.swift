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

    @Test func onlyAFirstStartShowsThePanel() {
        #expect(everyKind.filter { NoGoogleNotice.showsOnBoot($0, firstBootPending: false) } == [.firstStart])
    }

    @Test func theRetryOfAFirstBootThatDidNotCompleteShowsItToo() {
        // A normal start of a guest whose first Android boot is unfinished is that boot still; no other kind changes.
        #expect(everyKind.filter { NoGoogleNotice.showsOnBoot($0, firstBootPending: true) } == [.normal, .firstStart])
    }

    @Test func nothingIsKeptForThePanel() {
        withDefaults { defaults in
            // A person who has used the line, and a database that has seen it all, change nothing for the panel.
            NoGoogleNotice(defaults: defaults).keepFeedbackAnswered()
            #expect(everyKind.filter { NoGoogleNotice.showsOnBoot($0, firstBootPending: false) } == [.firstStart])
            // The one key the notice keeps is the line's.
            #expect(defaults.dictionaryRepresentation().keys.filter { $0.hasPrefix("andarium.noGoogleNotice") }
                == [NoGoogleNotice.feedbackKey])
        }
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

    @Test func aLaunchThatKeepsNothingAlwaysShowsTheLine() {
        let notice = NoGoogleNotice()
        notice.keepFeedbackAnswered()
        #expect(notice.showsInFeedback)
    }
}
