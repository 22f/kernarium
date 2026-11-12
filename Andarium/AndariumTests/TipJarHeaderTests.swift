// SPDX-License-Identifier: GPL-2.0-or-later

#if TIP_JAR

import AndariumCore
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The sender line of Buy me tokens: a circular avatar, the developer's name and role, and a logo
/// button for each profile. The values here are the arrangement's placeholders (`example`) and the
/// drawn artwork of `Config/DeveloperExample`; the maker's own are local to an AppStore build.
@MainActor
@Suite(.serialized)
struct TipJarHeaderTests {
    /// The drawn stand-ins, found from this file so that no path is written in it.
    private static let example = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().deletingLastPathComponent()
        .appendingPathComponent("Config").appendingPathComponent("DeveloperExample")

    private static func path(_ name: String) -> String { example.appendingPathComponent(name).path }

    private static func content(
        name: String? = "example", github: String? = "example", x: String? = "example",
        avatar: Bool = true, githubLogo: Bool = true, xLogo: Bool = true
    ) -> TipJarSenderContent {
        TipJarSenderContent(
            developer: TipJarDeveloper(name: name, githubHandle: github, xHandle: x),
            avatarPath: avatar ? path("developer-avatar.jpeg") : nil,
            githubPath: githubLogo ? path("github-logo.svg") : nil, xPath: xLogo ? path("x-logo.svg") : nil)
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    // MARK: What the header holds

    @Test func theDrawnArtworkMakesTheWholeHeader() throws {
        let header = Self.content()
        #expect(header.name == "example")
        #expect(header.avatar != nil)
        #expect(header.links.map(\.site) == [.github, .x])
        #expect(header.links.map(\.url.absoluteString) == ["https://github.com/example", "https://x.com/example"])
        #expect(header.links.map(\.name) == ["GitHub", "X"], "the brands' names, whatever the language")
        for link in header.links {
            #expect(!link.glyph.path.boundingBoxOfPath.isEmpty, "\(link.name) draws something")
        }
    }

    /// Each piece follows what exists: no avatar keeps the mark, no name reads the role alone, and a
    /// logo button needs its handle and its file.
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
        #expect(Self.content(githubLogo: false).links.map(\.site) == [.x], "no logo file, no button")
        #expect(Self.content(github: nil).links.map(\.site) == [.x], "no handle, no button")
        #expect(Self.content(github: nil, xLogo: false).links.isEmpty)
        // A handle that is no account name builds no address.
        #expect(Self.content(github: "-bad", x: "way_too_long_for_an_x_account").links.isEmpty)
        #expect(Self.content(github: "a/b", x: "a b").links.isEmpty)
    }

    @Test func aMissingOrBrokenFileMakesNoLogoOrAvatar() throws {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("tipjar-header-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: folder) }
        let broken = folder.appendingPathComponent("broken.svg")
        try "<svg viewBox=\"0 0 10 10\"><circle r=\"3\"/></svg>".write(to: broken, atomically: true, encoding: .utf8)
        let notAnImage = folder.appendingPathComponent("avatar")
        try Data("not an image".utf8).write(to: notAnImage)
        let header = TipJarSenderContent(
            developer: TipJarDeveloper(name: "example", githubHandle: "example", xHandle: "example"),
            avatarPath: notAnImage.path, githubPath: broken.path, xPath: folder.appendingPathComponent("absent").path)
        #expect(header.avatar == nil && header.links.isEmpty && header.name == "example")
    }

    /// The build's own values and artwork: whatever Info.plist and the product hold, no more.
    @Test func theLiveHeaderHoldsExactlyWhatTheBuildHolds() {
        typealias Configuration = TipJarDeveloperConfiguration
        let live = TipJarSenderContent.live
        func key(_ name: String) -> String? { Bundle.main.object(forInfoDictionaryKey: name) as? String }
        #expect(live.name == key(Configuration.nameKey))
        #expect((live.avatar != nil) == (Configuration.artworkPath(Configuration.avatarFile) != nil))
        #expect(live.links.contains { $0.site == .github } == (key(Configuration.githubKey) != nil && Configuration.artworkPath(Configuration.githubFile) != nil))
        #expect(live.links.contains { $0.site == .x } == (key(Configuration.xKey) != nil && Configuration.artworkPath(Configuration.xFile) != nil))
        // The arrangement states all three placeholders and takes the drawn artwork.
        if key(Configuration.nameKey) == "example" {
            #expect(live.name == "example" && live.avatar != nil)
            #expect(live.links.map(\.url.absoluteString) == ["https://github.com/example", "https://x.com/example"])
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
    @Test func voiceOverReadsTheSenderOnceAndFindsTwoLinks() async throws {
        let model = app()
        var opened: [URL] = []
        model.tipJarServices.sender = { Self.content() }
        model.tipJarServices.openLink = { opened.append($0); return .handled }
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (_, elements) = try await openSheet(model, lease: lease)
        let role = String(localized: TipJarWords.sender)
        let sender = elements.filter { $0.label == "example, \(role)" }
        try #require(sender.count == 1, "one sender element: \(elements.map(\.description))")
        #expect(sender[0].traits.contains(.header))
        // Before the first bubble.
        let bubble = String(localized: TipJarWords.coffee)
        let first = try #require(elements.first { $0.label == bubble || $0.label.hasPrefix(String(bubble.prefix(12))) })
        #expect(sender[0].top < first.top, "the sender comes before the messages")
        // No element without a label in the header's band: the avatar is decorative.
        let band = elements.filter { $0.label.isEmpty && abs($0.top - sender[0].top) < 40 }
        #expect(band.isEmpty, "unlabelled elements by the sender: \(band.map(\.description))")
        // Two links, each its own element with the link trait, labelled with the brand.
        let links = elements.filter { $0.traits.contains(.link) }
        #expect(links.map(\.label).sorted() == ["GitHub", "X"], "\(elements.map(\.description))")
        // A tap hands the profile to openURL, which the arrangement's seam sees.
        for name in ["GitHub", "X"] {
            let link = try #require(links.first { $0.label == name })
            #expect(link.object.accessibilityActivate(), "\(name) activates")
        }
        #expect(await eventually { opened.count == 2 })
        #expect(opened.map(\.absoluteString).sorted() == ["https://github.com/example", "https://x.com/example"])
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
