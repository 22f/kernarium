// SPDX-License-Identifier: GPL-2.0-or-later

#if TIP_JAR

import AndariumCore
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The sender line of Buy me tokens: a circular avatar, the developer's name and role, and a logo
/// button for each profile. The values here are the arrangement's placeholders (`example`) and
/// images drawn in the test; the maker's own are local to an AppStore build.
@MainActor
@Suite(.serialized)
struct TipJarHeaderTests {
    /// The sites the build gives a button: the header's X button exists only where the build names
    /// a handle for it (`SHEET_X`).
    private static var sites: [TipJarSenderContent.Link.Site] {
        #if SHEET_X
        [.github, .x]
        #else
        [.github]
        #endif
    }

    private static func drawn() -> UIImage {
        UIGraphicsImageRenderer(size: CGSize(width: 24, height: 20)).image { context in
            UIColor.black.setFill()
            context.fill(CGRect(x: 2, y: 2, width: 20, height: 16))
        }
    }

    private static func content(
        name: String? = "example", github: String? = "example", x: String? = "example_x",
        avatar: Bool = true, githubLogo: Bool = true, xLogo: Bool = true
    ) -> TipJarSenderContent {
        TipJarSenderContent(
            developer: TipJarDeveloper(name: name, githubHandle: github, xHandle: x),
            avatar: avatar ? drawn() : nil, githubLogo: githubLogo ? drawn() : nil, xLogo: xLogo ? drawn() : nil)
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    // MARK: What the header holds

    @Test func theArtworkMakesTheWholeHeader() throws {
        let header = Self.content()
        #expect(header.name == "example")
        #expect(header.avatar != nil)
        #expect(header.links.map(\.site) == Self.sites)
        let addresses = ["github": "https://github.com/example", "x": "https://x.com/example_x"]
        #expect(header.links.map(\.url.absoluteString) == header.links.map { addresses[$0.site.rawValue]! })
        #expect(header.links.map(\.name) == header.links.map { $0.site == .github ? "GitHub" : "X" },
                "the brands' names, whatever the language")
        for link in header.links {
            #expect(link.logo.renderingMode == .alwaysTemplate, "\(link.name) takes the text's colour")
            #expect(link.aspectRatio == 1.2, "\(link.name) keeps the logo's proportions")
        }
    }

    /// Each piece follows what exists: no avatar keeps the mark, no name reads the role alone, and a
    /// logo button needs its handle and its logo.
    @Test func eachPieceFollowsWhatExists() {
        let role = String(localized: TipJarWords.sender)
        let anonymous = TipJarSenderContent()
        #expect(anonymous.avatar == nil && anonymous.links.isEmpty && anonymous.name == nil)
        #expect(anonymous.line == role && anonymous.spokenLine == role)
        let named = Self.content(github: nil, x: nil)
        #expect(named.line == "example \u{00B7} \(role)" && named.spokenLine == "example, \(role)")
        #expect(named.links.isEmpty)
        #expect(Self.content(avatar: false).avatar == nil)
        #expect(Self.content(name: nil).line == role)
        #expect(Self.content(githubLogo: false).links.map(\.site) == Self.sites.filter { $0 != .github }, "no logo, no button")
        #expect(Self.content(github: nil).links.map(\.site) == Self.sites.filter { $0 != .github }, "no handle, no button")
        #expect(Self.content(github: nil, xLogo: false).links.isEmpty)
        // A handle that is no account name builds no address.
        #expect(Self.content(github: "-bad", x: "way_too_long_for_an_x_account").links.isEmpty)
        #expect(Self.content(github: "a/b", x: "a b").links.isEmpty)
    }

    /// The build's own values and assets: whatever Info.plist and the product hold, no more.
    @Test func theLiveHeaderHoldsExactlyWhatTheBuildHolds() {
        typealias Configuration = TipJarDeveloperConfiguration
        let live = TipJarSenderContent.live
        func key(_ name: String) -> String? { Bundle.main.object(forInfoDictionaryKey: name) as? String }
        #expect(live.name == key(Configuration.nameKey))
        #expect((live.avatar != nil) == (UIImage(named: Configuration.avatarAsset) != nil))
        #expect((live.avatar != nil) == (key(Configuration.nameKey) != nil), "the avatar comes with the name")
        #expect(live.links.contains { $0.site == .github } == (key(Configuration.githubKey) != nil && UIImage(named: Configuration.githubAsset) != nil))
        #expect(live.links.contains { $0.site == .github } == (key(Configuration.githubKey) != nil), "the mark comes with the handle")
        #if SHEET_X
        #expect(live.links.contains { $0.site == .x } == (key(Configuration.xKey) != nil && UIImage(named: Configuration.xAsset) != nil))
        #else
        #expect(!live.links.contains { $0.site == .x }, "no X button without a handle of its own")
        #expect(key(Configuration.xKey) == nil)
        #endif
        // The arrangement states all three placeholders and takes the drawn artwork.
        if key(Configuration.nameKey) == "example" {
            #expect(live.name == "example" && live.avatar != nil)
            #expect(live.links.map(\.url.absoluteString).first == "https://github.com/example")
        }
    }

    // MARK: The sheet

    private func app() -> AppModel {
        let host = HostDevice(
            name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
        model.launchApplied = true
        model.driver = StubDriver(plan: .normal)
        model.phase = .home
        model.tipJarServices.products = { TipJarProductsFixture.arranged }
        model.tipJarServices.store = TipJarStoreServices(
            canMakePayments: { true },
            products: { ids in ids.map { TipJarProduct(identifier: $0, displayPrice: "$1.00") } },
            purchase: { _ in .cancelled },
            transactions: { AsyncStream { $0.finish() } })
        return model
    }

    /// Opens the real sheet over Home and returns what VoiceOver finds in it.
    private func openSheet(_ model: AppModel, lease: HostWindow.Lease) async throws -> (UIViewController, [ToolbarProbe.Element]) {
        let controller = AndariumHostingController(
            rootView: RootView(entry: AppEntry(overrides: LaunchOverrides())).environment(model), model: model)
        lease.window.rootViewController = controller
        try #require(await eventually { controller.viewIfLoaded?.window != nil })
        model.tipJarSheetOpen = true
        try #require(await eventually { controller.presentedViewController?.view.window != nil })
        try await Task.sleep(for: .seconds(1.5))
        return (controller, ToolbarProbe.elements(in: lease.window, above: 5000, includeUnlabelled: true))
    }

    /// One sender element, read once before the messages, with the role in it; each logo its own
    /// link; the avatar nowhere in the accessibility tree.
    @Test func voiceOverReadsTheSenderOnceAndFindsTheLinks() async throws {
        let model = app()
        var opened: [URL] = []
        model.tipJarServices.sender = { Self.content() }
        model.tipJarServices.openLink = { opened.append($0); return .handled }
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (controller, elements) = try await openSheet(model, lease: lease)
        let sheetView = controller.presentedViewController?.view
        let role = String(localized: TipJarWords.sender)
        let sender = elements.filter { $0.label == "example, \(role)" }
        try #require(sender.count == 1, "one sender element: \(elements.map(\.description))")
        #expect(sender[0].traits.contains(.header))
        // Before the first bubble.
        let bubble = String(localized: TipJarWords.coffee)
        let first = try #require(elements.first { $0.label == bubble || $0.label.hasPrefix(String(bubble.prefix(12))) })
        #expect(sender[0].top < first.top, "the sender comes before the messages")
        // No element without a label in the header's band: the avatar is decorative. (A control of Home
        // behind a form sheet is not the sheet's.)
        func inSheet(_ element: ToolbarProbe.Element) -> Bool {
            guard let view = element.object as? UIView, let sheetView else { return true }
            return view.isDescendant(of: sheetView)
        }
        let band = elements.filter { $0.label.isEmpty && abs($0.top - sender[0].top) < 40 && inSheet($0) }
        #expect(band.isEmpty, "unlabelled elements by the sender: \(band.map(\.description))")
        // Each link its own element with the link trait, labelled with the brand.
        let brands = Self.sites.map { $0 == .github ? "GitHub" : "X" }
        let links = elements.filter { $0.traits.contains(.link) }
        #expect(links.map(\.label).sorted() == brands.sorted(), "\(elements.map(\.description))")
        // A tap hands the profile to openURL, which the arrangement's seam sees.
        for name in brands {
            let link = try #require(links.first { $0.label == name })
            #expect(link.object.accessibilityActivate(), "\(name) activates")
        }
        #expect(await eventually { opened.count == brands.count })
        let addresses = ["GitHub": "https://github.com/example", "X": "https://x.com/example_x"]
        #expect(opened.map(\.absoluteString).sorted() == brands.map { addresses[$0]! }.sorted())
    }

    /// At the largest text size the logos stay a touch target each, 44 pt tall, no larger than the
    /// target, and no two of them overlap: they used to grow with the text until they touched and
    /// rivalled the name.
    @Test func atTheLargestTextSizeTheLogosKeepTheirSizeAndTheirTargets() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = UIHostingController(
            rootView: TipJarSender(content: Self.content())
                .dynamicTypeSize(.accessibility5)
                .padding(16)
                .frame(width: 380)
                .frame(maxHeight: .infinity, alignment: .top))
        lease.window.rootViewController = controller
        var links: [CGRect] = []
        let drawn = await eventually {
            controller.view.setNeedsLayout()
            controller.view.layoutIfNeeded()
            links = ToolbarProbe.elements(in: lease.window, above: 5000, includeUnlabelled: true)
                .filter { $0.traits.contains(.link) }.map { $0.object.accessibilityFrame }
                .sorted { ($0.minY, $0.minX) < ($1.minY, $1.minX) }
            return links.count == Self.sites.count
        }
        try #require(drawn, "the logos were not drawn; is ApplicationAccessibilityEnabled on?")
        for frame in links {
            #expect(frame.width <= 48 && frame.height >= 43.5 && frame.height <= 48, "a logo's target: \(frame)")
        }
        for (first, second) in zip(links, links.dropFirst()) {
            #expect(!first.insetBy(dx: 0.5, dy: 0.5).intersects(second.insetBy(dx: 0.5, dy: 0.5)),
                    "two targets overlap: \(first) \(second)")
        }
    }

    // MARK: The gaps

    /// A solid logo, so that its drawn edges are its frame's.
    private static func solid() -> UIImage {
        UIGraphicsImageRenderer(size: CGSize(width: 24, height: 20)).image { context in
            UIColor.black.setFill()
            context.fill(CGRect(x: 0, y: 0, width: 24, height: 20))
        }
    }

    /// The runs of columns (or rows) of `image` that hold a black pixel, in points, inside `area`.
    private static func inkRuns(in image: UIImage, area: CGRect, columns: Bool) -> [ClosedRange<CGFloat>] {
        guard let cgImage = image.cgImage, let data = cgImage.dataProvider?.data, let bytes = CFDataGetBytePtr(data) else { return [] }
        let scale = image.scale
        let perRow = cgImage.bytesPerRow
        let perPixel = cgImage.bitsPerPixel / 8
        func isInk(_ x: Int, _ y: Int) -> Bool {
            let at = y * perRow + x * perPixel
            return bytes[at] < 48 && bytes[at + 1] < 48 && bytes[at + 2] < 48
        }
        let x0 = max(0, Int(area.minX * scale)), x1 = min(cgImage.width, Int(area.maxX * scale))
        let y0 = max(0, Int(area.minY * scale)), y1 = min(cgImage.height, Int(area.maxY * scale))
        guard x0 < x1, y0 < y1 else { return [] }
        let range = columns ? x0..<x1 : y0..<y1
        var runs: [ClosedRange<CGFloat>] = []
        var start: Int?
        for position in range {
            let hit = columns ? (y0..<y1).contains { isInk(position, $0) } : (x0..<x1).contains { isInk($0, position) }
            if hit, start == nil { start = position }
            if !hit, let from = start {
                runs.append(CGFloat(from) / scale ... CGFloat(position) / scale)
                start = nil
            }
        }
        if let from = start { runs.append(CGFloat(from) / scale ... CGFloat(range.upperBound) / scale) }
        return runs
    }

    /// The name, the GitHub mark and the X mark read as one tight group: the gap from the name to the
    /// first mark and between the marks is one inline gap (8 pt), at the default size, where the line
    /// holds them all, and at the largest, where the marks go under the name. The touch targets are
    /// 44 pt tall and do not space the glyphs.
    @Test(arguments: [DynamicTypeSize.large, .accessibility5])
    func theNameAndTheMarksAreOneTightGroup(size: DynamicTypeSize) async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let developer = TipJarDeveloper(name: "example", githubHandle: "example", xHandle: "example_x")
        let content = TipJarSenderContent(
            developer: developer, avatar: nil, githubLogo: Self.solid(), xLogo: Self.solid())
        let controller = UIHostingController(
            rootView: TipJarSender(content: content)
                .dynamicTypeSize(size)
                .padding(16)
                .frame(width: 380)
                .frame(maxHeight: .infinity, alignment: .top)
                .background(Color.white))
        controller.overrideUserInterfaceStyle = .light
        lease.window.rootViewController = controller
        var name: CGRect = .null
        let drawn = await eventually {
            controller.view.setNeedsLayout()
            controller.view.layoutIfNeeded()
            let elements = ToolbarProbe.elements(in: lease.window, above: 5000, includeUnlabelled: true)
            name = elements.first { $0.traits.contains(.header) }?.object.accessibilityFrame ?? .null
            return !name.isNull && elements.filter { $0.traits.contains(.link) }.count == Self.sites.count
        }
        try #require(drawn, "the header was not drawn; is ApplicationAccessibilityEnabled on?")
        try await Task.sleep(for: .milliseconds(300))
        let view = controller.view!
        let line = try #require(view.window).screen.coordinateSpace.convert(name, to: view)
        let picture = UIGraphicsImageRenderer(bounds: view.bounds).image { _ in
            view.drawHierarchy(in: view.bounds, afterScreenUpdates: true)
        }
        let wrapped = size.isAccessibilitySize
        // The marks are the ink right of the name (one line) or under it (wrapped).
        let area = wrapped
            ? CGRect(x: 0, y: line.maxY, width: view.bounds.width, height: 120)
            : CGRect(x: line.maxX, y: line.minY - 12, width: view.bounds.width - line.maxX, height: line.height + 24)
        let marks = Self.inkRuns(in: picture, area: area, columns: true)
        try #require(marks.count == Self.sites.count, "the marks drawn: \(marks) in \(area), the name at \(line)")
        let gap = 8.0
        if wrapped {
            let rows = Self.inkRuns(in: picture, area: area, columns: false)
            let top = try #require(rows.first?.lowerBound)
            #expect(top - line.maxY <= 8, "the marks sit \(top - line.maxY) pt under the name")
            #expect(abs(marks[0].lowerBound - line.minX) <= 1.5, "the first mark lines up with the name: \(marks[0]) \(line)")
        } else {
            #expect(abs(marks[0].lowerBound - line.maxX - gap) <= 1.5, "the name to the first mark: \(marks[0].lowerBound - line.maxX) pt")
        }
        for (first, second) in zip(marks, marks.dropFirst()) {
            #expect(abs(second.lowerBound - first.upperBound - gap) <= 1.5, "between the marks: \(second.lowerBound - first.upperBound) pt")
        }
    }

    // MARK: The close control

    /// Every view under `root` of the given type.
    private static func views<V: UIView>(of type: V.Type, under root: UIView) -> [V] {
        var found: [V] = []
        if let match = root as? V { found.append(match) }
        for child in root.subviews { found += views(of: type, under: child) }
        return found
    }

    /// The sheet closes with the close symbol, an icon-only toolbar button (which the folding iPhone's
    /// cover moves into the column beside its camera, as it does Settings' Done), that VoiceOver reads
    /// as "Done", and no text button of that name is left in the bar.
    @Test func theSheetClosesWithTheCloseSymbolAndVoiceOverSaysDone() async throws {
        let model = app()
        model.tipJarServices.sender = { Self.content() }
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (controller, elements) = try await openSheet(model, lease: lease)
        let done = String(localized: TipJarWords.done)
        // A bar button is two nested views, each an element: one control.
        let buttons = elements.filter { $0.label == done && $0.traits.contains(.button) }
        let views = buttons.compactMap { $0.object as? UIView }
        let outer = views.first { outer in views.allSatisfy { $0 === outer || $0.isDescendant(of: outer) } }
        try #require(!buttons.isEmpty && views.count == buttons.count && outer != nil,
                     "one Done in the sheet: \(buttons.map(\.description))")
        let symbols = Self.views(of: UIImageView.self, under: outer!).compactMap(\.image).filter(\.isSymbolImage)
        #expect(!symbols.isEmpty, "the bar's button draws a symbol: \(buttons.map(\.description))")
        let texts = Self.views(of: UILabel.self, under: outer!).compactMap(\.text)
        #expect(!texts.contains(done), "the word is not drawn in the bar: \(texts)")
        _ = controller
        // It closes the sheet.
        let control = try #require(views.compactMap { $0 as? UIControl }.first, "a control: \(views)")
        control.performPrimaryAction()
        #expect(await eventually { !model.tipJarSheetOpen })
    }

    /// Where purchases are not allowed the sheet says so once, in the title, over the dimmed tiers: no
    /// second line says it again, and VoiceOver reads the title one time.
    @Test func whereTipsAreNotAllowedTheTitleAloneSaysSo() async throws {
        let model = app()
        model.tipJarServices.sender = { Self.content() }
        model.tipJarServices.store.canMakePayments = { false }
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (_, elements) = try await openSheet(model, lease: lease)
        let title = String(localized: TipJarWords.restrictedTitle(device: "iPhone"))
        #expect(elements.filter { $0.label == title }.count == 1, "the title once: \(elements.map(\.description))")
        #expect(!elements.contains { $0.label.localizedCaseInsensitiveContains("restricted") },
                "no second line: \(elements.map(\.description))")
    }

    @Test func withoutTheDeveloperTheHeaderIsTheRoleAlone() async throws {
        let model = app()
        model.tipJarServices.sender = { TipJarSenderContent() }
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (_, elements) = try await openSheet(model, lease: lease)
        let role = String(localized: TipJarWords.sender)
        #expect(elements.filter { $0.label == role }.count == 1, "\(elements.map(\.description))")
        #expect(!elements.contains { $0.traits.contains(.link) }, "no link without a handle and a logo")
    }
}

#endif
