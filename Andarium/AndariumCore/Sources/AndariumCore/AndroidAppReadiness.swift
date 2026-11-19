// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Whether Android is ready for an app to be opened after a start. Android's
/// boot is reported done (`apexd` marks it) before the activity manager has
/// finished `sys.boot_completed`, before the home screen has taken its first
/// focus (an activity started that early is pushed behind it) and, on a first
/// start, while Android's own setup is on the screen. The request waits for all
/// of these, by asking Android, not by guessing a delay.
public enum AndroidAppReadiness {
    /// One look at Android.
    public enum State: Equatable, Sendable {
        case ready
        /// Something is still starting.
        case notYet
        /// Android's first-run setup is on the screen: an app opened now would
        /// land in it, and nothing here knows when the person will be done.
        case setupUnfinished
    }

    /// How the wait ended.
    public enum Outcome: Equatable, Sendable {
        case ready
        case setupUnfinished
        /// Android did not become ready within the limit.
        case timedOut
    }

    /// How often Android is looked at, how long the home screen's first
    /// resume is given to settle once the rest holds, and how long Android has
    /// to become ready (counted in the guest's own running time).
    public static let pollInterval = Duration.milliseconds(400)
    public static let settle = Duration.milliseconds(300)
    public static let limit = Duration.seconds(30)

    static let bootCompletedCommand = "getprop sys.boot_completed"
    static let homeCommand = "cmd package resolve-activity --brief -a android.intent.action.MAIN -c android.intent.category.HOME"
    static let setupCommand = "settings get secure user_setup_complete"

    /// One look: the boot is complete, the internal storage is up, a home
    /// activity resolves, and the first-run setup is not on the screen. A
    /// command that cannot be asked, or answers anything else, is "not yet";
    /// the setup answer `0` is the one that says it is unfinished (a setting
    /// that is not set at all does not).
    public static func check(shell: any GuestShell, storageReady: () async -> Bool?) async -> State {
        guard let boot = try? await shell.run(bootCompletedCommand, limit: .seconds(5)), boot.exitCode == 0,
              boot.text.trimmingCharacters(in: .whitespacesAndNewlines) == "1"
        else { return .notYet }
        guard await storageReady() == true else { return .notYet }
        guard let home = try? await shell.run(homeCommand, limit: .seconds(5)), home.exitCode == 0,
              home.text.contains("/")
        else { return .notYet }
        if let setup = try? await shell.run(setupCommand, limit: .seconds(5)), setup.exitCode == 0,
           setup.text.trimmingCharacters(in: .whitespacesAndNewlines) == "0" {
            return .setupUnfinished
        }
        return .ready
    }

    /// Looks every `pollInterval` until Android is ready, its setup is on the
    /// screen, or `limit` of the guest's running time has passed (`now` is
    /// that clock, so the time Andarium spends in the background does not
    /// count). Ready is confirmed after `settle`.
    public static func wait(
        check: () async -> State, now: () -> Duration, sleep: (Duration) async -> Void,
        limit: Duration = AndroidAppReadiness.limit, interval: Duration = pollInterval, settle: Duration = settle
    ) async -> Outcome {
        let started = now()
        while true {
            if Task.isCancelled { return .timedOut }
            switch await check() {
            case .setupUnfinished:
                return .setupUnfinished
            case .ready:
                await sleep(settle)
                return .ready
            case .notYet:
                break
            }
            if now() - started >= limit { return .timedOut }
            await sleep(interval)
        }
    }
}

/// Settings > Apps > Add to Home Screen's switch for Search and Siri: whether
/// Android's apps are offered to Spotlight (and so to Search and Siri). On
/// until the person turns it off; it lasts across launches.
public struct AndroidAppSearchPreference {
    public static let key = "andarium.androidAppsInSearch"

    private let defaults: UserDefaults

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    public var isOn: Bool { defaults.object(forKey: Self.key) as? Bool ?? true }

    public func keep(_ on: Bool) {
        defaults.set(on, forKey: Self.key)
    }
}
