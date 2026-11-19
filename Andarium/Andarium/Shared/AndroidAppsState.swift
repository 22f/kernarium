// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AppIntents
import CoreSpotlight
import Foundation
import Observation
import UIKit

/// Where Android's apps come from and how one is opened, for Add to Home
/// Screen: the runtime asks the tool in Android's image (`AndroidAppsCommands`)
/// and waits for Android to be ready for an app, and the Simulator preview
/// plays a list. Everything the screens know of the tool is `AndariumCore`'s
/// `AndroidApp`, its icon and the answers of `AndroidAppsTool`.
@MainActor
protocol AndroidAppsSource: AnyObject {
    /// The tool for this run; nil where the run cannot ask it (no adb, Recovery).
    var androidAppsTool: (any AndroidAppsTool)? { get }
    /// Waits until Android is ready for an app to be opened: its boot complete,
    /// its storage up, its home screen resumed. The wait ends early when
    /// Android's own first-run setup is on the screen, and by itself after
    /// `AndroidAppReadiness.limit` of the guest's running time.
    func awaitAndroidReady() async -> AndroidAppReadiness.Outcome
}

/// The tool, with the pictures iOS would draw badly made safe: an icon with
/// transparent pixels is drawn on white before it is kept, because iOS paints a
/// transparent Home Screen icon black. The listing and the launching are the
/// tool's.
nonisolated struct FlatteningAppsTool: AndroidAppsTool {
    let base: any AndroidAppsTool

    func digest(limit: Duration) async -> AndroidAppDigestAnswer { await base.digest(limit: limit) }
    func list(limit: Duration) async -> AndroidAppListing { await base.list(limit: limit) }
    func open(_ component: AndroidComponent, limit: Duration) async -> AndroidAppOpenAnswer {
        await base.open(component, limit: limit)
    }

    func icons(of apps: [AndroidApp], limit: Duration) async -> [String: AndroidAppIconAnswer] {
        var answers = await base.icons(of: apps, limit: limit)
        for (id, answer) in answers {
            guard case let .png(data, hasAlpha) = answer, hasAlpha else { continue }
            answers[id] = Self.flattened(data).map { .png($0, hasAlpha: false) } ?? .error
        }
        return answers
    }

    /// The picture on an opaque white square; nil where it cannot be read.
    static func flattened(_ data: Data) -> Data? {
        guard let image = UIImage(data: data), image.size.width > 0, image.size.height > 0 else { return nil }
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        format.opaque = true
        let pixels = CGSize(width: image.size.width * image.scale, height: image.size.height * image.scale)
        let renderer = UIGraphicsImageRenderer(size: pixels, format: format)
        return renderer.image { context in
            UIColor.white.setFill()
            context.fill(CGRect(origin: .zero, size: pixels))
            image.draw(in: CGRect(origin: .zero, size: pixels))
        }.pngData()
    }
}

/// What the session knows of Android's apps: the list the cache holds, a
/// refresh under way, the request to open an app and the banner of how the last
/// one ended. The list is the cache's, so the chooser and the App Intent have
/// it while Android is off; the rest lasts for one run.
@Observable
final class AndroidAppsState {
    /// The apps of the last listing that was kept, in the order the person reads them.
    private(set) var apps: [AndroidApp] = []
    /// When the cached list was made (Android's listing), for the page's note.
    private(set) var listedAt: Date?
    /// A refresh is under way.
    private(set) var refreshing = false
    /// Icons the refresh is still to fetch.
    private(set) var iconsLeft = 0
    /// A listing answered this run (an empty one included).
    private(set) var listed = false
    /// The image has no listing tool, or speaks a version of it this Andarium
    /// does not know: nothing is offered.
    private(set) var unavailable = false
    /// The last refresh did not get an answer.
    private(set) var failed = false
    /// Changes whenever an icon arrives, so the screens read the pictures again.
    private(set) var iconRevision = 0
    /// The request to open an app and the banner of how the last one ended.
    var open = AppOpenState()
    /// Whether Android's apps are offered to Search and Siri (the Spotlight index).
    var searchOn = true
    /// Where the switch is kept.
    @ObservationIgnored var preference = AndroidAppSearchPreference()
    /// The cache: the catalog, the icons, and the record kept per icon.
    @ObservationIgnored var store: AndroidAppStore
    /// The refresh under way, which a second asker joins.
    @ObservationIgnored var inFlight: Task<AndroidAppRefresh.Result, Never>?
    /// The run's first refresh, asked again a few times if it got no answer.
    @ObservationIgnored var first: Task<Void, Never>?
    /// Android being made ready for the request, then the app being opened.
    @ObservationIgnored var opening: Task<Void, Never>?
    /// The wait for the next request or banner to run out.
    @ObservationIgnored var expiry: Task<Void, Never>?
    /// The preview keeps a waiting request and a banner as they are, for a
    /// screen that must stay still.
    @ObservationIgnored var holdsForever = false
    /// The first refresh waits this long after Android's storage is up, so it
    /// does not compete with the end of the boot.
    @ObservationIgnored var startDelay = Duration.seconds(5)
    /// The first refresh's attempts wait twice as long as the one before.
    @ObservationIgnored var retryInterval = Duration.seconds(5)
    /// How many times the run's first refresh is asked.
    static let startAttempts = 5

    init(store: AndroidAppStore = AndroidAppsHub.shared.store) {
        self.store = store
        searchOn = preference.isOn
        adoptCache()
    }

    /// The cached list, as the store has it now.
    func adoptCache() {
        let catalog = store.catalog()
        apps = catalog?.apps ?? []
        listedAt = catalog?.listedAt
    }

    var installed: [AndroidApp] { apps.filter { !$0.builtIn } }
    var builtIn: [AndroidApp] { apps.filter(\.builtIn) }

    func begin() {
        refreshing = true
        failed = false
    }

    /// A catalog is in the store (the listing, then each batch of icons).
    func progressed(_ catalog: AndroidAppCatalog, iconsLeft left: Int) {
        if apps != catalog.apps { apps = catalog.apps }
        listedAt = catalog.listedAt
        iconsLeft = left
        iconRevision &+= 1
    }

    func finished(_ result: AndroidAppRefresh.Result) {
        refreshing = false
        iconsLeft = 0
        switch result {
        case let .refreshed(catalog):
            progressed(catalog, iconsLeft: 0)
            listed = true
            unavailable = false
            failed = false
        case .unavailable:
            unavailable = true
            listed = true
        case .failed:
            failed = true
        }
    }

    /// Dropped for a run that ended meanwhile.
    func abandonedRefresh() {
        refreshing = false
        iconsLeft = 0
    }

    /// The run is over: nothing is under way, and what this run listed is not
    /// known of the next. The cached list stays.
    func runEnded() {
        inFlight?.cancel()
        inFlight = nil
        first?.cancel()
        first = nil
        opening?.cancel()
        opening = nil
        open.runEnded()
        open.dismissBanner()
        refreshing = false
        iconsLeft = 0
        listed = false
        failed = false
        unavailable = false
        expiry?.cancel()
        expiry = nil
    }

    /// Android was erased: the cache is gone, the record of each icon is kept.
    func erased() {
        runEnded()
        store.erase()
        adoptCache()
        iconRevision &+= 1
    }

    /// The disk may not be what was listed (a restored snapshot or update):
    /// the next refresh lists again.
    func invalidated() {
        store.invalidate()
        listed = false
    }

    /// Sleeps until the earliest time a waiting request or a banner runs out,
    /// then lets `expire` drop what has, and looks again.
    func scheduleExpiry(_ changed: @escaping @MainActor () -> Void) {
        expiry?.cancel()
        expiry = nil
        guard let deadline = nextDeadline() else { return }
        expiry = Task { [weak self] in
            let wait = max(deadline.timeIntervalSinceNow, 0) + 0.05
            try? await Task.sleep(for: .seconds(wait))
            guard let self, !Task.isCancelled else { return }
            if open.expire(now: Date()) { changed() }
            scheduleExpiry(changed)
        }
    }

    private func nextDeadline() -> Date? {
        guard !holdsForever else { return nil }
        var deadlines: [Date] = []
        if let pending = open.pending, case let .waiting(_, since) = pending.stage {
            deadlines.append(since.addingTimeInterval(AppOpenState.holdLimit))
        }
        if let banner = open.banner {
            let limit = banner.failure.canRetry ? AppOpenState.retryBannerLimit : AppOpenState.bannerLimit
            deadlines.append(banner.shownAt.addingTimeInterval(limit))
        }
        return deadlines.min()
    }
}

/// How the app list reaches Spotlight (Search and Siri) and the App Shortcuts,
/// so a test can watch it without the system's index.
@MainActor
protocol AndroidAppsSearchIndexing: AnyObject {
    /// Offers these apps to Search and Siri, in place of what was offered.
    func offer(_ entities: [AndroidAppEntity]) async
    /// Takes every app back out of Search and Siri.
    func withdraw() async
    /// Tells the system that the apps the shortcut tiles are made of changed.
    func shortcutsChanged()
}

/// The system's index.
@MainActor
final class SpotlightAppsIndex: AndroidAppsSearchIndexing {
    func offer(_ entities: [AndroidAppEntity]) async {
        let index = CSSearchableIndex.default()
        try? await index.deleteAppEntities(ofType: AndroidAppEntity.self)
        guard !entities.isEmpty else { return }
        try? await index.indexAppEntities(entities)
    }

    func withdraw() async {
        try? await CSSearchableIndex.default().deleteAppEntities(ofType: AndroidAppEntity.self)
    }

    func shortcutsChanged() {
        AndroidAppShortcuts.updateAppShortcutParameters()
    }
}

/// What the App Intent, its entity and its queries share with the application:
/// the cache (which answers while Android is off, and in a process the system
/// started for a query, before any screen or model exists), the model a
/// request goes to, and the index.
nonisolated final class AndroidAppsHub: @unchecked Sendable {
    static let shared = AndroidAppsHub()

    private let lock = NSLock()
    private var kept: AndroidAppStore?

    /// The cache, in the application's support folder unless a launch put it elsewhere.
    var store: AndroidAppStore {
        lock.withLock {
            if let kept { return kept }
            let store = AndroidAppStore(directory: Self.defaultDirectory())
            kept = store
            return store
        }
    }

    /// Puts the cache somewhere else (an isolated test guest, a hosted test,
    /// the preview). Done before the first use; a model made afterwards adopts it.
    func configure(directory: URL) {
        lock.withLock { kept = AndroidAppStore(directory: directory) }
    }

    static func defaultDirectory() -> URL {
        let support = (try? FileManager.default.url(
            for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true
        )) ?? FileManager.default.temporaryDirectory
        return support.appendingPathComponent("AndroidApps", isDirectory: true)
    }

    private weak var attachedModel: AppModel?
    private var indexKept: (any AndroidAppsSearchIndexing)?

    /// The model a request is handed to. The process's own, unless a test
    /// attached another.
    @MainActor var model: AppModel {
        if let attached = lock.withLock({ attachedModel }) { return attached }
        return AndariumProcess.shared.model
    }

    @MainActor func attach(_ model: AppModel?) {
        lock.withLock { attachedModel = model }
    }

    /// Where the apps are offered to the system; a test or the preview puts another.
    @MainActor var searchIndex: any AndroidAppsSearchIndexing {
        get {
            lock.withLock {
                if let indexKept { return indexKept }
                let index = SpotlightAppsIndex()
                indexKept = index
                return index
            }
        }
        set { lock.withLock { indexKept = newValue } }
    }

    /// The entities of the cache, as the query answers.
    func entities() -> [AndroidAppEntity] {
        (store.catalog()?.apps ?? []).map(AndroidAppEntity.init)
    }
}
