// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import AndariumCore
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// Home's one toolbar menu: Info, and Send feedback where it is compiled in and its
/// receiver is configured. A build without Send feedback has no menu and is not here
/// (`HomePlainInfoTests`).
@MainActor
@Suite(.serialized)
struct HomeMenuTests {
    private func model(guest: Bool = true, feedback: Bool = true) -> AppModel {
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
        model.feedbackServices.address = { feedback ? "feedback@example.com" : nil }
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

    /// Send feedback is there exactly where its receiver is configured, on Home and on Welcome,
    /// where no virtual machine is set up yet.
    @Test func sendFeedbackIsThereWhereItsReceiverIsConfigured() {
        for guest in [true, false] {
            #expect(model(guest: guest, feedback: true).homeMenuItems == [.info, .sendFeedback], "guest \(guest)")
            #expect(model(guest: guest, feedback: false).homeMenuItems == [.info], "guest \(guest)")
        }
    }

    @Test func eachItemDoesWhatItsEntranceDid() {
        let model = model()
        model.choose(.info)
        #expect(model.sheet == .info)
        model.sheet = nil
        model.choose(.sendFeedback)
        #expect(model.feedbackSheetOpen)
        #expect(model.sheet == nil, "Send feedback is its own sheet, not a page of Info")
    }

    /// The words of the menu are in every language the application ships: Info and Send feedback.
    @Test(arguments: ["ja", "zh-Hans", "zh-Hant"])
    func theMenuSaysItsItemsInEachLanguage(language: String) {
        func text(_ key: String.LocalizationValue, table: String? = nil) -> String {
            String(localized: LocalizedStringResource(key, table: table, locale: Locale(identifier: language), bundle: .main))
        }
        #expect(text("Info") != "Info" && !text("Info").isEmpty, "\(language) says Info in English")
        let sendFeedback = text("Send feedback", table: "Feedback")
        #expect(sendFeedback != "Send feedback" && !sendFeedback.isEmpty, "\(language) says Send feedback in English")
        #expect(!sendFeedback.contains("\u{2026}"), "the menu item carries no ellipsis")
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

    /// The bar holds one menu button in place of the Info button, which keeps Info's own
    /// accessibility label, on Home and on Welcome. (SwiftUI builds a toolbar menu's items when it
    /// opens, so what it holds is `homeMenuItems`, which `HomeToolbarMenu` lays out.)
    @Test(arguments: [true, false])
    func theToolbarHoldsOneMenuInPlaceOfTheInfoButton(onHome: Bool) async throws {
        let model = model(guest: onHome)
        let (controller, lease) = try await host(model)
        defer { lease.give() }
        let shown = await eventually {
            ToolbarProbe.barItems(under: controller).contains { $0.menu != nil }
        }
        try #require(shown, "the toolbar shows a menu; it holds: \(ToolbarProbe.describe(under: controller))")
        try await Task.sleep(for: .milliseconds(500))
        let items = ToolbarProbe.barItems(under: controller)
        #expect(items.filter { $0.menu != nil }.count == 1, "one menu")
        #expect(items.count == 1, "Info is in the menu, not beside it")
        #expect(items.first?.title == "Info" || items.first?.accessibilityLabel == "Info", "it reads Info: \(ToolbarProbe.describe(under: controller))")
        #expect(model.homeMenuItems == [.info, .sendFeedback], "the menu holds Info, then Send feedback")
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

    /// Opened as a tap opens it, the menu offers Info and Send feedback as buttons, which is what
    /// VoiceOver finds in it, in the order the menu holds them.
    @Test func theOpenMenuOffersEachItemToVoiceOver() async throws {
        let model = model()
        let (controller, lease) = try await host(model)
        defer { lease.give() }
        let shown = await eventually { ToolbarProbe.barItems(under: controller).contains { $0.menu != nil } }
        try #require(shown, "the toolbar shows a menu")
        try await Task.sleep(for: .milliseconds(600))
        var pending: [UIView] = [lease.window]
        var interaction: UIContextMenuInteraction?
        while let view = pending.popLast(), interaction == nil {
            if let control = view as? UIControl,
               let menu = control.interactions.compactMap({ $0 as? UIContextMenuInteraction }).first {
                control.performPrimaryAction()
                interaction = menu
            }
            pending += view.subviews
        }
        try #require(interaction != nil, "a control of the bar carries the menu")
        defer { interaction?.dismissMenu() }
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        let found = await eventually {
            let labels = scenes.flatMap(\.windows).flatMap { ToolbarProbe.elements(in: $0, above: 4000) }
                .filter { $0.traits.contains(.button) }.map(\.label)
            return labels.contains("Send feedback")
        }
        let labels = scenes.flatMap(\.windows).flatMap { ToolbarProbe.elements(in: $0, above: 4000) }
            .filter { $0.traits.contains(.button) }.map(\.label)
        #expect(found, "the open menu: \(labels)")
        #expect(labels.contains("Info") && labels.contains("Send feedback"), "\(labels)")
        if let info = labels.firstIndex(of: "Send feedback") { #expect(info > 0) }
    }

    /// With no receiver there is nothing for a menu to hold beside Info: the plain button again.
    @Test func withoutAReceiverInfoIsThePlainButtonAgain() async throws {
        let model = model(feedback: false)
        let (controller, lease) = try await host(model)
        defer { lease.give() }
        try await Task.sleep(for: .seconds(1))
        #expect(model.homeMenuItems == [.info])
        #expect(!ToolbarProbe.barItems(under: controller).isEmpty, "the toolbar still shows Info")
        #expect(ToolbarProbe.barItems(under: controller).allSatisfy { $0.menu == nil }, "no menu holds Info alone")
    }

    @Test func sendFeedbackOpensOverHomeFromTheMenu() async throws {
        let model = model()
        let (controller, lease) = try await host(model)
        defer { lease.give() }
        model.choose(.sendFeedback)
        #expect(await eventually { controller.presentedViewController != nil }, "the sheet is over Home")
        model.feedbackSheetOpen = false
        #expect(await eventually { controller.presentedViewController == nil })
    }

    // MARK: Info

    private func nodes(in node: NSObject, depth: Int = 0) -> [NSObject] {
        guard depth < 64 else { return [] }
        var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = node as? UIView { children += view.subviews }
        return [node] + children.flatMap { nodes(in: $0, depth: depth + 1) }
    }

    /// Info keeps its other rows and no longer holds Send feedback, though the receiver is configured.
    @Test func infoHoldsNoSendFeedbackSection() async throws {
        let model = model()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = UIHostingController(
            rootView: InfoSheet(page: nil).environment(model).frame(width: 440, height: 1100))
        lease.window.rootViewController = controller
        let drawn = await eventually {
            controller.view.setNeedsLayout()
            controller.view.layoutIfNeeded()
            return nodes(in: controller.view).contains { $0.accessibilityLabel == "Project on GitHub" }
        }
        try #require(drawn, "Info was not drawn; is ApplicationAccessibilityEnabled on?")
        let labels = nodes(in: controller.view).compactMap(\.accessibilityLabel)
        let identifiers = nodes(in: controller.view).compactMap { ($0 as? UIAccessibilityIdentification)?.accessibilityIdentifier }
        #expect(!labels.contains { $0.contains("Send feedback") }, "\(labels)")
        #expect(!identifiers.contains("info-send-feedback"), "\(identifiers)")
        #expect(labels.contains("Acknowledgements"), "Info still lists Acknowledgements: \(labels)")
    }
}

#endif
