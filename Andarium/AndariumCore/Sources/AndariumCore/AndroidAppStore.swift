// SPDX-License-Identifier: GPL-2.0-or-later

import CryptoKit
import Foundation

/// What Android last said of its launchable apps, kept on the device so that
/// the Home Screen's icons, Shortcuts, Search and Siri can name them while
/// Android is not running.
public struct AndroidAppCatalog: Equatable, Sendable, Codable {
    public static let currentSchema = 1

    public var schema = AndroidAppCatalog.currentSchema
    /// When Android last answered the listing that this one is.
    public var listedAt: Date
    public var digest: String?
    /// By name, as the person reads them.
    public var apps: [AndroidApp]

    public init(listedAt: Date, digest: String?, apps: [AndroidApp]) {
        self.listedAt = listedAt
        self.digest = digest
        self.apps = apps
    }

    public func app(withID id: String) -> AndroidApp? { apps.first { $0.id == id } }
}

/// What the store keeps of one app after an erase: enough to say, for an icon
/// that outlived its Android, which app it was.
public struct AndroidAppRecord: Equatable, Sendable, Codable, Identifiable {
    public var id: String
    public var label: String
    public var builtIn: Bool
    public var seen: Date

    public init(id: String, label: String, builtIn: Bool, seen: Date) {
        self.id = id
        self.label = label
        self.builtIn = builtIn
        self.seen = seen
    }

    public var component: AndroidComponent? { AndroidComponent(identifier: id) }
}

/// The launchable apps on disk, in one folder of the application:
///
/// - `Catalog/catalog.json` and `Catalog/Icons/` hold the last listing and the
///   icons as PNG files; they belong to the Android that was listed and go
///   when it is erased (`erase`).
/// - `records.json` holds a small record per app ever listed (its identifier,
///   its name and when it was seen), which an erase keeps, so that a Home
///   Screen icon whose Android is gone can still say which app it is, and
///   works again once the app is back in a new Android.
///
/// Everything is excluded from backup. A store is safe to read from any
/// thread: the intents' queries read it while the application is not
/// running, and the model writes it.
public final class AndroidAppStore: @unchecked Sendable {
    public let directory: URL
    private let lock = NSLock()
    private var loaded = false
    private var catalogCache: AndroidAppCatalog?
    private var recordCache: [String: AndroidAppRecord] = [:]

    private let fileManager = FileManager.default

    public init(directory: URL) {
        self.directory = directory
    }

    private var catalogDirectory: URL { directory.appendingPathComponent("Catalog", isDirectory: true) }
    private var catalogFile: URL { catalogDirectory.appendingPathComponent("catalog.json") }
    private var iconDirectory: URL { catalogDirectory.appendingPathComponent("Icons", isDirectory: true) }
    private var recordsFile: URL { directory.appendingPathComponent("records.json") }

    // MARK: Reading

    /// The last listing; nil before Android has been listed and after an erase.
    public func catalog() -> AndroidAppCatalog? {
        lock.withLock {
            loadIfNeeded()
            return catalogCache
        }
    }

    /// The icon kept for the app as it is now listed (its tag included), as PNG data.
    public func icon(of app: AndroidApp) -> Data? {
        lock.withLock { iconData(for: app) }
    }

    /// The icon kept for the app whatever its tag, for the entity of an app
    /// that is listed.
    public func icon(forID id: String) -> Data? {
        lock.withLock {
            loadIfNeeded()
            return catalogCache?.app(withID: id).flatMap { iconData(for: $0) }
        }
    }

    public func record(forID id: String) -> AndroidAppRecord? {
        lock.withLock {
            loadIfNeeded()
            return recordCache[id]
        }
    }

    public func records() -> [AndroidAppRecord] {
        lock.withLock {
            loadIfNeeded()
            return recordCache.values.sorted { $0.id < $1.id }
        }
    }

    /// The apps whose icon the store does not have as listed. An app that
    /// declares no icon (`markNoIcon`) is not missing one.
    public func appsMissingIcons(in list: AndroidAppList) -> [AndroidApp] {
        lock.withLock { list.apps.filter { iconData(for: $0) == nil && !declaresNoIcon($0) } }
    }

    /// Whether Android said the app declares no icon, as listed.
    public func declaresNoIcon(of app: AndroidApp) -> Bool {
        lock.withLock { declaresNoIcon(app) }
    }

    // MARK: Writing

    /// Keeps a listing. Icons of apps no longer listed, or listed with a new
    /// tag, are removed; the records learn every listed app's name. A listing
    /// that is the same as the stored one (digest and apps) changes nothing
    /// but the time it was taken.
    public func apply(_ list: AndroidAppList, at date: Date) throws {
        try lock.withLock {
            loadIfNeeded()
            try prepareDirectories()
            let apps = Array(list.apps.prefix(AndroidAppLimits.mostApps))
            let catalog = AndroidAppCatalog(listedAt: date, digest: list.digest, apps: apps)
            try write(catalog, to: catalogFile)
            catalogCache = catalog
            let keep = Set(apps.flatMap { [iconFileName(for: $0), noIconFileName(for: $0)] })
            if let files = try? fileManager.contentsOfDirectory(atPath: iconDirectory.path) {
                for file in files where !keep.contains(file) {
                    try? fileManager.removeItem(at: iconDirectory.appendingPathComponent(file))
                }
            }
            for app in apps {
                recordCache[app.id] = AndroidAppRecord(id: app.id, label: app.label, builtIn: app.builtIn, seen: date)
            }
            trimRecords()
            try writeRecords()
        }
    }

    /// Keeps one app's icon (PNG data) as listed. An empty or enormous
    /// picture is not kept.
    public func setIcon(_ data: Data, for app: AndroidApp) {
        lock.withLock {
            guard !data.isEmpty, data.count <= AndroidAppLimits.largestIcon else { return }
            guard (try? prepareDirectories()) != nil else { return }
            try? data.write(to: iconDirectory.appendingPathComponent(iconFileName(for: app)), options: .atomic)
        }
    }

    /// Keeps that Android said the app declares no icon, so it is not asked for again.
    public func markNoIcon(for app: AndroidApp) {
        lock.withLock {
            guard (try? prepareDirectories()) != nil else { return }
            try? Data().write(to: iconDirectory.appendingPathComponent(noIconFileName(for: app)), options: .atomic)
        }
    }

    /// The listing is the one the store has: only the time it was taken changes.
    public func touch(at date: Date) {
        lock.withLock {
            loadIfNeeded()
            guard var catalog = catalogCache else { return }
            catalog.listedAt = date
            guard (try? write(catalog, to: catalogFile)) != nil else { return }
            catalogCache = catalog
        }
    }

    /// Android was erased or reset: its apps are none. The records stay.
    public func erase() {
        lock.withLock {
            loadIfNeeded()
            try? fileManager.removeItem(at: catalogDirectory)
            catalogCache = nil
        }
    }

    /// Forgets everything, the records included (for a test, and for the person
    /// who wants the leftovers gone).
    public func eraseRecords() {
        lock.withLock {
            loadIfNeeded()
            try? fileManager.removeItem(at: catalogDirectory)
            try? fileManager.removeItem(at: recordsFile)
            catalogCache = nil
            recordCache = [:]
        }
    }

    // MARK: Files

    private func loadIfNeeded() {
        guard !loaded else { return }
        loaded = true
        if let data = try? Data(contentsOf: catalogFile),
           let decoded = try? JSONDecoder.store.decode(AndroidAppCatalog.self, from: data),
           decoded.schema == AndroidAppCatalog.currentSchema {
            catalogCache = decoded
        }
        if let data = try? Data(contentsOf: recordsFile),
           let decoded = try? JSONDecoder.store.decode([AndroidAppRecord].self, from: data) {
            recordCache = Dictionary(decoded.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        }
    }

    private func prepareDirectories() throws {
        try fileManager.createDirectory(at: iconDirectory, withIntermediateDirectories: true)
        var url = directory
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try? url.setResourceValues(values)
    }

    private func write<Value: Encodable>(_ value: Value, to url: URL) throws {
        try JSONEncoder.store.encode(value).write(to: url, options: .atomic)
    }

    private func writeRecords() throws {
        try write(recordCache.values.sorted { $0.id < $1.id }, to: recordsFile)
    }

    /// The oldest records go first once there are more than the bound.
    private func trimRecords() {
        guard recordCache.count > AndroidAppLimits.mostRecords else { return }
        let oldest = recordCache.values.sorted { ($0.seen, $0.id) < ($1.seen, $1.id) }
            .prefix(recordCache.count - AndroidAppLimits.mostRecords)
        for record in oldest { recordCache[record.id] = nil }
    }

    private func iconData(for app: AndroidApp) -> Data? {
        try? Data(contentsOf: iconDirectory.appendingPathComponent(iconFileName(for: app)))
    }

    private func declaresNoIcon(_ app: AndroidApp) -> Bool {
        fileManager.fileExists(atPath: iconDirectory.appendingPathComponent(noIconFileName(for: app)).path)
    }

    func noIconFileName(for app: AndroidApp) -> String {
        String(iconFileName(for: app).dropLast(".png".count)) + ".none"
    }

    /// A file name that no app's own text can shape: the hash of the identifier and the tag.
    func iconFileName(for app: AndroidApp) -> String {
        let digest = SHA256.hash(data: Data("\(app.id)\n\(app.iconTag)".utf8))
        return digest.map { String(format: "%02x", $0) }.joined() + ".png"
    }
}

extension JSONEncoder {
    fileprivate static var store: JSONEncoder {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .secondsSince1970
        encoder.outputFormatting = [.sortedKeys]
        return encoder
    }
}

extension JSONDecoder {
    fileprivate static var store: JSONDecoder {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .secondsSince1970
        return decoder
    }
}

/// Brings the store up to what Android lists now, for the model and its tests.
public enum AndroidAppRefresh {
    /// How a refresh ended.
    public enum Result: Equatable, Sendable {
        /// The store has Android's list (changed or not).
        case refreshed(AndroidAppCatalog)
        case unavailable
        case failed
    }

    /// Asks the tool whether the apps changed (its digest against the stored
    /// one) and lists them only if they did; the listing is stored first, so
    /// every name is known at once, and the icons that are missing follow, a
    /// few to an ask, each stored as it arrives. `progress` is told after the
    /// listing and after each batch of icons, with how many are still to come.
    /// An unchanged digest with every icon in hand asks for nothing more.
    public static func run(
        tool: any AndroidAppsTool, store: AndroidAppStore, now: Date = Date(),
        progress: @Sendable (AndroidAppCatalog, _ iconsLeft: Int) -> Void = { _, _ in }
    ) async -> Result {
        let remote = await tool.digest(limit: AndroidAppLimits.digestLimit)
        switch remote {
        case .unavailable: return .unavailable
        case .failed: return .failed
        case .digest: break
        }
        if case let .digest(digest) = remote, let kept = store.catalog(), kept.digest == digest {
            // Nothing that is cached is out of date; an icon that never arrived is still owed.
            store.touch(at: now)
            guard let catalog = store.catalog() else { return .failed }
            await fetchIcons(tool: tool, store: store, catalog: catalog, progress: progress)
            return .refreshed(catalog)
        }
        switch await tool.list(limit: AndroidAppLimits.listLimit) {
        case .unavailable:
            return .unavailable
        case .failed:
            return .failed
        case let .apps(list):
            let sorted = AndroidAppList(apps: list.apps.sorted(by: AndroidAppText.inOrder), digest: list.digest)
            do { try store.apply(sorted, at: now) } catch { return .failed }
            guard let catalog = store.catalog() else { return .failed }
            await fetchIcons(tool: tool, store: store, catalog: catalog, progress: progress)
            return .refreshed(catalog)
        }
    }

    private static func fetchIcons(
        tool: any AndroidAppsTool, store: AndroidAppStore, catalog: AndroidAppCatalog,
        progress: @Sendable (AndroidAppCatalog, Int) -> Void
    ) async {
        let list = AndroidAppList(apps: catalog.apps, digest: catalog.digest)
        var missing = store.appsMissingIcons(in: list)
        progress(catalog, missing.count)
        // An app the tool did not answer for, or answered `deferred` or `error`
        // for, is asked again at the next refresh, not in this one.
        while !missing.isEmpty {
            if Task.isCancelled { break }
            let batch = Array(missing.prefix(AndroidAppLimits.iconsPerAsk))
            missing.removeFirst(batch.count)
            let answers = await tool.icons(of: batch, limit: AndroidAppLimits.iconLimit)
            for app in batch {
                switch answers[app.id] {
                case let .png(data, _)?: store.setIcon(data, for: app)
                case .noIcon?: store.markNoIcon(for: app)
                case .missing?, .error?, .deferred?, nil: break
                }
            }
            progress(catalog, missing.count)
        }
    }
}
