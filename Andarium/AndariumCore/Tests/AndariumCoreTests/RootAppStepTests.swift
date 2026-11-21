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

    /// A wait that a cancel does not end and that holds no thread, so that a
    /// suite running beside others is not starved by it.
    private static func waitThatACancelDoesNotEnd(seconds: Double) async {
        await withCheckedContinuation { continuation in
            DispatchQueue.global().asyncAfter(deadline: .now() + seconds) { continuation.resume() }
        }
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

    @Test func anAppTheApplicationPutInForThisReleaseIsLeftAlone() async {
        var state = rooted
        state.appVersionCode = 31_100
        let shell = shell("package:com.topjohnwu.magisk versionCode:31100\n")
        let showed = RootCounter()
        let outcome = await RootAppStep.run(
            shell: shell, release: release, state: state,
            installing: { await showed.bump() }, install: { Issue.record("installed a current app") })
        #expect(outcome == .notNeeded)
        let shown = await showed.value
        #expect(shown == 0, "no step is shown for a boot that installs nothing")
        #expect(shell.commands == [MagiskApp.probeCommand])
    }

    @Test func aPackageTheStateDoesNotSayWePutInIsInstalledOverNotRecordedAsOurs() async {
        // The same package name and version code as the release's: only its
        // signature says whose it is, and the package manager checks that.
        for line in ["package:com.topjohnwu.magisk versionCode:31100\n"] {
            let shell = shell(line)
            let installed = RootCounter()
            let outcome = await RootAppStep.run(
                shell: shell, release: release, state: rooted,
                installing: {}, install: { await installed.bump() })
            guard case .installed(let state) = outcome else { Issue.record("\(line): \(outcome)"); continue }
            let done = await installed.value
            #expect(done == 1, Comment(rawValue: line))
            #expect(state.appInstall == .init(reason: .notRecorded, installed: true), Comment(rawValue: line))
            #expect(state.appVersionCode == 31_100)
            #expect(!shell.commands.contains { $0.contains("uninstall") }, "the person's app is never removed")
        }
    }

    @Test func aNewerPackageTheStateDoesNotRecordIsLeftAloneAtEveryStart() async {
        // The person's newer official app, after root was turned off and on or a snapshot was restored:
        // Android would refuse the downgrade, so no install is tried and no alert would follow.
        for boot in 1...2 {
            let shell = shell("package:com.topjohnwu.magisk versionCode:31200\n")
            let showed = RootCounter()
            let outcome = await RootAppStep.run(
                shell: shell, release: release, state: rooted,
                installing: { await showed.bump() }, install: { Issue.record("a newer app was installed over") })
            #expect(outcome == .notNeeded, "boot \(boot)")
            let shown = await showed.value
            #expect(shown == 0, "boot \(boot)")
            #expect(shell.commands == [MagiskApp.probeCommand], "boot \(boot)")
        }
    }

    @Test func aSignatureMismatchFailsTheInstallAndTheReasonIsKept() async {
        struct Incompatible: Error {}
        let outcome = await RootAppStep.run(
            shell: shell("package:com.topjohnwu.magisk versionCode:31100\n"), release: release, state: rooted,
            installing: {}, install: { throw Incompatible() })
        guard case .failed(let state) = outcome else { Issue.record("\(outcome)"); return }
        #expect(state.appInstall == .init(reason: .notRecorded, installed: false))
        #expect(state.appVersionCode == nil, "a failed install records no version")
    }

    @Test func anOlderAppIsUpdated() async {
        let outcome = await RootAppStep.run(
            shell: shell("package:com.topjohnwu.magisk versionCode:27000\n"), release: release, state: rooted,
            installing: {}, install: {})
        guard case .installed(let state) = outcome else { Issue.record("\(outcome)"); return }
        #expect(state.appVersionCode == 31_100)
        #expect(state.appInstall == .init(reason: .older, installed: true))
    }

    @Test func anAppThePersonRemovedStaysRemovedWithinThePeriod() async {
        var state = rooted
        state.appVersionCode = 31_100
        let showed = RootCounter()
        let outcome = await RootAppStep.run(
            shell: shell(""), release: release, state: state,
            installing: { await showed.bump() }, install: { Issue.record("installed again") })
        #expect(outcome == .notNeeded)
        let shown = await showed.value
        #expect(shown == 0)
    }

    @Test func aFailedInstallEndsTheStepAsAFailure() async {
        struct Refused: Error {}
        let shell = shell("")
        let outcome = await RootAppStep.run(
            shell: shell, release: release, state: rooted, installing: {}, install: { throw Refused() })
        #expect(outcome.isFailure)
        #expect(!shell.commands.contains(MagiskApp.openCommand), "nothing is opened after a failure")
    }

    @Test func theBoundEndsAnInstallThatNeverFinishes() async {
        let started = ContinuousClock.now
        let outcome = await RootAppStep.run(
            shell: shell(""), release: release, state: rooted, installing: {},
            install: { try await Task.sleep(for: .seconds(60)) }, limit: .milliseconds(200))
        #expect(outcome.isFailure)
        #expect(ContinuousClock.now - started < .seconds(10))
    }

    @Test func theBoundIsReleasedEvenWhenTheWorkIgnoresItsCancellation() async {
        // The waits are long against the bound and the checks are loose: a
        // suite that runs beside others may hold the pool for seconds.
        let started = ContinuousClock.now
        let outcome = await RootAppStep.run(
            shell: shell(""), release: release, state: rooted, installing: {},
            install: { await Self.waitThatACancelDoesNotEnd(seconds: 40) },
            limit: .milliseconds(200))
        #expect(outcome.isFailure)
        #expect(ContinuousClock.now - started < .seconds(20))
    }

    @Test func openingTheAppIsInsideTheBoundAndSaysNothingOfTheInstall() async {
        let slowOpen = RootScriptedShell { command in
            if command == MagiskApp.openCommand { await Self.waitThatACancelDoesNotEnd(seconds: 40) }
            return GuestShellOutput(text: "", exitCode: 0)
        }
        let started = ContinuousClock.now
        let outcome = await RootAppStep.run(
            shell: slowOpen, release: release, state: rooted, installing: {}, install: {}, limit: .seconds(2))
        guard case .installed = outcome else { Issue.record("an installed app was reported as \(outcome)"); return }
        #expect(ContinuousClock.now - started < .seconds(20), "the open waited past the bound")
    }

    @Test func aCancelledCallerIsReleasedBeforeTheWorkEnds() async {
        let answered = RootCounter()
        let slowAnswer = RootScriptedShell { _ in
            await Self.waitThatACancelDoesNotEnd(seconds: 40)
            await answered.bump()
            return GuestShellOutput(text: "", exitCode: 0)
        }
        let release = release
        let state = rooted
        let task = Task {
            await RootAppStep.run(shell: slowAnswer, release: release, state: state, installing: {}, install: {})
        }
        while slowAnswer.commands.isEmpty { try? await Task.sleep(for: .milliseconds(5)) }
        task.cancel()
        let outcome = await task.value
        let answeredBefore = await answered.value
        #expect(outcome.isFailure)
        #expect(answeredBefore == 0, "a cancelled step kept its caller until the guest answered")
    }

    @Test func aCancelledStepInstallsNothingWhenTheGuestAnswersLate() async {
        let gate = RootCounter()
        let answered = RootCounter()
        let slowAnswer = RootScriptedShell { _ in
            while await gate.value == 0 { try? await Task.sleep(for: .milliseconds(5)) }
            await answered.bump()
            return GuestShellOutput(text: "", exitCode: 0)
        }
        let installed = RootCounter()
        let release = release
        let state = rooted
        let task = Task {
            await RootAppStep.run(
                shell: slowAnswer, release: release, state: state, installing: {}, install: { await installed.bump() })
        }
        while slowAnswer.commands.isEmpty { try? await Task.sleep(for: .milliseconds(5)) }
        task.cancel()
        _ = await task.value
        await gate.bump()
        var waited = 0
        while await answered.value == 0, waited < 4_000 { try? await Task.sleep(for: .milliseconds(5)); waited += 5 }
        try? await Task.sleep(for: .milliseconds(200))
        let done = await installed.value
        #expect(done == 0)
    }

    @Test func theBoundIsFortyFiveSeconds() {
        #expect(RootAppStep.limit == .seconds(45))
    }

    @Test func aGuestThatCannotBeAskedGetsTheInstallAndItsFailureIsReported() async {
        struct Unreachable: Error {}
        let shell = RootScriptedShell { _ in throw Unreachable() }
        let outcome = await RootAppStep.run(
            shell: shell, release: release, state: rooted, installing: {}, install: { throw Unreachable() })
        #expect(outcome.isFailure)
    }

    @Test func theBootScreenHasAFifthStepOnlyForTheInstall() {
        #expect(BootSteps.count == 4 && BootSteps.countWithRootApp == 5)
        #expect(BootSteps.reportName(ofStep: BootSteps.rootAppStep, recovery: false) == "Magisk app")
        #expect(BootSteps.reportName(ofStep: 3, recovery: false) == "Startup")
    }
}
