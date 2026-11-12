// SPDX-License-Identifier: GPL-2.0-or-later

#if APP_REVIEW

import Foundation

// What the application keeps about asking for a rating, and what it makes of
// the system's reaction to a request. Pure logic: the clocks come in as
// parameters and the storage behind a protocol. Everything here stays on the
// device; nothing leaves it or enters a bug report.

/// What the system showed, as far as the application can tell. iOS gives no
/// result for a review request (`AppStore.requestReview(in:)` may or may not
/// show anything), so the application watches for a second after making one.
nonisolated enum ReviewPromptEvidence: Equatable, Sendable {
    /// The system presented something over the application.
    case seen
    /// Nothing happened that a prompt would explain.
    case notSeen
    /// The application presented or dismissed something itself in that second,
    /// which explains what happened as well as a prompt does: no verdict.
    case notJudged
}

/// What the system side of the watch saw, from public notifications only: the
/// scene leaving the foreground-active state, or a window resigning key.
nonisolated struct ReviewPromptObservation: Equatable, Sendable {
    var sceneLeftForeground = false
    var keyWindowResigned = false
}

nonisolated enum ReviewPromptJudgement {
    /// A prompt is taken to have shown when the scene or the key window gave
    /// way and the application itself presented nothing in the same second.
    /// The person leaving the application in that second looks the same and
    /// counts as seen: the attempt ends there, which is accepted.
    static func evidence(_ observation: ReviewPromptObservation, appPresented: Bool) -> ReviewPromptEvidence {
        if appPresented { return .notJudged }
        return observation.sceneLeftForeground || observation.keyWindowResigned ? .seen : .notSeen
    }
}

/// The attempts to ask and how they ended: kept on the device, in two places
/// (`ReviewAskPersistence`), and read back as the furthest of the two.
nonisolated struct ReviewAskHistory: Equatable, Sendable, Codable {
    static let maximumAttempts = 3
    /// The least time between two attempts.
    static let spacing: TimeInterval = 7 * 24 * 3600

    /// Requests made, including one whose outcome was not yet known.
    var attempts = 0
    var lastAttempt: Date?
    /// A prompt was seen, or the person opened Review on the App Store.
    var finished = false

    /// Nothing more is asked: the attempts are used up whatever was seen, or
    /// the question was answered.
    var done: Bool { finished || attempts >= Self.maximumAttempts }

    /// An attempt less than the spacing ago holds the next one back. A date in
    /// the future (the clock was set back) does not: the three attempts are
    /// the limit that matters.
    func isTooSoon(at now: Date) -> Bool {
        guard let last = lastAttempt else { return false }
        let elapsed = now.timeIntervalSince(last)
        return elapsed >= 0 && elapsed < Self.spacing
    }

    /// Recorded before the request goes, so that nothing the request or the
    /// application's end does can lose it.
    mutating func beginAttempt(at now: Date) {
        attempts += 1
        lastAttempt = now
    }

    /// The attempt's outcome. Not seen leaves the attempt counted; seen ends
    /// the asking; not judged gives the attempt back, keeping its date so
    /// that the retry waits the same spacing.
    mutating func conclude(_ evidence: ReviewPromptEvidence) {
        switch evidence {
        case .seen: finished = true
        case .notSeen: break
        case .notJudged: attempts = max(0, attempts - 1)
        }
    }

    /// The person went to write a review.
    mutating func finish() { finished = true }

    /// A value damaged in storage is held to what a history can be.
    func sanitized() -> ReviewAskHistory {
        var copy = self
        copy.attempts = min(max(attempts, 0), 99)
        if let last = lastAttempt, !last.timeIntervalSinceReferenceDate.isFinite { copy.lastAttempt = nil }
        return copy
    }

    /// The furthest of two readings of the same history: the attempts that
    /// were made, the latest of their dates and every way it ended.
    func merged(with other: ReviewAskHistory) -> ReviewAskHistory {
        ReviewAskHistory(
            attempts: max(attempts, other.attempts),
            lastAttempt: [lastAttempt, other.lastAttempt].compactMap { $0 }.max(),
            finished: finished || other.finished)
    }
}

/// What a launch of the application reads back about the counter and the
/// previous run.
nonisolated struct ReviewAskSnapshot: Equatable, Sendable {
    var activeSeconds: TimeInterval = 0
    /// The most recent run ended normally. A run that has begun has not (yet):
    /// an application that ends in the middle of one leaves it so.
    var lastRunEndedNormally = true
}

/// Where the counter and the history are kept.
nonisolated protocol ReviewAskPersistence {
    func loadSnapshot() -> ReviewAskSnapshot
    func save(_ snapshot: ReviewAskSnapshot)
    /// nil when the history cannot be read; the tracker then asks nothing.
    func loadHistory() -> ReviewAskHistory?
    func save(_ history: ReviewAskHistory)
}

/// The counter, the run's end and the attempts, with their writes: one at each
/// state change and one for each checkpoint while the counter runs, never more.
nonisolated struct ReviewAskTracker {
    private let persistence: any ReviewAskPersistence
    /// A monotonic clock, in seconds.
    private let uptime: () -> TimeInterval
    private(set) var counter: ReviewAskCounter
    private(set) var history: ReviewAskHistory
    private(set) var lastRunEndedNormally: Bool

    init(persistence: any ReviewAskPersistence, uptime: @escaping () -> TimeInterval) {
        self.persistence = persistence
        self.uptime = uptime
        let snapshot = persistence.loadSnapshot()
        counter = ReviewAskCounter(banked: snapshot.activeSeconds)
        lastRunEndedNormally = snapshot.lastRunEndedNormally
        // A history that cannot be read is not guessed at.
        history = persistence.loadHistory()?.sanitized() ?? ReviewAskHistory(finished: true)
    }

    var activeSeconds: TimeInterval { counter.total(at: uptime()) }

    private func saveSnapshot() {
        persistence.save(ReviewAskSnapshot(activeSeconds: activeSeconds, lastRunEndedNormally: lastRunEndedNormally))
    }

    private func saveHistory() { persistence.save(history) }

    /// A virtual machine is running in the foreground, or it is not.
    mutating func count(_ counting: Bool) {
        if counter.set(counting: counting, at: uptime()) { saveSnapshot() }
    }

    /// The periodic write while the counter runs.
    mutating func checkpoint() {
        guard counter.isCounting else { return }
        counter.bank(at: uptime())
        saveSnapshot()
    }

    /// A run begins: until it ends normally the most recent run has not.
    mutating func runBegan() { setRun(normal: false) }

    /// A run ended, or the previous process's record says how its run ended.
    mutating func runEnded(normally: Bool) { setRun(normal: normally) }

    private mutating func setRun(normal: Bool) {
        guard lastRunEndedNormally != normal else { return }
        lastRunEndedNormally = normal
        saveSnapshot()
    }

    func isTooSoon(at now: Date) -> Bool { history.isTooSoon(at: now) }

    mutating func beginAttempt(at now: Date) {
        history.beginAttempt(at: now)
        saveHistory()
    }

    mutating func conclude(_ evidence: ReviewPromptEvidence) {
        history.conclude(evidence)
        saveHistory()
    }

    /// Review on the App Store was opened from the menu.
    mutating func openedReview() {
        guard !history.finished else { return }
        history.finish()
        saveHistory()
    }

    #if !APP_STORE
    /// For a Debug build's own check: the counter at `seconds`, and the
    /// history cleared. An App Store product holds neither.
    mutating func debugSet(activeSeconds seconds: TimeInterval?, clearHistory: Bool) {
        if let seconds {
            counter = ReviewAskCounter(banked: seconds)
            saveSnapshot()
        }
        if clearHistory {
            history = ReviewAskHistory()
            saveHistory()
        }
    }
    #endif
}

#endif
