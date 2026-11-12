// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL || TIP_JAR || APP_REVIEW

import AndariumCore
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// Home's one toolbar menu: Info, and each feature that is compiled in and
/// available. A build that has all three features answers every combination of
/// them; a build with fewer answers its own; a build with none has no menu and
/// is not here (`HomePlainInfoTests`).
@MainActor
@Suite(.serialized)
struct HomeMenuTests {
    private static let arranged = TipJarProductsFixture.arranged

    private func model(guest: Bool = true, feedback: Bool = true, review: Bool = true, tips: Bool = true) -> AppModel {
        let host = HostDevice(
            name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
        )
        let model = AppModel(
            guest: guest ? Guest.fresh(on: host) : nil, host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
        model.launchApplied = true
        model.driver = StubDriver(plan: .normal)
        model.phase = .home
        #if FEEDBACK_MAIL
        model.feedbackServices.address = { feedback ? "feedback@example.com" : nil }
        #endif
        #if APP_REVIEW
        model.reviewServices.link = { review ? ReviewLink(identifier: "id1234567890") : nil }
        #endif
        #if TIP_JAR
        model.tipJarServices.products = { tips ? Self.arranged : nil }
        #endif
        return model
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    // MARK: What the menu holds

    #if FEEDBACK_MAIL && TIP_JAR && APP_REVIEW
    /// All three features: the eight combinations, in the order the menu shows them.
    @Test func theMenuFollowsTheFeaturesInEachOfTheEightCombinations() {
        for feedback in [false, true] {
            for review in [false, true] {
                for tips in [false, true] {
                    var expected: [HomeMenuItem] = [.info]
                    if feedback { expected.append(.sendFeedback) }
                    if review { expected.append(.reviewOnAppStore) }
                    if tips { expected.append(.buyMeTokens) }
                    #expect(
                        model(feedback: feedback, review: review, tips: tips).homeMenuItems == expected,
                        "feedback \(feedback), review \(review), tips \(tips)")
                }
            }
        }
        #expect(model().homeMenuItems == [.info, .sendFeedback, .reviewOnAppStore, .buyMeTokens])
    }

    /// Welcome, before a virtual machine is set up: Send feedback is there, the other two never are.
    @Test func onWelcomeTheMenuHoldsInfoAndSendFeedbackAndNothingElse() {
        for review in [true, false] {
            for tips in [true, false] {
                #expect(
                    model(guest: false, feedback: true, review: review, tips: tips).homeMenuItems == [.info, .sendFeedback])
                #expect(model(guest: false, feedback: false, review: review, tips: tips).homeMenuItems == [.info])
            }
        }
    }
    #endif

    /// Whatever the build compiled in: each item is there exactly where its feature is configured
    /// and, for the two that need a virtual machine, where one is set up.
    @Test func eachItemIsThereWhereItsFeatureIsAvailable() {
        var expected: [HomeMenuItem] = [.info]
        #if FEEDBACK_MAIL
        expected.append(.sendFeedback)
        #endif
        #if APP_REVIEW
        expected.append(.reviewOnAppStore)
        #endif
        #if TIP_JAR
        expected.append(.buyMeTokens)
        #endif
        #expect(model().homeMenuItems == expected)
        var onWelcome: [HomeMenuItem] = [.info]
        #if FEEDBACK_MAIL
        onWelcome.append(.sendFeedback)
        #endif
        #expect(model(guest: false).homeMenuItems == onWelcome)
        #expect(model(feedback: false, review: false, tips: false).homeMenuItems == [.info])
    }

    @Test func eachItemDoesWhatItsEntranceDid() {
        let model = model()
        model.choose(.info)
        #expect(model.sheet == .info)
        model.sheet = nil
        #if FEEDBACK_MAIL
        model.choose(.sendFeedback)
        #expect(model.feedbackSheetOpen)
        model.feedbackSheetOpen = false
        #endif
        #if APP_REVIEW
        var opened: [URL] = []
        model.reviewServices.open = { opened.append($0) }
        model.choose(.reviewOnAppStore)
        #expect(opened.map(\.absoluteString) == ["https://apps.apple.com/app/id1234567890?action=write-review"])
        #expect(model.sheet == nil, "the item opens the App Store, not a sheet")
        #endif
        #if TIP_JAR
        model.choose(.buyMeTokens)
        #expect(model.tipJarSheetOpen)
        model.tipJarSheetOpen = false
        #endif
        #expect(model.sheet == nil)
    }

    // MARK: The real toolbar

    private func host(_ model: AppModel) async throws -> (UIViewController, HostWindow.Lease) {
        let lease = try await HostWindow.take()
        let controller = AndariumHostingController(
            rootView: RootView(entry: AppEntry(overrides: LaunchOverrides())).environment(model), model: model)
        lease.window.rootViewController = controller
        guard await eventually({ controller.viewIfLoaded?.window != nil }) else {
            lease.give()
            throw HostWindow.Failure.noKeyWindow
        }
        return (controller, lease)
    }

    /// The bar holds one menu button, which keeps Info's own accessibility label, in place of
    /// the Info button.
    @Test func theToolbarHoldsOneMenuInPlaceOfTheInfoButton() async throws {
        let model = model()
        let (controller, lease) = try await host(model)
        defer { lease.give() }
        let shown = await eventually {
            ToolbarProbe.barItems(under: controller).contains { $0.menu != nil }
        }
        try #require(shown, "the toolbar shows a menu; it holds: \(ToolbarProbe.describe(under: controller))")
        try await Task.sleep(for: .milliseconds(500))
        let items = ToolbarProbe.barItems(under: controller)
        #expect(items.filter { $0.menu != nil }.count == 1, "one menu, not one per feature")
        #expect(items.count == 1, "Info is in the menu, not beside it")
        #expect(items.first?.title == "Info" || items.first?.accessibilityLabel == "Info", "it reads Info: \(ToolbarProbe.describe(under: controller))")
        #expect(model.homeMenuItems.first == .info)
    }

    /// What VoiceOver finds in the navigation bar: buttons labelled Info (the bar's button and
    /// the control inside it), which are the menu's control (a hint on a toolbar menu never
    /// reaches it, so the label carries the meaning).
    @Test func voiceOverFindsTheMenuAsTheInfoButton() async throws {
        let model = model()
        let (controller, lease) = try await host(model)
        defer { lease.give() }
        let shown = await eventually {
            ToolbarProbe.barItems(under: controller).contains { $0.menu != nil }
        }
        try #require(shown, "the toolbar shows a menu")
        try await Task.sleep(for: .milliseconds(500))
        let top = ToolbarProbe.elements(in: lease.window)
        let buttons = top.filter { $0.traits.contains(.button) && $0.top < 100 }
        #expect(!buttons.isEmpty && buttons.allSatisfy { $0.label == "Info" }, "the bar: \(top.map(\.description))")
    }

    #if TIP_JAR || APP_REVIEW
    @Test func withNothingAvailableInfoIsThePlainButtonAgain() async throws {
        let model = model(guest: false, feedback: false, review: true, tips: true)
        let (controller, lease) = try await host(model)
        defer { lease.give() }
        try await Task.sleep(for: .seconds(1))
        #expect(model.homeMenuItems == [.info])
        #expect(!ToolbarProbe.barItems(under: controller).isEmpty, "the toolbar still shows Info")
        #expect(ToolbarProbe.barItems(under: controller).allSatisfy { $0.menu == nil }, "no menu holds Info alone")
    }
    #endif

    #if FEEDBACK_MAIL
    @Test func sendFeedbackOpensOverHomeFromTheMenu() async throws {
        let model = model()
        let (controller, lease) = try await host(model)
        defer { lease.give() }
        model.choose(.sendFeedback)
        #expect(await eventually { controller.presentedViewController != nil }, "the sheet is over Home")
        model.feedbackSheetOpen = false
        #expect(await eventually { controller.presentedViewController == nil })
    }
    #endif

    #if TIP_JAR
    @Test func buyMeTokensOpensOverHomeFromTheMenuAndNeverByItself() async throws {
        let model = model()
        let (controller, lease) = try await host(model)
        defer { lease.give() }
        // Nothing prompts: Home stays as it is while the app waits.
        try await Task.sleep(for: .seconds(2))
        #expect(controller.presentedViewController == nil && !model.tipJarSheetOpen)
        model.choose(.buyMeTokens)
        #expect(await eventually { controller.presentedViewController != nil }, "the sheet is over Home")
        model.tipJarSheetOpen = false
        #expect(await eventually { controller.presentedViewController == nil })
    }
    #endif
}

#endif
