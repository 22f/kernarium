// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

private func component(_ package: String, _ activity: String = "com.example.Main") -> AndroidComponent {
    AndroidComponent(packageName: package, activityClass: activity)!
}

private func app(_ package: String, _ label: String, builtIn: Bool = false, tag: String = "1", activity: String = "com.example.Main")
    -> AndroidApp
{
    AndroidApp(component: component(package, activity), label: label, builtIn: builtIn, iconTag: tag)
}

private func makeDirectory() -> URL {
    FileManager.default.temporaryDirectory.appendingPathComponent("andarium-apps-tests-\(UUID().uuidString)")
}

/// A tool that answers as scripted and counts what it was asked.
private final class FakeTool: AndroidAppsTool, @unchecked Sendable {
    private let lock = NSLock()
    var listing: AndroidAppListing
    var digestAnswer: AndroidAppDigestAnswer?
    var icons: [String: AndroidAppIconAnswer] = [:]
    var opens: [AndroidComponent: AndroidAppOpenAnswer] = [:]
    /// The most icons one ask is answered for; a bigger ask gets no answer (the tool ran out of time).
    var answersOnlyUpTo: Int?
    /// Icons answered `deferred` the first time they are asked for.
    var deferredOnce: Set<String> = []
    private(set) var iconAsks: [[String]] = []
    private(set) var openAsks: [AndroidComponent] = []
    private(set) var listAsks = 0
    private(set) var digestAsks = 0

    init(_ listing: AndroidAppListing) { self.listing = listing }

    var askedIcons: [String] { iconAsks.flatMap { $0 } }

    func digest(limit: Duration) async -> AndroidAppDigestAnswer {
        lock.withLock { digestAsks += 1 }
        if let digestAnswer { return digestAnswer }
        switch listing {
        case let .apps(list): return .digest(list.digest ?? "none")
        case .unavailable: return .unavailable
        case .failed: return .failed
        }
    }

    func list(limit: Duration) async -> AndroidAppListing {
        lock.withLock { listAsks += 1 }
        return listing
    }

    func icons(of apps: [AndroidApp], limit: Duration) async -> [String: AndroidAppIconAnswer] {
        let asked = lock.withLock { () -> [String] in
            iconAsks.append(apps.map(\.id))
            return iconAsks.dropLast().flatMap { $0 }
        }
        if let answersOnlyUpTo, apps.count > answersOnlyUpTo { return [:] }
        return Dictionary(uniqueKeysWithValues: apps.compactMap { app in
            if deferredOnce.contains(app.id), !asked.contains(app.id) { return (app.id, .deferred) }
            return icons[app.id].map { (app.id, $0) }
        })
    }

    func open(_ component: AndroidComponent, limit: Duration) async -> AndroidAppOpenAnswer {
        lock.withLock { openAsks.append(component) }
        return opens[component] ?? .failed
    }
}

struct AndroidComponentTests {
    @Test func theIdentifierIsThePackageAndTheClass() {
        let c = component("com.example.app", "com.example.app.Main$Launcher")
        #expect(c.identifier == "com.example.app/com.example.app.Main$Launcher")
        #expect(AndroidComponent(identifier: c.identifier) == c)
    }

    @Test func aPackageNameIsAtLeastTwoSegmentsOfIdentifiers() {
        for good in ["com.example", "a.b", "com.example.app_1", "Com.Example2"] {
            #expect(AndroidComponent.isPackageName(good), "\(good)")
        }
        for bad in [
            "", "example", "com..example", ".com.example", "com.example.", "com.1example", "com.exa mple", "com.example;id",
            "-n.example", "com.exämple", "com.example'", "com.example\n", "com/example", String(repeating: "a.", count: 200) + "b",
        ] {
            #expect(!AndroidComponent.isPackageName(bad), "\(bad)")
        }
    }

    @Test func aClassNameIsJavaIdentifiersWithAnOptionalLeadingDot() {
        for good in ["com.example.Main", "com.example.Main", "com.example.Main$Launcher", "Main", "com.example.$Hidden", "_a._b"] {
            #expect(AndroidComponent.isActivityClass(good), "\(good)")
        }
        for bad in [
            "", ".", ".Main", "..Main", "com.example.", "com..Main", "-n", "--es", "com.example.Main'", "com.example.Main\"", "a;b",
            "a|b", "a&b", "a`b", "a$(id)", "a b", "a\nb", "a/b", "a\\b", "a<b", "a>b", "(a)", "Mäin", "1Main",
            String(repeating: "a", count: 256),
        ] {
            #expect(!AndroidComponent.isActivityClass(bad), "\(bad)")
        }
    }

    @Test func theLongestTokenTheToolListsIsAComponentAndOneByteMoreOnEitherSideIsNot() {
        let package = String(repeating: "a", count: 253) + ".x"
        let activity = String(repeating: "b", count: 255)
        let longest = AndroidComponent(packageName: package, activityClass: activity)
        #expect(longest?.identifier.utf8.count == 511)
        #expect(AndroidComponent(identifier: longest?.identifier ?? "") == longest)
        #expect(AndroidComponent(packageName: package + "y", activityClass: activity) == nil)
        #expect(AndroidComponent(packageName: package, activityClass: activity + "c") == nil)
    }

    @Test func anIdentifierWithAnythingElseIsNoComponent() {
        for bad in ["", "com.example", "com.example/", "/com.example.Main", "com.example/.Main", "com.example/com.example.Main/extra", "com.example/com.example.Main;id"] {
            #expect(AndroidComponent(identifier: bad) == nil, "\(bad)")
        }
    }
}

struct AndroidAppTextTests {
    @Test func aLabelIsMadeSafeToShow() {
        #expect(AndroidAppText.label("  Comet\tHop \u{202E}\n", fallback: "x") == "Comet Hop")
        #expect(AndroidAppText.label("a\u{2066}b\u{2069}", fallback: "x") == "ab")
        #expect(AndroidAppText.label("   ", fallback: "com.example") == "com.example")
        // Characters that draw nothing and are no format character: the Hangul fillers and the blank braille.
        #expect(AndroidAppText.label("\u{3164}\u{115F}\u{2800}\u{FFA0}\u{1160}", fallback: "com.example") == "com.example")
        #expect(AndroidAppText.label("\u{3164}Comet\u{2800}Hop\u{3164}", fallback: "x") == "Comet Hop")
        #expect(AndroidAppText.label(String(repeating: "é", count: 1_000), fallback: "x").count == AndroidAppLimits.longestLabel)
    }

    @Test func theAppsAreInTheOrderThePersonReads() {
        let apps = [app("com.b.app", "banana"), app("com.a.app", "Apple"), app("com.c.app", "apple"), app("com.d.app", "10 Ten"), app("com.e.app", "9 Nine")]
        let ordered = apps.sorted(by: AndroidAppText.inOrder).map(\.label)
        #expect(ordered.map { $0.lowercased() } == ["9 nine", "10 ten", "apple", "apple", "banana"])
    }
}

struct AndroidAppStoreTests {
    @Test func invalidatingMakesTheNextRefreshListAgain() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let comet = app("com.example.comet", "Comet Hop")
        let tool = FakeTool(.apps(AndroidAppList(apps: [comet], digest: "d1")))
        tool.icons[comet.id] = .noIcon
        _ = await AndroidAppRefresh.run(tool: tool, store: store, now: Date(timeIntervalSince1970: 1))
        _ = await AndroidAppRefresh.run(tool: tool, store: store, now: Date(timeIntervalSince1970: 2))
        #expect(tool.listAsks == 1)
        store.invalidate()
        #expect(store.catalog()?.digest == nil)
        #expect(store.catalog()?.apps == [comet])
        _ = await AndroidAppRefresh.run(tool: tool, store: store, now: Date(timeIntervalSince1970: 3))
        #expect(tool.listAsks == 2)
        #expect(AndroidAppStore(directory: directory).catalog()?.digest == "d1")
    }

    @Test func aListingIsKeptAndReadBackByAnotherStore() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        #expect(store.catalog() == nil)
        let comet = app("com.example.comet", "Comet Hop")
        try store.apply(AndroidAppList(apps: [comet, app("com.android.settings", "Settings", builtIn: true)], digest: "d1"), at: Date(timeIntervalSince1970: 100))
        store.setIcon(Data([1, 2, 3]), for: comet)

        let again = AndroidAppStore(directory: directory)
        let catalog = try #require(again.catalog())
        #expect(catalog.apps.map(\.label) == ["Comet Hop", "Settings"])
        #expect(catalog.digest == "d1")
        #expect(catalog.listedAt == Date(timeIntervalSince1970: 100))
        #expect(again.icon(forID: comet.id) == Data([1, 2, 3]))
        #expect(again.icon(forID: "com.android.settings/com.example.Main") == nil)
    }

    @Test func anIconOfAnotherTagIsNotTheIconOfTheApp() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let old = app("com.example.comet", "Comet Hop", tag: "1")
        try store.apply(AndroidAppList(apps: [old], digest: nil), at: Date())
        store.setIcon(Data([1]), for: old)
        let changed = app("com.example.comet", "Comet Hop", tag: "2")
        #expect(store.icon(of: changed) == nil)
        try store.apply(AndroidAppList(apps: [changed], digest: nil), at: Date())
        // The old picture went with the tag it belonged to.
        #expect(store.icon(of: old) == nil)
        #expect(store.appsMissingIcons(in: AndroidAppList(apps: [changed], digest: nil)) == [changed])
    }

    @Test func anIconThatIsEmptyOrEnormousIsNotKept() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let comet = app("com.example.comet", "Comet Hop")
        try store.apply(AndroidAppList(apps: [comet], digest: nil), at: Date())
        store.setIcon(Data(), for: comet)
        #expect(store.icon(of: comet) == nil)
        store.setIcon(Data(count: AndroidAppLimits.largestIcon + 1), for: comet)
        #expect(store.icon(of: comet) == nil)
        store.setIcon(Data(count: AndroidAppLimits.largestIcon), for: comet)
        #expect(store.icon(of: comet) != nil)
    }

    @Test func anEraseTakesTheAppsAndKeepsTheRecords() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let comet = app("com.example.comet", "Comet Hop")
        try store.apply(AndroidAppList(apps: [comet], digest: "d"), at: Date(timeIntervalSince1970: 5))
        store.setIcon(Data([9]), for: comet)

        store.erase()
        #expect(store.catalog() == nil)
        #expect(store.icon(forID: comet.id) == nil)
        let record = try #require(store.record(forID: comet.id))
        #expect(record.label == "Comet Hop")
        #expect(record.component == comet.component)

        // The records survive a new process too, and a reinstall lists the app again.
        let later = AndroidAppStore(directory: directory)
        #expect(later.catalog() == nil)
        #expect(later.record(forID: comet.id)?.label == "Comet Hop")
        try later.apply(AndroidAppList(apps: [app("com.example.comet", "Comet Hop 2")], digest: "e"), at: Date(timeIntervalSince1970: 9))
        #expect(later.record(forID: comet.id)?.label == "Comet Hop 2")
        #expect(later.catalog()?.apps.count == 1)
    }

    @Test func theRecordsAreBoundedAndTheOldestGoFirst() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let first = (0..<600).map { app("com.example.a\($0)", "A\($0)") }
        try store.apply(AndroidAppList(apps: first, digest: nil), at: Date(timeIntervalSince1970: 1))
        let second = (0..<600).map { app("com.example.b\($0)", "B\($0)") }
        try store.apply(AndroidAppList(apps: second, digest: nil), at: Date(timeIntervalSince1970: 2))
        #expect(store.records().count == AndroidAppLimits.mostRecords)
        #expect(store.record(forID: "com.example.b0/com.example.Main") != nil)
        // 200 of the first listing's records went, which no listing names any more.
        #expect(store.records().filter { $0.id.hasPrefix("com.example.a") }.count == 400)
    }

    @Test func aListingLongerThanTheBoundIsCut() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let many = (0..<(AndroidAppLimits.mostApps + 50)).map { app("com.example.a\($0)", "A\($0)") }
        try store.apply(AndroidAppList(apps: many, digest: nil), at: Date())
        #expect(store.catalog()?.apps.count == AndroidAppLimits.mostApps)
    }

    @Test func aCatalogOfAnotherSchemaOrNoJSONReadsAsNone() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let catalogFolder = directory.appendingPathComponent("Catalog", isDirectory: true)
        try FileManager.default.createDirectory(at: catalogFolder, withIntermediateDirectories: true)
        try Data("not json".utf8).write(to: catalogFolder.appendingPathComponent("catalog.json"))
        try Data("[]".utf8).write(to: directory.appendingPathComponent("records.json"))
        let store = AndroidAppStore(directory: directory)
        #expect(store.catalog() == nil)
        #expect(store.records().isEmpty)
    }

    @Test func theStoreIsLeftOutOfTheBackup() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        try store.apply(AndroidAppList(apps: [app("com.example.comet", "Comet Hop")], digest: nil), at: Date())
        let values = try directory.resourceValues(forKeys: [.isExcludedFromBackupKey])
        #expect(values.isExcludedFromBackup == true)
    }
}

struct AndroidAppRefreshTests {
    @Test func theNamesAreKeptFirstAndTheIconsFollowInBatches() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let apps = (0..<10).map { app("com.example.a\($0)", "A\($0)") }
        let tool = FakeTool(.apps(AndroidAppList(apps: apps.reversed(), digest: "d")))
        for app in apps { tool.icons[app.id] = .png(Data([1]), hasAlpha: false) }
        let seen = ProgressLog()
        let result = await AndroidAppRefresh.run(tool: tool, store: store, now: Date(timeIntervalSince1970: 7)) { catalog, left in
            seen.add(catalog.apps.map(\.label), left, store.appsMissingIcons(in: AndroidAppList(apps: catalog.apps, digest: nil)).count)
        }
        guard case let .refreshed(catalog) = result else { Issue.record("not refreshed"); return }
        #expect(catalog.apps.map(\.label) == ["A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7", "A8", "A9"])
        // Eight icons to an ask, then the two that are left.
        #expect(tool.iconAsks.map(\.count) == [8, 2])
        #expect(seen.entries.map(\.left) == [10, 2, 0])
        // Each progress call saw the store as it was then: names first, a batch more each time.
        #expect(seen.entries.map(\.stored) == [10, 2, 0])
        #expect(store.icon(forID: apps[0].id) == Data([1]))
    }

    @Test func anUnchangedDigestAsksForNoListingAndNoIconThatItHas() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let a = app("com.example.aaa", "Aaa")
        let tool = FakeTool(.apps(AndroidAppList(apps: [a], digest: "d")))
        tool.icons = [a.id: .png(Data([1]), hasAlpha: false)]
        _ = await AndroidAppRefresh.run(tool: tool, store: store, now: Date(timeIntervalSince1970: 1))
        _ = await AndroidAppRefresh.run(tool: tool, store: store, now: Date(timeIntervalSince1970: 2))
        #expect(tool.listAsks == 1)
        #expect(tool.askedIcons.count == 1)
        #expect(tool.digestAsks == 2)
        // The time of the check is the list's time.
        #expect(store.catalog()?.listedAt == Date(timeIntervalSince1970: 2))
        // A new digest lists again and a new tag asks for the icon again.
        tool.listing = .apps(AndroidAppList(apps: [app("com.example.aaa", "Aaa", tag: "2")], digest: "e"))
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        #expect(tool.listAsks == 2)
        #expect(tool.askedIcons.count == 2)
    }

    @Test func anIconOwedAtTheLastRefreshIsAskedForEvenWhenTheDigestHeld() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let a = app("com.example.aaa", "Aaa")
        let tool = FakeTool(.apps(AndroidAppList(apps: [a], digest: "d")))
        tool.icons = [a.id: .error]
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        #expect(store.icon(forID: a.id) == nil)
        tool.icons = [a.id: .png(Data([1]), hasAlpha: false)]
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        #expect(tool.listAsks == 1)
        #expect(store.icon(forID: a.id) == Data([1]))
    }

    @Test func anAppWithNoIconIsNotAskedForAgain() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let a = app("com.example.aaa", "Aaa")
        let tool = FakeTool(.apps(AndroidAppList(apps: [a], digest: "d")))
        tool.icons = [a.id: .noIcon]
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        #expect(tool.askedIcons.count == 1)
        #expect(store.declaresNoIcon(of: a))
        #expect(store.icon(forID: a.id) == nil)
        // A new tag is a new icon.
        tool.listing = .apps(AndroidAppList(apps: [app("com.example.aaa", "Aaa", tag: "2")], digest: "e"))
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        #expect(tool.askedIcons.count == 2)
    }

    @Test func aDeferredOrMissingOrUnansweredIconIsAskedForAtTheNextRefresh() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let apps = [app("com.example.a", "A"), app("com.example.b", "B"), app("com.example.c", "C")]
        let tool = FakeTool(.apps(AndroidAppList(apps: apps, digest: "d")))
        tool.icons = [apps[0].id: .deferred, apps[1].id: .missing]
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        #expect(store.appsMissingIcons(in: AndroidAppList(apps: apps, digest: nil)).count == 3)
    }

    @Test func aBatchThatGotNoAnswerIsAskedAgainInSmallerBatches() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let apps = (0..<8).map { app("com.example.a\($0)", "A\($0)") }
        let tool = FakeTool(.apps(AndroidAppList(apps: apps, digest: "d")))
        for app in apps { tool.icons[app.id] = .png(Data([1]), hasAlpha: false) }
        tool.answersOnlyUpTo = 2
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        // Eight, then four, then pairs: every icon arrived in this refresh.
        #expect(tool.iconAsks.map(\.count) == [8, 4, 2, 2, 2, 2])
        #expect(store.appsMissingIcons(in: AndroidAppList(apps: apps, digest: nil)).isEmpty)
    }

    @Test func aToolThatNeverAnswersIsGivenUpOnAfterThreeAsks() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let apps = (0..<8).map { app("com.example.a\($0)", "A\($0)") }
        let tool = FakeTool(.apps(AndroidAppList(apps: apps, digest: "d")))
        tool.answersOnlyUpTo = 0
        let result = await AndroidAppRefresh.run(tool: tool, store: store)
        #expect(tool.iconAsks.map(\.count) == [8, 4, 2])
        guard case .refreshed = result else { Issue.record("the names are kept even when no icon comes: \(result)"); return }
        #expect(store.catalog()?.apps.count == 8)
        #expect(store.appsMissingIcons(in: AndroidAppList(apps: apps, digest: nil)).count == 8)
    }

    @Test func anIconTheToolDeferredIsAskedAgainWithinTheRefreshButNotWithoutEnd() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let apps = [app("com.example.a", "A"), app("com.example.b", "B")]
        let tool = FakeTool(.apps(AndroidAppList(apps: apps, digest: "d")))
        tool.icons = [apps[0].id: .png(Data([1]), hasAlpha: false), apps[1].id: .deferred]
        tool.deferredOnce = [apps[0].id]
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        // A came back deferred once and arrived at the second ask; B is deferred every time and is asked three times.
        #expect(store.icon(forID: apps[0].id) == Data([1]))
        #expect(tool.askedIcons.filter { $0 == apps[1].id }.count == 1 + AndroidAppLimits.deferredRetries)
        #expect(store.icon(forID: apps[1].id) == nil)
    }

    @Test func theLimitOfOneIconsAskIsFortySeconds() {
        #expect(AndroidAppLimits.iconLimit == .seconds(40))
    }

    @Test func aListingThatFailsOrIsMissingChangesNothing() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        try store.apply(AndroidAppList(apps: [app("com.example.aaa", "Aaa")], digest: "d"), at: Date())
        let tool = FakeTool(.failed)
        #expect(await AndroidAppRefresh.run(tool: tool, store: store) == .failed)
        tool.listing = .unavailable
        #expect(await AndroidAppRefresh.run(tool: tool, store: store) == .unavailable)
        // A digest that answers but a listing that does not.
        tool.listing = .failed
        tool.digestAnswer = .digest("other")
        #expect(await AndroidAppRefresh.run(tool: tool, store: store) == .failed)
        #expect(store.catalog()?.apps.map(\.label) == ["Aaa"])
    }
}

private final class ProgressLog: @unchecked Sendable {
    struct Entry { var labels: [String]; var left: Int; var stored: Int }
    private let lock = NSLock()
    private var all: [Entry] = []
    var entries: [Entry] { lock.withLock { all } }
    func add(_ labels: [String], _ left: Int, _ stored: Int) { lock.withLock { all.append(Entry(labels: labels, left: left, stored: stored)) } }
}

struct AndroidAppOpenTests {
    @Test func anAppThatOpensIsOpened() async {
        let c = component("com.example.comet")
        let tool = FakeTool(.failed)
        tool.opens[c] = .opened
        #expect(await tool.open(c) == .opened)
        #expect(tool.listAsks == 0)
    }

    @Test func aFailedCommandIsAFailureAndNeverAGuess() async {
        let c = component("com.example.comet")
        let tool = FakeTool(.apps(AndroidAppList(apps: [], digest: nil)))
        tool.opens[c] = .failed
        #expect(await tool.open(c) == .failed)
        #expect(tool.listAsks == 0)
    }

    @Test func aLauncherThatMovedIsFollowedWhenThePackageHasExactlyOne() async {
        let old = component("com.example.comet", "com.example.OldMain")
        let new = component("com.example.comet", "com.example.NewMain")
        let tool = FakeTool(.apps(AndroidAppList(apps: [app("com.example.comet", "Comet", activity: "com.example.NewMain")], digest: nil)))
        tool.opens[old] = .notFound
        tool.opens[new] = .opened
        #expect(await tool.open(old) == .openedRenamed(new))
        #expect(tool.openAsks == [old, new])
    }

    @Test func aPackageWithTwoLauncherActivitiesIsNotGuessedAt() async {
        let old = component("com.example.comet", "com.example.OldMain")
        let tool = FakeTool(.apps(AndroidAppList(apps: [
            app("com.example.comet", "Comet", activity: "com.example.A"), app("com.example.comet", "Comet 2", activity: "com.example.B"),
        ], digest: nil)))
        tool.opens[old] = .notFound
        #expect(await tool.open(old) == .notInAndroid)
        #expect(tool.openAsks == [old])
    }

    @Test func aPackageThatIsGoneIsNotInAndroid() async {
        let old = component("com.example.comet", "com.example.OldMain")
        let tool = FakeTool(.apps(AndroidAppList(apps: [app("com.example.other", "Other")], digest: nil)))
        tool.opens[old] = .notFound
        #expect(await tool.open(old) == .notInAndroid)
    }

    @Test func aMissingListingAfterARefusalIsAFailureNotAMissingApp() async {
        let old = component("com.example.comet", "com.example.OldMain")
        let tool = FakeTool(.failed)
        tool.opens[old] = .notFound
        #expect(await tool.open(old) == .failed)
        tool.listing = .unavailable
        #expect(await tool.open(old) == .failed)
    }

    @Test func theSameLauncherListedAgainIsNotARename() async {
        let old = component("com.example.comet", "com.example.Main")
        let tool = FakeTool(.apps(AndroidAppList(apps: [app("com.example.comet", "Comet", activity: "com.example.Main")], digest: nil)))
        tool.opens[old] = .notFound
        #expect(await tool.open(old) == .notInAndroid)
        #expect(tool.openAsks == [old])
    }

    @Test func aRenamedLauncherThatDoesNotOpenEndsAsTheAnswerSays() async {
        let old = component("com.example.comet", "com.example.OldMain")
        let new = component("com.example.comet", "com.example.NewMain")
        let tool = FakeTool(.apps(AndroidAppList(apps: [app("com.example.comet", "Comet", activity: "com.example.NewMain")], digest: nil)))
        tool.opens[old] = .notFound
        tool.opens[new] = .notFound
        #expect(await tool.open(old) == .notInAndroid)
        tool.opens[new] = .failed
        #expect(await tool.open(old) == .failed)
    }
}
