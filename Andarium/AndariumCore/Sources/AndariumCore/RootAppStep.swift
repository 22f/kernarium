// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The boot screen's Magisk app step: after Android has booted with root, the
/// application asks the guest whether the official Magisk app is there and puts
/// it in when it is not (`RootInstallRule`). The check and the install are
/// bounded together by `limit`; past it, or on a failure, the boot goes on
/// with root on and no app, and Home says so.
public enum RootAppStep {
    /// The hard bound on the check and the install together.
    public static let limit = Duration.seconds(45)
    /// The bound on each of the short commands (the check, opening the app).
    static let commandLimit = Duration.seconds(10)

    public enum Outcome: Equatable, Sendable {
        /// Nothing was installed.
        case notNeeded
        /// The app was put in; the state records it.
        case installed(RootState)
        /// The check or the install failed, or the bound passed; the state
        /// carries the install that was tried and why, when one was.
        case failed(RootState)

        public var isFailure: Bool {
            if case .failed = self { return true }
            return false
        }
    }

    /// Runs the step. `installing` is told once, when an install is about to
    /// begin and not before, so that the screen adds its step only for a boot
    /// that installs. `install` puts the carried app in the guest.
    public static func run(
        shell: any GuestShell, release: RootRelease, state: RootState,
        installing: @escaping @Sendable () async -> Void,
        install: @escaping @Sendable () async throws -> Void,
        limit: Duration = RootAppStep.limit
    ) async -> Outcome {
        let attempt = Attempt(state)
        let outcome = await bounded(limit) { () -> Outcome in
            let presence = await MagiskApp.presence(shell: shell, limit: commandLimit)
            if Task.isCancelled { return .failed(state) }
            let action = RootInstallRule.decide(
                presence: presence, carriedVersionCode: release.versionCode, state: state)
            guard action == .install else { return .notNeeded }
            let reason = RootInstallRule.reason(presence: presence, carriedVersionCode: release.versionCode)
            var tried = state
            tried.appInstall = .init(reason: reason, installed: false)
            attempt.set(tried)
            await installing()
            do {
                try await install()
            } catch {
                return .failed(tried)
            }
            var done = RootInstallRule.recorded(state, versionCode: release.versionCode)
            done.appInstall = .init(reason: reason, installed: true)
            return .installed(done)
        }
        guard let outcome else { return .failed(attempt.state) }
        if case .installed = outcome, RootInstallRule.opensAppAfterInstall {
            await MagiskApp.open(shell: shell, limit: commandLimit)
        }
        return outcome
    }

    /// The state with the install the step has begun, for a bound that passes
    /// before the work says.
    private final class Attempt: @unchecked Sendable {
        private let lock = NSLock()
        private var value: RootState

        init(_ state: RootState) { value = state }

        var state: RootState { lock.withLock { value } }
        func set(_ state: RootState) { lock.withLock { value = state } }
    }

    /// `work`'s value, or nil when `limit` passes first. The caller is released
    /// at the bound even if the work ignores its cancellation.
    static func bounded<Value: Sendable>(
        _ limit: Duration, _ work: @escaping @Sendable () async -> Value
    ) async -> Value? {
        let gate = Gate<Value>()
        return await withCheckedContinuation { continuation in
            gate.install(continuation)
            let task = Task { gate.finish(await work()) }
            Task {
                try? await Task.sleep(for: limit)
                task.cancel()
                gate.finish(nil)
            }
        }
    }

    /// Resumes its continuation once, with whichever finishes first.
    private final class Gate<Value: Sendable>: @unchecked Sendable {
        private let lock = NSLock()
        private var continuation: CheckedContinuation<Value?, Never>?
        private var finished = false

        func install(_ continuation: CheckedContinuation<Value?, Never>) {
            lock.lock()
            self.continuation = continuation
            lock.unlock()
        }

        func finish(_ value: Value?) {
            lock.lock()
            guard !finished else { lock.unlock(); return }
            finished = true
            let continuation = continuation
            lock.unlock()
            continuation?.resume(returning: value)
        }
    }
}
