// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The boot screen's Magisk app step: what it asks the guest, when it
/// installs, and the 45 second bound on the check and the install together.
struct RootAppStepTests {
    private let release = RootRelease(versionName: "v31.1", versionCode: 31_100, apkPath: "root/Magisk-v31.1.apk")
    private var rooted: RootState {
        var state = RootState()
        state.on = true
        return state
    }

    private func shell(_ probe: String, exit: Int = 0) -> RootScriptedShell {
        RootScriptedShell { command in
            command == MagiskApp.probeCommand ? GuestShellOutput(text: probe, exitCode: exit) : GuestShellOutput(text: "", exitCode: 0)
        }
    }

    @Test func anAbsentAppIsInstalledAndOpened() async {
        let shell = shell("")
        let showed = RootCounter()
        let installed = RootCounter()
        let outcome = await RootAppStep.run(
            shell: shell, release: release, state: rooted,
            installing: { await showed.bump() }, install: { await installed.bump() })
        guard case .installed(let state) = outcome else { Issue.record("not installed: \(outcome)"); return }
        #expect(state.appVersionCode == 31_100 && state.appEverHandled && state.on)
        let shown = await showed.value
        let done = await installed.value
        #expect(shown == 1 && done == 1)
        #expect(shell.commands == [MagiskApp.probeCommand, MagiskApp.openCommand])
    }

    @Test func aCurrentAppIsLeftAloneAndRemembered() async {
        let shell = shell("package:com.topjohnwu.magisk versionCode:31100\n")
        let showed = RootCounter()
        let outcome = await RootAppStep.run(
            shell: shell, release: release, state: rooted,
            installing: { await showed.bump() }, install: { Issue.record("installed a current app") })
        guard case .notNeeded(let state) = outcome else { Issue.record("\(outcome)"); return }
        #expect(state.appVersionCode == 31_100 && state.appEverHandled)
        let shown = await showed.value
        #expect(shown == 0, "no step is shown for a boot that installs nothing")
        #expect(shell.commands == [MagiskApp.probeCommand])
    }

    @Test func anOlderAppIsUpdated() async {
        let outcome = await RootAppStep.run(
            shell: shell("package:com.topjohnwu.magisk versionCode:27000\n"), release: release, state: rooted,
            installing: {}, install: {})
        guard case .installed(let state) = outcome else { Issue.record("\(outcome)"); return }
        #expect(state.appVersionCode == 31_100)
    }

    @Test func anAppThePersonRemovedStaysRemovedWithinThePeriod() async {
        var state = rooted
        state.appVersionCode = 31_100
        let showed = RootCounter()
        let outcome = await RootAppStep.run(
            shell: shell(""), release: release, state: state,
            installing: { await showed.bump() }, install: { Issue.record("installed again") })
        #expect(outcome == .notNeeded(state))
        let shown = await showed.value
        #expect(shown == 0)
    }

    @Test func aFailedInstallEndsTheStepAsAFailure() async {
        struct Refused: Error {}
        let shell = shell("")
        let outcome = await RootAppStep.run(
            shell: shell, release: release, state: rooted, installing: {}, install: { throw Refused() })
        #expect(outcome == .failed)
        #expect(!shell.commands.contains(MagiskApp.openCommand), "nothing is opened after a failure")
    }

    @Test func theBoundEndsAnInstallThatNeverFinishes() async {
        let started = ContinuousClock.now
        let outcome = await RootAppStep.run(
            shell: shell(""), release: release, state: rooted, installing: {},
            install: { try await Task.sleep(for: .seconds(60)) }, limit: .milliseconds(200))
        #expect(outcome == .failed)
        #expect(ContinuousClock.now - started < .seconds(10))
    }

    @Test func theBoundIsReleasedEvenWhenTheWorkIgnoresItsCancellation() async {
        let started = ContinuousClock.now
        let outcome = await RootAppStep.run(
            shell: shell(""), release: release, state: rooted, installing: {},
            install: {
                // A wait that a cancel does not end.
                let deadline = ContinuousClock.now + .seconds(3)
                while ContinuousClock.now < deadline { await Task.yield() }
            },
            limit: .milliseconds(200))
        #expect(outcome == .failed)
        #expect(ContinuousClock.now - started < .seconds(2))
    }

    @Test func theBoundIsFortyFiveSeconds() {
        #expect(RootAppStep.limit == .seconds(45))
    }

    @Test func aGuestThatCannotBeAskedGetsTheInstallAndItsFailureIsReported() async {
        struct Unreachable: Error {}
        let shell = RootScriptedShell { _ in throw Unreachable() }
        let outcome = await RootAppStep.run(
            shell: shell, release: release, state: rooted, installing: {}, install: { throw Unreachable() })
        #expect(outcome == .failed)
    }

    @Test func theBootScreenHasAFifthStepOnlyForTheInstall() {
        #expect(BootSteps.count == 4 && BootSteps.countWithRootApp == 5)
        #expect(BootSteps.reportName(ofStep: BootSteps.rootAppStep, recovery: false) == "Magisk app")
        #expect(BootSteps.reportName(ofStep: 3, recovery: false) == "Startup")
    }
}
