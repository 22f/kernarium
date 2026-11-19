// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import CoreSpotlight
import Foundation
import Testing
@testable import Andarium

/// What the application's index is told, in order, instead of the system's.
@MainActor
final class RecordingAppsIndex: AndroidAppsSearchIndexing {
    private(set) var offered: [[String]] = []
    private(set) var withdrawn = 0
    private(set) var shortcutUpdates = 0

    func offer(_ entities: [AndroidAppEntity]) async { offered.append(entities.map(\.id)) }
    func withdraw() async { withdrawn += 1 }
    func shortcutsChanged() { shortcutUpdates += 1 }
}

/// Add to Home Screen as the model plays it: the list a refresh keeps and what
/// it tells the system, a request made as an App Intent makes it while Android
/// runs, while it starts and while it is off, what a request that must wait
/// waits for, how each failure ends, and what an erase leaves. The runtime is
/// a stub and the tool is the played one (`ScriptedAndroidApps`).
@Suite(.serialized)
@MainActor
struct AndroidAppsModelTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000)
    private let system = SystemInfo(
        version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1")
    private let comet = ScriptedAndroidApps.installedApps[0]
    private let inkleaf = ScriptedAndroidApps.installedApps[1]

    private struct Rig {
        let model: AppModel
        let driver: StubDriver
        let scripted: ScriptedAndroidApps
        let index: RecordingAppsIndex
        let directory: URL
        var state: AndroidAppsState { model.androidApps }
        var tool: ScriptedAppsTool { scripted.tool }
    }

    /// A model over a stub runtime, a cache of its own and the played tool.
    /// Nothing is asked slowly: each answer takes a millisecond.
    private func rig(phase: Phase = .home, plan: StartPlan = .normal, guest: Bool = true) throws -> Rig {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-apps-test-\(UUID().uuidString)", isDirectory: true)
        let model = AppModel(
            guest: guest ? Guest.fresh(on: host) : nil, host: host, system: system, capabilities: .device,
            createRequirementBytes: 1)
        let driver = StubDriver(plan: plan)
        model.driver = driver
        model.plan = plan
        model.phase = phase
        model.session.storageReady = true
        let scripted = ScriptedAndroidApps()
        scripted.tool.delay = .milliseconds(1)
        scripted.readyDelay = .milliseconds(1)
        model.scriptedAndroidApps = scripted
        AndroidAppsHub.shared.configure(directory: directory)
        AndroidAppsHub.shared.attach(model)
        let index = RecordingAppsIndex()
        AndroidAppsHub.shared.searchIndex = index
        let state = model.androidApps
        state.store = AndroidAppsHub.shared.store
        state.preference = AndroidAppSearchPreference(defaults: try #require(UserDefaults(suiteName: "andarium-apps-test-\(UUID().uuidString)")))
        state.searchOn = true
        state.adoptCache()
        return Rig(model: model, driver: driver, scripted: scripted, index: index, directory: directory)
    }

    private func eventually(_ seconds: Double = 10, _ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(seconds)
        while ContinuousClock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(5))
        }
        return condition()
    }

    // MARK: The list

    @Test func aRefreshKeepsTheListAndItsIconsAndTellsTheSystem() async throws {
        let rig = try rig(phase: .running(.normal))
        #expect(await rig.model.refreshAndroidApps())
        let state = rig.state
        #expect(state.listed && !state.failed && !state.refreshing)
        #expect(state.apps.count == ScriptedAndroidApps.sampleApps.count)
        #expect(state.store.icon(forID: comet.id) != nil)
        // The cache answers without a model: the App Intent's query reads it.
        #expect(AndroidAppsHub.shared.entities().map(\.id) == state.apps.map(\.id))
        #expect(rig.index.offered.count == 1 && rig.index.offered[0] == state.apps.map(\.id))
        #expect(rig.index.shortcutUpdates == 1)
    }

    @Test func aRefreshOfAnUnchangedListAsksOnlyTheDigest() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        let listings = rig.tool.listingsAsked
        await rig.model.refreshAndroidApps()
        #expect(rig.tool.listingsAsked == listings)
        #expect(rig.tool.digestsAsked >= 2)
        #expect(rig.index.offered.count == 1, "nothing changed, so nothing is offered again")
    }

    @Test func anInstalledAppReachesTheListAndTheSystem() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        let extra = AndroidApp(
            component: try #require(AndroidComponent(packageName: "com.example.newcomer", activityClass: "com.example.newcomer.Main")),
            label: "Newcomer", builtIn: false, iconTag: "1")
        rig.tool.install(extra)
        await rig.model.refreshAndroidApps()
        #expect(rig.state.apps.contains { $0.id == extra.id })
        #expect(rig.index.offered.count == 2 && rig.index.offered[1].contains(extra.id))
    }

    @Test func noRefreshIsAskedWhereTheToolCannotBe() async throws {
        let home = try rig(phase: .home)
        #expect(await home.model.refreshAndroidApps() == false)
        let recovery = try rig(phase: .running(.recovery))
        #expect(await recovery.model.refreshAndroidApps() == false)
        let paused = try rig(phase: .running(.normal))
        paused.model.spacePause = SpacePause(freeBytes: 0, stage: .pausing)
        #expect(await paused.model.refreshAndroidApps() == false)
        #expect(paused.tool.digestsAsked == 0)
    }

    @Test func anImageWithoutTheToolOffersNothing() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.tool.answer(listing: .unavailable, digest: .unavailable)
        await rig.model.refreshAndroidApps()
        #expect(rig.state.unavailable)
        #expect(!rig.model.offersAddToHomeScreen)
    }

    @Test func aRefreshThatGotNoAnswerKeepsWhatWasCached() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        let before = rig.state.apps
        rig.tool.install(try #require(ScriptedAndroidApps.installedApps.first))  // the digest changes
        rig.tool.answer(listing: .failed, digest: .digest("changed"))
        await rig.model.refreshAndroidApps()
        #expect(rig.state.failed && rig.state.apps == before)
        #expect(rig.state.listedAt != nil)
    }

    // MARK: Search and Siri

    @Test func theSwitchWithdrawsTheAppsAndOffersThemAgain() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        rig.model.setAndroidAppsInSearch(false)
        #expect(await eventually { rig.index.withdrawn == 1 })
        #expect(!rig.state.searchOn && !rig.state.preference.isOn)
        // The list still reaches the shortcut tiles, which are the route to the Home Screen.
        #expect(AndroidAppsHub.shared.entities().count == rig.state.apps.count)
        rig.model.setAndroidAppsInSearch(true)
        #expect(await eventually { rig.index.offered.count == 2 })
        #expect(rig.state.preference.isOn)
    }

    @Test func aRefreshWhileTheSwitchIsOffOffersNothing() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.state.searchOn = false
        rig.tool.install(try #require(ScriptedAndroidApps.installedApps.last))
        await rig.model.refreshAndroidApps()
        #expect(rig.index.offered.isEmpty && rig.index.withdrawn == 1)
    }

    // MARK: A request while Android runs

    @Test func aRequestWhileAndroidRunsOpensTheAppAndClosesWhatCoversIt() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.model.sessionSettingsOpen = true
        rig.model.session.sideKeysOpen = true
        rig.model.requestAndroidApp(comet.component)
        #expect(!rig.model.sessionSettingsOpen && !rig.model.session.sideKeysOpen)
        #expect(rig.state.open.pending?.stage == .opening)
        #expect(await eventually { rig.tool.opened == [comet.component] })
        #expect(await eventually { rig.state.open.pending == nil })
        #expect(rig.state.open.banner == nil)
        #expect(rig.driver.begun.isEmpty, "Android runs: nothing is started")
        #expect(rig.scripted.readinessAsked == 1)
    }

    @Test func aRequestNamesTheAppAsItsEntityDid() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.scripted.readyDelay = .seconds(3600)
        #expect(rig.model.requestAndroidApp(identifier: comet.id, label: "Comet Hop"))
        #expect(rig.state.open.pending?.request.label == "Comet Hop")
        #expect(rig.state.open.pending?.request.component == comet.component)
        rig.model.dontOpenAndroidApp()
    }

    @Test func aValueThatIsNoComponentIsNoRequest() throws {
        let rig = try rig(phase: .running(.normal))
        #expect(!rig.model.requestAndroidApp(identifier: "not-a-component"))
        #expect(!rig.model.requestAndroidApp(identifier: ""))
        #expect(rig.state.open.pending == nil)
    }

    @Test func aNewerRequestReplacesAnOlderOne() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.scripted.readyDelay = .milliseconds(200)
        rig.model.requestAndroidApp(comet.component)
        rig.model.requestAndroidApp(inkleaf.component)
        #expect(await eventually { rig.state.open.pending == nil })
        #expect(rig.tool.opened == [inkleaf.component])
    }

    @Test func dontOpenDropsTheRequestAndNothingIsOpenedLater() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.scripted.readyDelay = .milliseconds(100)
        rig.model.requestAndroidApp(comet.component)
        rig.model.dontOpenAndroidApp()
        #expect(rig.state.open.pending == nil)
        try await Task.sleep(for: .milliseconds(300))
        #expect(rig.tool.opened.isEmpty)
    }

    @Test func theRunEndingDropsTheRequestAndTheBanner() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.scripted.readyDelay = .seconds(3600)
        rig.model.requestAndroidApp(comet.component)
        rig.model.runEnded(RunEnd(result: 0), uptimeSeconds: nil)
        #expect(rig.state.open.pending == nil && rig.state.open.banner == nil)
        #expect(!rig.state.listed)
        #expect(rig.tool.opened.isEmpty)
    }

    // MARK: A request while Android is off

    @Test func aRequestWhileAndroidIsOffStartsItAsStartDoesAndOpensTheAppWhenItIsUp() async throws {
        let rig = try rig(phase: .home, plan: .normal)
        rig.model.requestAndroidApp(comet.component)
        #expect(rig.driver.begun.count == 1 && rig.driver.begun.first?.mode == .normal)
        #expect(rig.model.phase == .booting(.normal))
        #expect(rig.state.open.pending?.stage == .starting)
        #expect(rig.tool.opened.isEmpty)
        rig.model.phase = .running(.normal)
        rig.model.finishBoot(.normal)
        #expect(await eventually { rig.tool.opened == [comet.component] })
        #expect(await eventually { rig.state.open.pending == nil })
    }

    @Test func aRequestWhileAndroidIsStartingJoinsTheStart() throws {
        let rig = try rig(phase: .booting(.normal))
        rig.model.requestAndroidApp(comet.component)
        #expect(rig.driver.begun.isEmpty, "no second start")
        #expect(rig.state.open.pending?.stage == .starting)
    }

    @Test func aRequestWaitsAtHomeForAnUnfinishedUpdateAndStartsWithTheAnswer() async throws {
        let rig = try rig(phase: .home, plan: .unfinishedUpdate(to: "r2", canRetry: true, canRestore: true))
        rig.model.requestAndroidApp(comet.component)
        guard case .waiting(.update, _)? = rig.state.open.pending?.stage else {
            Issue.record("the request should wait for the answer to the update: \(String(describing: rig.state.open.pending))")
            return
        }
        #expect(rig.driver.begun.isEmpty)
        // The person answers: Update and start.
        rig.model.plan = .update(from: "r1", to: "r2")
        rig.model.start(.normal)
        #expect(rig.driver.begun.count == 1)
        #expect(rig.state.open.pending?.stage == .starting)
    }

    @Test func aRequestWaitsAtHomeWhileThereIsNoRoomToStart() throws {
        var tight = host
        tight.freeStorageBytes = 1_000_000
        let model = AppModel(guest: Guest.fresh(on: tight), host: tight, system: system, capabilities: .device, createRequirementBytes: 1)
        model.driver = StubDriver(plan: .normal)
        model.plan = .normal
        model.scriptedAndroidApps = ScriptedAndroidApps()
        AndroidAppsHub.shared.configure(
            directory: FileManager.default.temporaryDirectory.appendingPathComponent("andarium-apps-test-\(UUID().uuidString)"))
        model.androidApps.store = AndroidAppsHub.shared.store
        model.requestAndroidApp(comet.component)
        guard case .waiting(.space, _)? = model.androidApps.open.pending?.stage else {
            Issue.record("the request should wait for space: \(String(describing: model.androidApps.open.pending))")
            return
        }
    }

    @Test func aRequestWithNoVirtualMachineSaysTheAppIsNotInAndroid() throws {
        let rig = try rig(phase: .home, plan: .firstStart, guest: false)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(rig.model.alert?.title == AndroidAppsWords.notInAndroidTitle("Comet Hop"))
        #expect(rig.model.alert?.message == AndroidAppsWords.noAndroidText)
        #expect(rig.state.open.pending == nil)
    }

    @Test func aRequestInRecoveryIsRefusedWithABanner() throws {
        let rig = try rig(phase: .running(.recovery))
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(rig.state.open.banner?.failure == .recovery)
        #expect(rig.state.open.pending == nil && rig.tool.opened.isEmpty)
    }

    // MARK: How an open ends

    @Test func anAppThatIsGoneEndsWithABannerAndAListAskedAgain() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        rig.tool.remove(comet.id)
        rig.model.requestAndroidApp(comet.component)
        #expect(await eventually { rig.state.open.banner?.failure == .notInAndroid })
        #expect(rig.state.open.banner?.app == comet.component)
        #expect(rig.state.open.banner?.component == nil, "no Try Again for an app that is not there")
        // The cache was dropped and read again, so the app is out of the list.
        #expect(await eventually { !rig.state.apps.contains { $0.id == comet.id } && rig.state.listed })
    }

    @Test func androidTakingTooLongEndsWithABannerAndTryAgainOpensTheApp() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.tool.answer(open: comet.component, .failed)
        rig.model.requestAndroidApp(comet.component)
        #expect(await eventually { rig.state.open.banner?.failure == .tookTooLong })
        rig.tool.answer(open: comet.component, .opened)
        rig.model.retryAndroidAppOpen()
        #expect(rig.state.open.banner == nil)
        #expect(await eventually { rig.tool.opened.count == 2 && rig.state.open.pending == nil })
        #expect(rig.state.open.banner == nil)
    }

    @Test func androidNeverBecomingReadyEndsWithTheSameBanner() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.scripted.readiness = .timedOut
        rig.model.requestAndroidApp(comet.component)
        #expect(await eventually { rig.state.open.banner?.failure == .tookTooLong })
        #expect(rig.tool.opened.isEmpty)
    }

    @Test func androidsUnfinishedSetupEndsWithABannerThatOffersTryAgain() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.scripted.readiness = .setupUnfinished
        rig.model.requestAndroidApp(comet.component)
        #expect(await eventually { rig.state.open.banner?.failure == .setupUnfinished })
        #expect(rig.state.open.banner?.component == comet.component)
        #expect(rig.tool.opened.isEmpty)
        rig.model.dismissAndroidAppBanner()
        #expect(rig.state.open.banner == nil)
    }

    // MARK: Erase and the entities

    @Test func anEraseDropsTheCacheAndKeepsTheRecordOfEachIcon() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        #expect(rig.state.store.record(forID: comet.id)?.label == "Comet Hop")
        rig.model.androidAppsErased()
        #expect(rig.state.apps.isEmpty && rig.state.store.catalog() == nil)
        #expect(AndroidAppsHub.shared.entities().isEmpty)
        #expect(rig.state.store.record(forID: comet.id)?.label == "Comet Hop")
        // What was offered to Search is replaced by nothing.
        #expect(await eventually { rig.index.offered.last == [] })
    }

    @Test func anIconAfterAnEraseStillResolvesToItsAppAndWorksAfterAReinstall() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        rig.model.androidAppsErased()
        let query = AndroidAppQuery()
        let leftover = try await query.entities(for: [comet.id])
        #expect(leftover.map(\.name) == ["Comet Hop"], "the leftover icon keeps its app's name")
        // The app is installed in a new Android: the same identifier opens it again.
        rig.model.phase = .running(.normal)
        rig.model.session.storageReady = true
        await rig.model.refreshAndroidApps()
        let again = try await query.entities(for: [comet.id])
        #expect(again.map(\.id) == [comet.id])
        #expect(rig.model.requestAndroidApp(identifier: again[0].id, label: again[0].name))
        #expect(await eventually { rig.tool.opened == [comet.component] })
    }

    @Test func theQueryAnswersFromTheCacheWhileAndroidIsOff() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        rig.model.phase = .home
        let query = AndroidAppQuery()
        let all = try await query.allEntities()
        #expect(all.count == ScriptedAndroidApps.sampleApps.count)
        #expect(all.map(\.id) == rig.state.apps.map(\.id))
        #expect(try await query.suggestedEntities().count == all.count)
        // An identifier that is no component names nothing.
        #expect(try await query.entities(for: ["nothing"]).isEmpty)
    }

    @Test func anEntityCarriesTheAppsNameItsIdentifierAndItsIcon() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        let entity = try #require(AndroidAppsHub.shared.entities().first { $0.id == comet.id })
        #expect(entity.name == "Comet Hop")
        #expect(entity.id == "com.example.comethop/com.example.comethop.MainActivity")
        #expect(entity.attributeSet.title == "Comet Hop")
        #expect(entity.attributeSet.thumbnailData != nil)
    }

    // MARK: The row

    @Test func theRowIsOfferedOnlyWhileAndroidRuns() throws {
        let running = try rig(phase: .running(.normal))
        #expect(running.model.offersAddToHomeScreen)
        for phase in [Phase.home, .booting(.normal), .running(.recovery), .failed] {
            let other = try rig(phase: phase)
            #expect(!other.model.offersAddToHomeScreen, "no row in \(phase)")
        }
    }

    @Test func aMacHasNoHomeScreenToAddTo() throws {
        let rig = try rig(phase: .running(.normal))
        rig.model.host.idiom = .mac
        #expect(!rig.model.offersAddToHomeScreen)
    }
}
