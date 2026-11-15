// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Observation

/// Where the old apps come from: the runtime's driver asks the image's
/// `kernarium_oldapp` command through `OldAppCompatibility`, and the Simulator
/// preview plays a list. Everything the screens know of the command is
/// `AndariumCore`'s `OldApp`, `OldAppListing` and `OldAppWrite`.
@MainActor
protocol OldAppsSource: AnyObject {
    /// The installed apps Old app compatibility can apply to.
    func listOldApps() async -> OldAppListing
    /// Switches it on or off for one app, which the image force-stops.
    func setOldApp(_ package: String, on: Bool) async -> OldAppWrite
    /// Whether the app has a process now; nil when that cannot be told.
    func isOldAppOpen(_ package: String) async -> Bool?
}

/// What the Old app compatibility page asks the person before it changes a
/// switch, and the one thing it says when the image would not.
enum OldAppsPrompt: Identifiable, Equatable {
    /// Turning it on is asked every time. `open` adds that the app closes now.
    case turnOn(OldApp, open: Bool)
    /// Turning it off is asked only for an app that is open.
    case turnOff(OldApp)
    /// The virtual machine did not accept the change.
    case notChanged

    var id: String {
        switch self {
        case let .turnOn(app, open): "on-\(app.package)-\(open)"
        case let .turnOff(app): "off-\(app.package)"
        case .notChanged: "not-changed"
        }
    }
}

/// What the session knows of Old app compatibility: the last listing and the
/// change under way. It lasts for one run: the image's apps and the switch
/// belong to the virtual machine that is running.
@Observable
final class OldAppsState {
    /// The apps of the last listing that answered, by name.
    private(set) var apps: [OldApp] = []
    /// A listing has answered this run (an empty one included).
    private(set) var listed = false
    /// The image has no listing command, or one this Andarium cannot read.
    private(set) var unavailable = false
    /// The last listing did not get an answer.
    private(set) var failed = false
    /// A listing is under way.
    private(set) var listing = false
    /// The package whose switch is being asked about or written.
    var busy: String?
    var prompt: OldAppsPrompt?
    /// The listing under way, which a second asker joins.
    @ObservationIgnored var inFlight: Task<OldAppListing, Never>?

    /// The Settings row is offered: the last listing found an app.
    var offered: Bool { !unavailable && !apps.isEmpty }
    /// The switch is on for at least one app.
    var anyOn: Bool { apps.contains(where: \.isOn) }

    func begin() {
        listing = true
        failed = false
    }

    func apply(_ answer: OldAppListing) {
        listing = false
        switch answer {
        case let .apps(found):
            apps = found
            listed = true
            unavailable = false
            failed = false
        case .unavailable:
            apps = []
            listed = true
            unavailable = true
            failed = false
        case .failed:
            failed = true
        }
    }

    /// The app's switch, as the image last answered or the person last changed it.
    func record(_ package: String, on: Bool, at time: Int) {
        guard let index = apps.firstIndex(where: { $0.package == package }) else { return }
        apps[index].isOn = on
        apps[index].turnedOn = on ? time : 0
    }

    func reset() {
        inFlight?.cancel()
        inFlight = nil
        apps = []
        listed = false
        unavailable = false
        failed = false
        listing = false
        busy = nil
        prompt = nil
    }
}

extension AppModel {
    /// Where the listing comes from: the runtime, or the list a preview or a
    /// UI test's probe session plays.
    private var oldAppsSource: (any OldAppsSource)? {
        #if !APP_STORE
        if let scriptedOldApps { return scriptedOldApps }
        #endif
        return driver
    }

    /// A listing can be asked now: a normal session whose runtime installs
    /// apps (Recovery runs no adb), not paused for space.
    private var canAskOldApps: Bool {
        guard capabilities.apkInstall, case .running(.normal) = phase, spacePause == nil else { return false }
        return oldAppsSource != nil
    }

    /// Lists the apps. A listing already under way is joined, one that found no
    /// command is not asked again, and nil says the run does not allow one.
    @discardableResult
    func refreshOldApps() async -> OldAppListing? {
        let state = session.oldApps
        guard canAskOldApps, let source = oldAppsSource else { return nil }
        if state.unavailable { return .unavailable }
        if let running = state.inFlight { return await running.value }
        let task = Task { await source.listOldApps() }
        state.inFlight = task
        state.begin()
        let answer = await task.value
        // The run ended meanwhile: its session, and its apps, are gone.
        guard session.oldApps === state, !task.isCancelled else { return nil }
        state.inFlight = nil
        state.apply(answer)
        return answer
    }

    /// The first listing of a run, once Android's storage is up, and again
    /// while Settings is open for as long as the virtual machine has not
    /// answered one.
    func listOldAppsOnce() async {
        guard session.storageReady == true, !session.oldApps.listed else { return }
        await refreshOldApps()
    }

    /// The person moved an app's switch. The switch itself stays where the
    /// image has it until the change is written. Turning it on is always
    /// asked; turning it off only when the app is open.
    func requestOldApp(_ package: String, on: Bool) async {
        let state = session.oldApps
        guard state.busy == nil, state.prompt == nil, let source = oldAppsSource,
              let app = state.apps.first(where: { $0.package == package }), app.isOn != on
        else { return }
        state.busy = package
        let open = await source.isOldAppOpen(package) == true
        guard session.oldApps === state, state.busy == package else { return }
        if on {
            state.prompt = .turnOn(app, open: open)
            state.busy = nil
        } else if open {
            state.prompt = .turnOff(app)
            state.busy = nil
        } else {
            await writeOldApp(app, on: false)
        }
    }

    /// The alert's answer. Cancel changes nothing.
    func answerOldApps(_ prompt: OldAppsPrompt, confirmed: Bool) async {
        let state = session.oldApps
        state.prompt = nil
        guard confirmed else { return }
        switch prompt {
        case let .turnOn(app, _): await writeOldApp(app, on: true)
        case let .turnOff(app): await writeOldApp(app, on: false)
        case .notChanged: break
        }
    }

    /// Writes one switch. The image's answer decides what the page shows: a
    /// change it took is recorded, and any other answer is said and the list
    /// is read again, since the app may be gone or no longer eligible.
    private func writeOldApp(_ app: OldApp, on: Bool) async {
        let state = session.oldApps
        guard let source = oldAppsSource else { return }
        state.busy = app.package
        let answer = await source.setOldApp(app.package, on: on)
        guard session.oldApps === state else { return }
        state.busy = nil
        switch answer {
        case .applied:
            state.record(app.package, on: on, at: Int(Date().timeIntervalSince1970))
        case .notEligible, .failed:
            state.prompt = .notChanged
            await refreshOldApps()
        }
    }

    /// After an Install anyway that succeeded for an app built for an Android
    /// before 4.0: whether the listing now names an app the install added or
    /// updated, which is the one Old app compatibility can help. `before` is
    /// the listing taken as the file was sent again.
    func installAddedOldApp(listedBefore before: [OldApp]) async -> Bool {
        guard case let .apps(after)? = await refreshOldApps() else { return false }
        return OldAppInstallHint.installedApp(listedBefore: before, listedAfter: after) != nil
    }

    /// The apps as the listing has them now, taken afresh: the baseline of
    /// an install that may add one. Nil when the virtual machine cannot say.
    func oldAppsBeforeInstall() async -> [OldApp]? {
        guard case let .apps(apps)? = await refreshOldApps() else { return nil }
        return apps
    }
}

/// The words of Old app compatibility. "Virtual machine", never "VM" or
/// "guest"; no promise that an app works; the app's own name and package
/// name are shown as they are.
nonisolated enum OldAppsWords {
    static let title = String(localized: "Old app compatibility")
    static let on = String(localized: "On")
    static let loading = String(localized: "Looking for old apps\u{2026}")
    static let sectionHeader = String(localized: "Built for Android before 4.0")
    static let empty = String(localized: "No app built for Android before 4.0 is installed.")
    static let failed = String(localized: "The virtual machine didn\u{2019}t answer. Try again.")
    static let tryAgain = String(localized: "Try again")
    static let turnOn = String(localized: "Turn on")
    static let turnOff = String(localized: "Turn off")
    static let cancel = String(localized: "Cancel")
    static let ok = String(localized: "OK")

    /// The page's footer: what the switch is for, then when to use it.
    static var footer: String {
        String(localized: "Some apps this old close as soon as they open, because they rely on how Android used to work. Old app compatibility makes Android run one app more like older versions did.")
            + "\n\n"
            + String(localized: "Turn it on only for an app that closes as soon as it opens: it can make an app that works today behave differently. Turning it on or off closes the app if it\u{2019}s open. Only apps it can apply to are listed.")
    }

    /// What VoiceOver reads for an app's switch.
    static func switchLabel(_ label: String) -> String {
        String(
            localized: "\(label), Old app compatibility",
            comment: "What VoiceOver reads for an app's switch on the Old app compatibility page: the app's name, then the feature's name.")
    }

    static func title(for prompt: OldAppsPrompt) -> String {
        switch prompt {
        case let .turnOn(app, _): String(localized: "Turn on for \u{201C}\(app.label)\u{201D}?")
        case let .turnOff(app): String(localized: "Turn off for \u{201C}\(app.label)\u{201D}?")
        case .notChanged: String(localized: "Old app compatibility wasn\u{2019}t changed")
        }
    }

    static func message(for prompt: OldAppsPrompt) -> String {
        switch prompt {
        case let .turnOn(app, open):
            let message = String(
                localized: "Android will run this app more like older versions did. This may help an app that closes as soon as it opens, but it can also make the app behave differently or close later on. You can turn it off again here.")
            return open ? Sentences.join(message, closesNow(app.label)) : message
        case let .turnOff(app):
            return Sentences.join(closesNow(app.label), String(localized: "It opens without old app compatibility next time."))
        case .notChanged:
            return String(localized: "The virtual machine didn\u{2019}t accept the change. Try again.")
        }
    }

    private static func closesNow(_ label: String) -> String {
        String(localized: "\u{201C}\(label)\u{201D} is open and closes now.")
    }

    /// The two sentences after Install anyway installed an old app: what it
    /// was built for, and where the switch is.
    static let installHint = String(
        localized: "It was built for an Android version before 4.0. If it closes as soon as it opens, open Settings here and choose Old app compatibility.")
}
