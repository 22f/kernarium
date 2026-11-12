// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL || TIP_JAR

import AndariumCore
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// Home's one toolbar menu: Info, and each feature that is compiled in and
/// available. A build that has both features answers every combination of
/// them; a build with one answers its own; a build with neither has no menu and
/// is not here (`HomePlainInfoTests`).
@MainActor
@Suite(.serialized)
struct HomeMenuTests {
    private static let arranged = TipJarProductsFixture.arranged

    private func model(guest: Bool = true, feedback: Bool = true, tips: Bool = true) -> AppModel {
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

    #if FEEDBACK_MAIL && TIP_JAR
    @Test func theMenuFollowsTheFeaturesInEachOfTheFourCombinations() {
        #expect(model(feedback: false, tips: false).homeMenuItems == [.info])
        #expect(model(feedback: true, tips: false).homeMenuItems == [.info, .sendFeedback])
        #expect(model(feedback: false, tips: true).homeMenuItems == [.info, .buyMeTokens])
        #expect(model(feedback: true, tips: true).homeMenuItems == [.info, .sendFeedback, .buyMeTokens])
    }

    @Test func withoutAVirtualMachineBuyMeTokensIsNotThereAndSendFeedbackStays() {
        #expect(model(guest: false, feedback: true, tips: true).homeMenuItems == [.info, .sendFeedback])
        #expect(model(guest: false, feedback: false, tips: true).homeMenuItems == [.info])
    }
    #elseif FEEDBACK_MAIL
    @Test func theMenuHoldsSendFeedbackWhereItsReceiverIsConfigured() {
        #expect(model(feedback: true).homeMenuItems == [.info, .sendFeedback])
        #expect(model(guest: false, feedback: true).homeMenuItems == [.info, .sendFeedback])
        #expect(model(feedback: false).homeMenuItems == [.info])
    }
    #else
    @Test func theMenuHoldsBuyMeTokensOnceAVirtualMachineIsSetUp() {
        #expect(model(tips: true).homeMenuItems == [.info, .buyMeTokens])
        #expect(model(guest: false, tips: true).homeMenuItems == [.info])
        #expect(model(tips: false).homeMenuItems == [.info])
    }
    #endif

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

    /// SwiftUI hands UIKit a deferred menu whose items exist only when it opens, so the
    /// toolbar's contents are read from the model above; here the bar holds exactly one menu.
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
        #expect(model.homeMenuItems.first == .info)
    }

    #if FEEDBACK_MAIL && TIP_JAR
    @Test func withNeitherAvailableInfoIsThePlainButtonAgain() async throws {
        let model = model(guest: false, feedback: false, tips: true)
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
