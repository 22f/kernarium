// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// When the application puts the official Magisk app in the guest, and what it
/// does once it has. The owner may choose another way; each choice is one
/// constant here and the rest follows it.
public enum RootInstallRule {
    /// When an absent app is put in again.
    public enum Policy: Sendable {
        /// An absent app is installed once in a period of root being on; a
        /// person who removed it keeps it removed until root is turned off and
        /// on again. Magisk's own "Hide the Magisk app" is respected too.
        case oncePerOnPeriod
        /// At every start with root, whenever the app is absent.
        case everyRootedBoot
        /// Once for the guest, whatever the switch does after.
        case oncePerGuest
    }

    /// The first option of the decision.
    public static let policy = Policy.oncePerOnPeriod

    /// The installing start ends with the Magisk app on the guest's screen, so
    /// that its one-time setup dialog is the first thing the person meets.
    public static let opensAppAfterInstall = true

    public enum Action: Equatable, Sendable {
        case none
        case install
    }

    /// What a start with root does about the app, given what the guest says of
    /// it and the version the release carries. An app older than the carried
    /// one is always updated (`-r` keeps its data); an absent one by `policy`.
    public static func decide(
        presence: MagiskApp.Presence, carriedVersionCode: Int, state: RootState, policy: Policy = RootInstallRule.policy
    ) -> Action {
        switch presence {
        case .installed(let code):
            return code < carriedVersionCode ? .install : .none
        case .absent, .unknown:
            switch policy {
            case .oncePerOnPeriod: return state.appVersionCode == nil ? .install : .none
            case .everyRootedBoot: return .install
            case .oncePerGuest: return state.appEverHandled ? .none : .install
            }
        }
    }

    /// What the state remembers after the app is there at `versionCode`.
    public static func recorded(_ state: RootState, versionCode: Int) -> RootState {
        var state = state
        state.appVersionCode = max(state.appVersionCode ?? 0, versionCode)
        state.appEverHandled = true
        return state
    }
}
