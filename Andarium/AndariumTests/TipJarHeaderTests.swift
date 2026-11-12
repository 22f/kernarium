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

    /// At the largest text size the logos stay a touch target each, no larger than the target, and
    /// keep a gap between them: they used to grow with the text until they touched and rivalled the name.
    @Test func atTheLargestTextSizeTheLogosKeepTheirSizeAndTheirDistance() async throws {
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
            #expect(frame.width <= 48 && frame.height <= 48, "a logo grew with the text: \(frame)")
        }
        for (first, second) in zip(links, links.dropFirst()) {
            let apart = second.minY >= first.maxY || second.minX - first.maxX >= 8
            #expect(apart, "the logos touch: \(first) \(second)")
        }
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
