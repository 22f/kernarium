// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// Add to Home Screen: the list of Android's apps, and a request to open one.
///
/// The list is the cache's (`AndroidAppStore`), brought up to date from the
/// tool in Android's image while Android runs. A request comes from an App
/// Intent (a Home Screen icon, Shortcuts, Search, Siri) and follows the Start
/// button's own rules (`AppOpenPlanner`): a running Android opens the app once
/// it is ready for it, a stopped one is started as Start starts it, and a start
/// that needs the person's answer first waits for it at Home. The request lives
/// in memory only.
extension AppModel {
    // MARK: What is offered

    /// Where the list and the opening come from: the runtime, or the list a
    /// preview or a UI test's probe session plays.
    var androidAppsSource: (any AndroidAppsSource)? {
        #if !APP_STORE
        if let scriptedAndroidApps { return scriptedAndroidApps }
        #endif
        return driver
    }

    /// The tool can be asked now: a normal session, not paused for space.
    var canAskAndroidApps: Bool {
        guard case .running(.normal) = phase, spacePause == nil else { return false }
        return androidAppsSource?.androidAppsTool != nil
    }

    /// Settings' Apps section offers Add to Home Screen: Android runs, and its
    /// image has a list of apps to offer. A Mac and a Vision Pro have no Home
    /// Screen to add to.
    var offersAddToHomeScreen: Bool {
        guard case .running(.normal) = phase, androidAppsSource != nil, !androidApps.unavailable else { return false }
        return host.idiom != .mac && !ProcessInfo.processInfo.isiOSAppOnVision
    }

    // MARK: The list

    /// Brings the cache up to what Android lists now: asks whether the apps
    /// changed, lists them if they did and fetches the icons that are missing.
    /// A refresh already under way is joined. Returns whether one was asked.
    @discardableResult
    func refreshAndroidApps() async -> Bool {
        let state = androidApps
        guard canAskAndroidApps, !state.unavailable, let tool = androidAppsSource?.androidAppsTool else { return false }
        if let running = state.inFlight {
            _ = await running.value
            return true
        }
        let run = session
        let flat = FlatteningAppsTool(base: tool)
        let store = state.store
        let before = state.apps
        let task = Task.detached {
            await AndroidAppRefresh.run(tool: flat, store: store) { catalog, left in
                Task { @MainActor in state.progressed(catalog, iconsLeft: left) }
            }
        }
        state.inFlight = task
        state.begin()
        let result = await task.value
        // The run ended meanwhile: its session, and its refresh, are gone.
        guard session === run, state.inFlight == task else { return true }
        state.inFlight = nil
        state.finished(result)
        if case .refreshed = result, state.apps != before {
            await syncAndroidAppsIndex()
        }
        return true
    }

    /// Android is up (the boot screen ends): the run lists the apps once, a few
    /// seconds after its storage is up so that the end of the boot is not
    /// slowed, and again a few times if it got no answer, each after twice the
    /// wait of the one before; then the task ends, as it does at the first
    /// answer, so that nothing keeps waking an idle run.
    func refreshAndroidAppsAtStart() {
        let state = androidApps
        guard state.first == nil, !state.listed, canAskAndroidApps else { return }
        state.first = Task { [weak self, weak state] in
            for attempt in 0..<AndroidAppsState.startAttempts {
                if let state {
                    let wait = attempt == 0 ? state.startDelay : state.retryInterval * (1 << (attempt - 1))
                    try? await Task.sleep(for: wait)
                }
                guard let self, let state, !Task.isCancelled, self.androidApps === state else { return }
                // An app that is being opened has Android's attention first.
                var opening = false
                if case .opening? = state.open.pending?.stage { opening = true }
                let storageUp = if let driver = self.driver { await driver.storageReady() == true } else { true }
                if !opening, storageUp { await self.refreshAndroidApps() }
                if state.listed { break }
            }
            state?.first = nil
        }
    }

    /// Tells the system what Android's apps are now: the tiles in Shortcuts, and
    /// Spotlight (Search and Siri) while the switch is on.
    func syncAndroidAppsIndex() async {
        let index = AndroidAppsHub.shared.searchIndex
        index.shortcutsChanged()
        if androidApps.searchOn {
            await index.offer(androidApps.apps.map(AndroidAppEntity.init))
        } else {
            await index.withdraw()
        }
    }

    /// The Search and Siri switch.
    func setAndroidAppsInSearch(_ on: Bool) {
        let state = androidApps
        guard state.searchOn != on else { return }
        state.searchOn = on
        state.preference.keep(on)
        Task { await syncAndroidAppsIndex() }
    }

    /// Android was erased: the cache and the apps offered are gone; the record
    /// of each icon stays, so a leftover icon can say its app is not in Android.
    func androidAppsErased() {
        androidApps.erased()
        Task { await syncAndroidAppsIndex() }
    }

    // MARK: A request to open an app

    /// What is true now, for the planner.
    private var androidAppOpenSituation: AppOpenSituation {
        AppOpenSituation(
            phase: phase, plan: plan, verdict: lifecycle.startVerdict(.normal, host: host, plan: plan),
            holdsGuestFiles: snapshotActivity.holdsGuestFiles, spacePaused: spacePause != nil)
    }

    /// A request to open `component`, from an App Intent or Try Again. The
    /// name is the one the request knew (the entity's), else the cache's.
    func requestAndroidApp(_ component: AndroidComponent, label: String? = nil) {
        let state = androidApps
        let known = label ?? state.store.catalog()?.app(withID: component.identifier)?.label
            ?? state.store.record(forID: component.identifier)?.label
        let name = AndroidAppText.label(known ?? "", fallback: component.packageName)
        // What covers Android goes: the app opens where the person looks.
        if case .running = phase {
            sessionSettingsOpen = false
            session.sideKeysOpen = false
        }
        let effect = state.open.request(component, label: name, in: androidAppOpenSituation, now: Date())
        performAndroidAppEffect(effect)
    }

    /// The request as an App Intent hands it over: its entity's identifier. A
    /// value that is no component is no request.
    @discardableResult
    func requestAndroidApp(identifier: String, label: String? = nil) -> Bool {
        guard let component = AndroidComponent(identifier: identifier) else { return false }
        requestAndroidApp(component, label: label)
        return true
    }

    private func performAndroidAppEffect(_ effect: AppOpenEffect) {
        switch effect {
        case .none:
            break
        case .startAndroid:
            startAndroidForRequest()
        case let .open(request):
            beginOpeningAndroidApp(request)
        case let .alert(refusal, label):
            switch refusal {
            case .needsNewerChip:
                alert = AppAlert(title: DeviceRequirement.title, message: DeviceRequirement.explanation(deviceName: host.name))
            default:
                alert = AppAlert(
                    title: AndroidAppsWords.notInAndroidTitle(label), message: AndroidAppsWords.noAndroidText)
            }
        }
        androidApps.scheduleExpiry { }
    }

    /// Starts Android as the Start button does. A start that is refused after
    /// all drops the request: nothing is left waiting for a boot that is not coming.
    private func startAndroidForRequest() {
        start(.normal)
        if case .booting = phase { return }
        androidApps.open.drop()
    }

    /// Android is up for a request: waits until it is ready for the app, then
    /// opens it. The run ending, or another request, ends the wait.
    private func beginOpeningAndroidApp(_ request: AppOpenRequest) {
        let state = androidApps
        state.opening?.cancel()
        state.opening = Task { [weak self] in
            guard let self else { return }
            let run = session
            let source = androidAppsSource
            let outcome = await source?.awaitAndroidReady() ?? .timedOut
            guard session === run, !Task.isCancelled, state.open.pending?.request == request else { return }
            switch outcome {
            case .setupUnfinished:
                state.open.setupUnfinished(for: request, now: Date())
            case .timedOut:
                state.open.opened(.failed, for: request, now: Date())
            case .ready:
                let result: AndroidAppOpenOutcome
                if let tool = source?.androidAppsTool {
                    result = await tool.open(request.component)
                } else {
                    result = .failed
                }
                guard session === run, !Task.isCancelled else { return }
                state.open.opened(result, for: request, now: Date())
                // An app that is gone means the cached list is old.
                if result == .notInAndroid { state.invalidated(); Task { await self.refreshAndroidApps() } }
            }
            state.scheduleExpiry { }
        }
    }

    /// The boot reached Android: a request that was starting opens now.
    func androidAppsBootFinished(recovery: Bool) {
        let effect = androidApps.open.androidIsUp(recovery: recovery, now: Date())
        performAndroidAppEffect(effect)
        if !recovery { refreshAndroidAppsAtStart() }
    }

    /// The person answered what Home asked (Start, Update and start): a
    /// request that waited is starting now.
    func androidAppsPersonStarted() {
        androidApps.open.personStarted()
    }

    /// What held a request ended: a snapshot operation, or the pause for space.
    func androidAppsHoldEnded(_ hold: AppOpenHold) {
        let effect = androidApps.open.releaseHold(hold, in: androidAppOpenSituation, now: Date())
        performAndroidAppEffect(effect)
    }

    /// "Don't open": the request is dropped, and Android keeps starting.
    func dontOpenAndroidApp() {
        androidApps.opening?.cancel()
        androidApps.opening = nil
        androidApps.open.drop()
    }

    /// The failure banner's close button.
    func dismissAndroidAppBanner() {
        androidApps.open.dismissBanner()
    }

    /// The failure banner's Try Again: the request is made again.
    func retryAndroidAppOpen() {
        guard let banner = androidApps.open.banner, let component = banner.component else { return }
        androidApps.open.dismissBanner()
        requestAndroidApp(component, label: banner.label)
    }

    /// The run is over or Android restarts: nothing waits for it, and what the
    /// run listed is not known of the next.
    func androidAppsRunEnded() {
        androidApps.runEnded()
    }
}
