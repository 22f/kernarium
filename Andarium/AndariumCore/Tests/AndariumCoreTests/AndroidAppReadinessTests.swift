// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// A guest shell that answers each command from a table.
private final class AnswerShell: GuestShell, @unchecked Sendable {
    struct Unreachable: Error {}
    private let lock = NSLock()
    private var answers: [String: GuestShellOutput]
    private(set) var asked: [String] = []
    var reachable = true

    init(boot: String = "1", home: String = "priority=0 preferredOrder=0\ncom.android.launcher3/.Launcher\n/x", setup: String? = "1") {
        answers = [
            AndroidAppReadiness.bootCompletedCommand: GuestShellOutput(text: boot + "\n", exitCode: 0),
            AndroidAppReadiness.homeCommand: GuestShellOutput(text: home, exitCode: 0),
        ]
        if let setup { answers[AndroidAppReadiness.setupCommand] = GuestShellOutput(text: setup + "\n", exitCode: 0) }
    }

    func set(_ command: String, _ output: GuestShellOutput?) { lock.withLock { answers[command] = output } }

    func run(_ command: String, limit: Duration) async throws -> GuestShellOutput {
        try lock.withLock {
            asked.append(command)
            guard reachable else { throw Unreachable() }
            return answers[command] ?? GuestShellOutput(text: "", exitCode: 127)
        }
    }
}

struct AndroidAppReadinessTests {
    @Test func androidIsReadyWhenEverythingHolds() async {
        let shell = AnswerShell()
        let state = await AndroidAppReadiness.check(shell: shell, storageReady: { true })
        #expect(state == .ready)
    }

    @Test func aSettingThatIsNotSetDoesNotHoldTheRequest() async {
        let shell = AnswerShell(setup: nil)
        shell.set(AndroidAppReadiness.setupCommand, GuestShellOutput(text: "null\n", exitCode: 0))
        #expect(await AndroidAppReadiness.check(shell: shell, storageReady: { true }) == .ready)
        shell.set(AndroidAppReadiness.setupCommand, GuestShellOutput(text: "", exitCode: 255))
        #expect(await AndroidAppReadiness.check(shell: shell, storageReady: { true }) == .ready)
    }

    @Test func anUnfinishedSetupIsSaid() async {
        let shell = AnswerShell(setup: "0")
        #expect(await AndroidAppReadiness.check(shell: shell, storageReady: { true }) == .setupUnfinished)
    }

    @Test func eachMissingPieceIsNotYet() async {
        #expect(await AndroidAppReadiness.check(shell: AnswerShell(boot: "0"), storageReady: { true }) == .notYet)
        #expect(await AndroidAppReadiness.check(shell: AnswerShell(boot: ""), storageReady: { true }) == .notYet)
        #expect(await AndroidAppReadiness.check(shell: AnswerShell(home: "No activity found"), storageReady: { true }) == .notYet)
        #expect(await AndroidAppReadiness.check(shell: AnswerShell(), storageReady: { false }) == .notYet)
        #expect(await AndroidAppReadiness.check(shell: AnswerShell(), storageReady: { nil }) == .notYet)
        let down = AnswerShell()
        down.reachable = false
        #expect(await AndroidAppReadiness.check(shell: down, storageReady: { true }) == .notYet)
    }

    @Test func theSetupIsOnlyAskedOnceTheRestHolds() async {
        let shell = AnswerShell(boot: "0", setup: "0")
        _ = await AndroidAppReadiness.check(shell: shell, storageReady: { true })
        #expect(shell.asked == [AndroidAppReadiness.bootCompletedCommand])
    }
}

/// A scripted sequence of looks at Android, and a clock that only the waits
/// between them advance.
private final class Script: @unchecked Sendable {
    private let lock = NSLock()
    private var states: [AndroidAppReadiness.State]
    private var clock = Duration.zero
    private var sleeps: [Duration] = []

    init(_ states: [AndroidAppReadiness.State]) { self.states = states }

    var slept: [Duration] { lock.withLock { sleeps } }
    func next() -> AndroidAppReadiness.State { lock.withLock { states.isEmpty ? .notYet : states.removeFirst() } }
    func now() -> Duration { lock.withLock { clock } }
    func sleep(_ interval: Duration) { lock.withLock { clock += interval; sleeps.append(interval) } }
}

struct AndroidAppReadinessWaitTests {
    private func run(_ states: [AndroidAppReadiness.State], limit: Duration = .seconds(30)) async -> (AndroidAppReadiness.Outcome, [Duration]) {
        let script = Script(states)
        let outcome = await AndroidAppReadiness.wait(
            check: { script.next() }, now: { script.now() }, sleep: { script.sleep($0) }, limit: limit)
        return (outcome, script.slept)
    }

    @Test func readyEndsTheWaitAfterTheSettle() async {
        let (outcome, slept) = await run([.notYet, .notYet, .ready])
        #expect(outcome == .ready)
        #expect(slept == [AndroidAppReadiness.pollInterval, AndroidAppReadiness.pollInterval, AndroidAppReadiness.settle])
    }

    @Test func anUnfinishedSetupEndsTheWaitAtOnce() async {
        let (outcome, slept) = await run([.notYet, .setupUnfinished])
        #expect(outcome == .setupUnfinished)
        #expect(slept == [AndroidAppReadiness.pollInterval])
    }

    @Test func theWaitEndsWhenTheGuestsRunningTimeIsUp() async {
        let (outcome, slept) = await run([], limit: .seconds(2))
        #expect(outcome == .timedOut)
        // Five looks, 400 ms apart, bring the clock to the limit.
        #expect(slept.count == 5)
        let (late, _) = await run([.notYet, .notYet, .notYet, .notYet, .ready], limit: .seconds(30))
        #expect(late == .ready)
    }

    @Test func aCancelledWaitIsTimedOut() async {
        let task = Task {
            await AndroidAppReadiness.wait(
                check: { .notYet }, now: { .zero }, sleep: { _ in try? await Task.sleep(for: .milliseconds(5)) },
                limit: .seconds(10_000))
        }
        task.cancel()
        #expect(await task.value == .timedOut)
    }
}

struct AndroidAppSearchPreferenceTests {
    private func defaults() -> UserDefaults {
        let name = "andarium-search-pref-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defaults.removePersistentDomain(forName: name)
        return defaults
    }

    @Test func searchAndSiriAreOnUntilTurnedOff() {
        let defaults = defaults()
        let preference = AndroidAppSearchPreference(defaults: defaults)
        #expect(preference.isOn)
        preference.keep(false)
        #expect(!AndroidAppSearchPreference(defaults: defaults).isOn)
        preference.keep(true)
        #expect(AndroidAppSearchPreference(defaults: defaults).isOn)
    }
}
