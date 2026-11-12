// SPDX-License-Identifier: GPL-2.0-or-later

#if APP_REVIEW

import StoreKit
import UIKit

/// What the rating prompt asks of the system and the device, in one place a
/// test replaces. The system's own prompt is the only thing it shows; it has no
/// screen, text or button of the application's.
struct ReviewAskServices {
    /// Where the counter and the attempts are kept.
    var persistence: any ReviewAskPersistence = ReviewAskStore()
    /// A monotonic clock, in seconds, for the counter. It is Swift's
    /// `ContinuousClock` and not `ProcessInfo.systemUptime`, a boot-time API that
    /// the privacy manifest would have to declare a reason for.
    var uptime: () -> TimeInterval = { ReviewAskServices.monotonicSeconds() }
    /// The calendar clock, for the spacing between attempts.
    var date: () -> Date = { Date() }
    /// Asks the system for its rating prompt over the foreground scene. The
    /// system decides whether anything shows, and says nothing either way.
    var request: @MainActor () -> Void = { ReviewAskServices.requestInForegroundScene() }
    /// Watches the second after a request for what the system did.
    var detector: any ReviewPromptDetecting = ReviewPromptWatcher()
    /// The scene is the foreground, active one.
    var sceneActive: @MainActor () -> Bool = { UIApplication.shared.applicationState == .active }
    /// Something is presented over Home by UIKit (a sheet, an alert, the
    /// share sheet), which the model does not know of.
    var presenting: @MainActor () -> Bool = { ReviewAskServices.uikitPresenting() }
    /// How long Home stays visible before a request, and how often the counter is written.
    var settle = ReviewAskRule.settle
    var checkpoint = ReviewAskRule.checkpoint
    /// Where the scene's edges are posted.
    var center = NotificationCenter.default

    private nonisolated static let origin = ContinuousClock.now

    /// Seconds since the clock was first read: only differences mean anything.
    nonisolated static func monotonicSeconds() -> TimeInterval {
        let elapsed = ContinuousClock.now - origin
        return TimeInterval(elapsed.components.seconds) + TimeInterval(elapsed.components.attoseconds) / 1e18
    }

    @MainActor
    static func requestInForegroundScene() {
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        guard let scene = scenes.first(where: { $0.activationState == .foregroundActive }) else { return }
        AppStore.requestReview(in: scene)
    }

    @MainActor
    static func uikitPresenting() -> Bool {
        UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
            .flatMap(\.windows)
            .contains { $0.rootViewController?.presentedViewController != nil }
    }
}

/// Watches the second after a request for what the system did, and says it.
/// The coordinator makes the request through it, so that the watch is in place
/// before the request goes.
@MainActor
protocol ReviewPromptDetecting {
    func observe(afterRequesting request: @MainActor () -> Void) async -> ReviewPromptObservation
}

/// The live detector. It hears only public notifications: the scene leaving the
/// foreground-active state and a window resigning key. It looks for no window
/// by its class and reads nothing private.
@MainActor
final class ReviewPromptWatcher: ReviewPromptDetecting {
    private final class Heard {
        var observation = ReviewPromptObservation()
    }

    private let center: NotificationCenter
    private let window: Duration

    init(center: NotificationCenter = .default, window: Duration = .seconds(1)) {
        self.center = center
        self.window = window
    }

    func observe(afterRequesting request: @MainActor () -> Void) async -> ReviewPromptObservation {
        let heard = Heard()
        let tokens = [
            center.addObserver(forName: UIScene.willDeactivateNotification, object: nil, queue: .main) { _ in
                MainActor.assumeIsolated { heard.observation.sceneLeftForeground = true }
            },
            center.addObserver(forName: UIWindow.didResignKeyNotification, object: nil, queue: .main) { _ in
                MainActor.assumeIsolated { heard.observation.keyWindowResigned = true }
            },
        ]
        defer { tokens.forEach { center.removeObserver($0) } }
        request()
        try? await Task.sleep(for: window)
        return heard.observation
    }
}

#endif
