// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// Settings' "120 Hz" in the model, in what it keeps, in what a launch states
/// of it, in the boot request and in what Low Power Mode does to the guest's
/// render rate. The setting itself is drawn by `SettingsOrderTests`.
@MainActor
struct HighRefreshTests {
    private func model() -> AppModel {
        AppModel(
            guest: nil, host: AppEntry.hostDevice(),
            system: SystemInfo(version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
    }

    private func withDefaults(_ body: (UserDefaults) async throws -> Void) async rethrows {
        let name = "andarium-tests-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defer { defaults.removePersistentDomain(forName: name) }
        try await body(defaults)
    }

    // MARK: The model

    @Test func theSettingIsOffAndNotOfferedUntilTheScreenSaysItShowsMoreThanSixty() {
        let model = model()
        #expect(!model.highRefreshOn)
        #expect(!model.highRefreshOffered)
        #expect(model.screenMaximumFramesPerSecond == 60)
        model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: 120)
        #expect(model.highRefreshOffered)
        // A reader of the window that does not read the rate leaves it.
        model.screenDidChange(nativeScale: 3)
        #expect(model.screenMaximumFramesPerSecond == 120)
        model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: 60)
        #expect(!model.highRefreshOffered)
    }

    @Test func aStatedScreenWinsOverTheReadOne() {
        let model = model()
        model.statedMaximumFramesPerSecond = 120
        model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: 60)
        #expect(model.highRefreshOffered)
    }

    @Test func aStartAsksForTheDisplayOnlyWhenTheSettingIsOnAndTheScreenCanShowIt() {
        let model = model()
        model.setHighRefresh(true)
        #expect(model.highRefreshOn)
        #expect(!model.asksForHighRefresh, "a 60 Hz screen asked for 120 Hz")
        model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: 120)
        #expect(model.asksForHighRefresh)
        model.setHighRefresh(false)
        #expect(!model.asksForHighRefresh)
    }

    @Test func aChangeWhileAndroidRunsWaitsForTheNextStart() {
        let model = model()
        model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: 120)
        model.phase = .running(.normal)
        model.runStarted(withHighRefresh: false)
        #expect(!model.highRefreshWaitsForRestart)
        model.setHighRefresh(true)
        #expect(model.highRefreshWaitsForRestart, "the running display cannot change")
        // The run's display is still the one it started with.
        #expect(!model.runHasHighRefresh)
        model.setHighRefresh(false)
        #expect(!model.highRefreshWaitsForRestart)
        // After a start that asked, turning it off waits too.
        model.setHighRefresh(true)
        model.runStarted(withHighRefresh: true)
        #expect(!model.highRefreshWaitsForRestart)
        model.setHighRefresh(false)
        #expect(model.highRefreshWaitsForRestart)
        // Nothing waits outside a run.
        model.phase = .home
        #expect(!model.highRefreshWaitsForRestart)
    }

    // MARK: What is kept

    @Test func theChoiceIsWrittenWhenMadeAndASecondSessionReadsIt() async throws {
        try await withDefaults { defaults in
            let model = model()
            #expect(model.highRefreshToKeep == false)
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            model.setHighRefresh(true)
            let write = try #require(keeper.pending, "the change queued no write")
            await write.value
            #expect(HighRefreshPreference(defaults: defaults).isOn)
            let next = HighRefreshPreference(defaults: defaults).launch(automated: false)
            #expect(next.isOn && next.keepsChanges)
        }
    }

    @Test func aLaunchThatDecidedTheSettingLeavesTheStoredChoice() async {
        await withDefaults { defaults in
            HighRefreshPreference(defaults: defaults).keep(true)
            let model = model()
            model.highRefreshOn = false
            model.highRefreshDecidedByLaunch = true
            #expect(model.highRefreshToKeep == nil)
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()
            model.setHighRefresh(true)
            model.setHighRefresh(false)
            model.session.showStats = true
            await keeper.pending?.value
            keeper.keep()
            #expect(HighRefreshPreference(defaults: defaults).isOn, "a harness launch replaced the person's choice")
        }
    }

    // MARK: A launch

    @Test func aLaunchStatesTheSettingWithOneOrZero() {
        #expect(HighRefreshLaunch.stated(in: ["ANDARIUM_HIGH_REFRESH": "1"]) == true)
        #expect(HighRefreshLaunch.stated(in: ["ANDARIUM_HIGH_REFRESH": "0"]) == false)
        #expect(HighRefreshLaunch.stated(in: ["ANDARIUM_HIGH_REFRESH": "yes"]) == nil)
        #expect(HighRefreshLaunch.stated(in: [:]) == nil)
    }

    @Test func aLaunchThatStatesTheSettingIsAHarnessLaunch() {
        let overrides = LaunchOverrides(environment: [:], processorCount: 4)
        #expect(AppEntry.isPersonsLaunch(overrides: overrides, environment: [:]))
        #expect(!AppEntry.isPersonsLaunch(overrides: overrides, environment: ["ANDARIUM_HIGH_REFRESH": "1"]))
    }

    // MARK: The boot request

    @Test func theBootRequestCarriesTheBitOnlyWhenAsked() {
        let bit = UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_HIGH_REFRESH)
        let haptics = UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS)
        #expect(bit == 8)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: false) == 0)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: false, highRefresh: false) == 0)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: false, highRefresh: true) == bit)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: true, highRefresh: true) == haptics | bit)
    }

    // MARK: Low Power Mode

    @Test func theLowPowerMonitorReadsNowOnAChangeAndOnRefreshAndSaysNoneWhenItStops() {
        let center = NotificationCenter()
        var states = [false, true, false]
        var delivered: [Bool] = []
        let monitor = LowPowerMonitor(
            center: center, read: { states.isEmpty ? false : states.removeFirst() },
            deliver: { delivered.append($0) })
        monitor.start()
        #expect(delivered == [false])
        monitor.refresh()
        #expect(delivered == [false, true])
        center.post(name: .NSProcessInfoPowerStateDidChange, object: nil)
        #expect(delivered == [false, true, false])
        monitor.stop()
        // Stopping lifts any hold, and a stopped monitor reads nothing.
        #expect(delivered == [false, true, false, false])
        center.post(name: .NSProcessInfoPowerStateDidChange, object: nil)
        #expect(delivered.count == 4, "a stopped monitor read the power state")
    }
}
