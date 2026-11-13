// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI
import Testing
import TipKit
import UIKit
@testable import Andarium

/// The open side keys take touches only on the keys. A touch that begins
/// anywhere else reaches the guest's display, which reports it to the session,
/// and the session closes the keys. The tip beside the closed keys goes with
/// them.
@MainActor
@Suite(.serialized)
struct SideKeysTouchTests {
    fileprivate func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    fileprivate func descendants(of view: UIView) -> [UIView] {
        view.subviews + view.subviews.flatMap { descendants(of: $0) }
    }

    // MARK: The session's rule

    @Test func closingTheKeysChangesOnlyAnOpenSet() {
        let session = SessionState()
        #expect(!session.sideKeysOpen)
        session.closeSideKeys()
        #expect(!session.sideKeysOpen)
        session.sideKeysOpen = true
        session.closeSideKeys()
        #expect(!session.sideKeysOpen)
        // Every run starts with the keys closed.
        session.sideKeysOpen = true
        #expect(!SessionState().sideKeysOpen)
    }

    // MARK: The display's report

    @Test func aTouchThatBeginsOnTheDisplayIsReportedOnce() {
        let view = GuestMetalView(frame: CGRect(x: 0, y: 0, width: 200, height: 100), device: nil)
        var reports = 0
        view.touchDidBegin = { reports += 1 }
        view.touchesBegan([UITouch()], with: nil)
        #expect(reports == 1)
        // Moving and lifting are not beginnings.
        view.touchesMoved([UITouch()], with: nil)
        view.touchesEnded([UITouch()], with: nil)
        view.touchesCancelled([UITouch()], with: nil)
        #expect(reports == 1)
    }

    // MARK: The keys over the real display

    /// The production root with a run on screen: the guest's display under the
    /// session's overlay, and the points beside the keys a test touches.
    @MainActor
    private final class RunningSession {
        let model: AppModel
        let lease: HostWindow.Lease
        let root: UIView
        let guest: GuestMetalView
        /// The screen of a given size, which the root fills.
        let holder: UIView?
        /// The safe area the overlay lays out in, from its top-left corner.
        let width: CGFloat
        let height: CGFloat
        /// The height of the keys' centre, from `SideKeys`.
        let centreY: CGFloat
        let groupWidth = KeyMetrics.width * 2 + KeyMetrics.columnGap
        private let safe: UIEdgeInsets

        /// Holds the host window until `end()`. The session runs in `mode` on a
        /// runtime with `capabilities`, and the window reads its text at
        /// `contentSize` when that is given. With a `size` the session is in a
        /// screen of that size at the window's top-left corner, as a smaller
        /// phone has it: the keys sit at its right edge, where the popover
        /// finds the room a smaller phone leaves it.
        init(
            _ test: SideKeysTouchTests, capabilities: Capabilities = .device, mode: StartMode = .normal,
            contentSize: UIContentSizeCategory? = nil, size: CGSize? = nil
        ) async throws {
            let entry = AppEntry.fromArguments()
            let model = entry.makeModel()
            model.launchApplied = true
            model.capabilities = capabilities
            model.driver = StubDriver(plan: nil)
            let lease = try await HostWindow.take()
            if let contentSize { lease.window.traitOverrides.preferredContentSizeCategory = contentSize }
            let controller = AndariumHostingController(rootView: RootView(entry: entry).environment(model), model: model)
            // These are the phone's edge keys. A display that folds lays the
            // session out by its pose instead (`FoldSessionHostingTests`), so
            // here the scene reads as one that does not fold.
            controller.foldReader.stub = { nil }
            if let size {
                let container = UIViewController()
                let holder = UIView(frame: CGRect(origin: .zero, size: size))
                lease.window.rootViewController = container
                container.view.addSubview(holder)
                container.addChild(controller)
                holder.addSubview(controller.view)
                controller.view.frame = holder.bounds
                controller.didMove(toParent: container)
                self.holder = holder
            } else {
                lease.window.rootViewController = controller
                holder = nil
            }
            model.phase = .running(mode)
            let root = controller.view!
            var display: GuestMetalView?
            let found = await test.eventually {
                root.layoutIfNeeded()
                display = test.descendants(of: root).compactMap { $0 as? GuestMetalView }.first
                return display != nil && display!.bounds.width > 0
            }
            guard found, let display else {
                lease.window.traitOverrides.remove(UITraitPreferredContentSizeCategory.self)
                lease.give()
                throw Failure.noDisplay
            }
            // The session hides the status bar. Where the bar has an inset of
            // its own (the iPhone Duo's inner display, an iPad) the safe area
            // changes with it, so it is read once the bar is hidden. A
            // session in a screen of its own size is not asked about the bar.
            if size == nil {
                _ = await test.eventually {
                    root.layoutIfNeeded()
                    return lease.scene.statusBarManager?.isStatusBarHidden ?? true
                }
            }
            root.layoutIfNeeded()
            let safe = root.safeAreaInsets
            let height = root.bounds.height - safe.top - safe.bottom
            self.model = model
            self.lease = lease
            self.root = root
            guest = display
            self.safe = safe
            width = root.bounds.width - safe.left - safe.right
            self.height = height
            centreY = min(max(118, height * 0.32), max(118, height - 118))
        }

        enum Failure: Error { case noDisplay }

        func end() {
            lease.window.traitOverrides.remove(UITraitPreferredContentSizeCategory.self)
            lease.give()
        }

        /// A point of the overlay's safe area in the window.
        func window(_ x: CGFloat, _ y: CGFloat) -> CGPoint {
            root.convert(CGPoint(x: safe.left + x, y: safe.top + y), to: lease.window)
        }

        func reachesGuest(_ point: CGPoint) -> Bool {
            guard let view = lease.window.hitTest(point, with: nil) else { return false }
            return view === guest || view.isDescendant(of: guest)
        }

        /// Beside the keys: across the screen and on the keys' own row.
        var beside: [CGPoint] {
            [
                window(width * 0.3, height * 0.5),
                window(width * 0.5, centreY),
                window(width - KeyMetrics.edgeGap - groupWidth - 40, centreY),
                window(width - 20, height * 0.9),
                window(width * 0.3, height * 0.1),
            ]
        }

        /// What the root presents over the session: the tip's popover.
        var presented: UIViewController? { lease.window.rootViewController?.presentedViewController }

        /// The tip's card while its popover is up, with the popover's margin.
        var tipCard: CGRect? {
            guard let view = presented?.viewIfLoaded else { return nil }
            return view.convert(view.bounds, to: lease.window).insetBy(dx: -12, dy: -12)
        }

        func onTip(_ point: CGPoint) -> Bool { tipCard?.contains(point) ?? false }

        /// A touch beside the shown tip reaches the guest. On a Mac an iPad
        /// app's popover keeps its dimming view, which takes every touch
        /// while the tip shows, so this holds on iOS alone.
        var besideTheTipReachesGuest: Bool { presented == nil || !ProcessInfo.processInfo.isiOSAppOnMac }
    }

    /// A point beside the open keys must reach the display (UIKit hands the
    /// touch to the display's view), and a point on a key must stay with the
    /// keys. Beside the closed keys only the tip, while it shows, takes a
    /// touch: on iOS only its card.
    @Test func theOpenKeysTakeNoTouchesBesideThem() async throws {
        let run = try await RunningSession(self)
        defer { run.end() }
        let model = run.model
        let width = run.width
        let far = run.beside[0]

        // The open group: Power's column at the edge and the guest keys beside
        // it, centred on the edge's fraction of the height, from `SideKeys`.
        let powerX = width - KeyMetrics.edgeGap - KeyMetrics.width / 2
        let settingsX = width - KeyMetrics.edgeGap - run.groupWidth + KeyMetrics.width / 2
        let groupTop = run.centreY - (KeyMetrics.keyHeight + KeyMetrics.rowGap + 2 * (KeyMetrics.keyHeight - 0.75) + 1.5) / 2
        let powerY = groupTop + KeyMetrics.keyHeight / 2
        let homeY = groupTop + KeyMetrics.keyHeight + KeyMetrics.rowGap + KeyMetrics.keyHeight * 1.5

        // Closed, as the baseline, beside the keys reaches the guest once the
        // tip's popover has finished appearing.
        model.session.sideKeysOpen = false
        try await Task.sleep(for: .milliseconds(600))
        run.root.layoutIfNeeded()
        if run.besideTheTipReachesGuest {
            #expect(await eventually { run.reachesGuest(far) }, "closed: \(far)")
            for point in run.beside where !run.onTip(point) { #expect(run.reachesGuest(point), "closed: \(point)") }
        }

        model.session.sideKeysOpen = true
        #expect(await eventually { run.presented == nil }, "the tip stayed beside the open keys")
        try await Task.sleep(for: .milliseconds(800))
        run.root.layoutIfNeeded()
        for point in run.beside { #expect(run.reachesGuest(point), "open: \(point)") }
        // On a key the touch stays with the keys.
        for (name, point) in [("Power", run.window(powerX, powerY)), ("Settings", run.window(settingsX, powerY)),
                              ("Home", run.window(settingsX, homeY)), ("Volume", run.window(powerX, homeY))] {
            #expect(!run.reachesGuest(point), "\(name) at \(point) reached the guest")
        }

        // A touch beginning on the display closes the keys, as the display's
        // report does.
        run.guest.touchesBegan([UITouch()], with: nil)
        #expect(await eventually { !model.session.sideKeysOpen })
        try await Task.sleep(for: .milliseconds(600))
        run.root.layoutIfNeeded()
        if run.besideTheTipReachesGuest {
            #expect(await eventually { run.reachesGuest(far) }, "closed again: \(far)")
            for point in run.beside where !run.onTip(point) { #expect(run.reachesGuest(point), "closed again: \(point)") }
        }
    }

    /// The tip beside the closed keys takes touches on its card (on iOS only
    /// there), and goes with the closed keys: when they open, nothing of it is
    /// left beside the open keys, and when the session ends, nothing of it is
    /// left over Home.
    /// The test bundle shows the tip in every session (`AndariumTestBundle`).
    @Test func theTipGoesWithTheClosedKeys() async throws {
        let run = try await RunningSession(self)
        defer { run.end() }
        let model = run.model

        try #require(await eventually { run.presented != nil }, "the tip did not show")
        let card = try #require(run.tipCard)
        let far = run.beside[0]
        let onCard = CGPoint(x: card.midX, y: card.midY)
        #expect(!run.reachesGuest(onCard), "a touch on the tip at \(onCard) reached the guest")
        // Once the popover is up, a touch beside its card reaches the guest.
        if run.besideTheTipReachesGuest {
            #expect(await eventually { run.reachesGuest(far) }, "the tip's popover took a touch beside it")
            for point in [far, run.window(run.width - 20, run.height * 0.9), run.window(run.width * 0.3, run.height * 0.1)] {
                #expect(run.reachesGuest(point), "tip shown: \(point)")
            }
        }

        model.session.sideKeysOpen = true
        #expect(await eventually { run.presented == nil }, "the tip stayed over the open keys")
        try await Task.sleep(for: .milliseconds(800))
        run.root.layoutIfNeeded()
        for point in run.beside { #expect(run.reachesGuest(point), "open: \(point)") }

        model.session.sideKeysOpen = false
        #expect(await eventually { run.presented != nil }, "the tip did not come back with the closed keys")
        model.phase = .home
        #expect(await eventually { run.presented == nil }, "the tip stayed after the session ended")
    }

    // MARK: The tip's line for Install APK

    private static let installLine = "To install an app, open Settings here and choose Install APK."

    private var withoutApkInstall: Capabilities {
        var capabilities = Capabilities.device
        capabilities.apkInstall = false
        return capabilities
    }

    /// The tip's words: its keys line always, the line for Install APK only
    /// when the session can offer the row. It is one tip either way, so a
    /// person who closed it is not shown it again for the added line.
    @Test func theTipsWordsAddTheInstallLineOnlyWhenAskedAndKeepTheKeysLine() {
        let keys = SideKeysTip.words(offersInstall: false)
        #expect(keys == "Tap for Back, Home and Settings. Hold Power for the power menu. Drag to move.")
        #expect(SideKeysTip.words(offersInstall: true) == keys + "\n" + Self.installLine)
        // The tip a person closed or saw three times stays closed: its
        // identity is the shipped one, the type's name, which TipKit takes
        // when a tip names none.
        #expect(SideKeysTip(offersInstall: true).id == "SideKeysTip")
        #expect(SideKeysTip(offersInstall: false).id == "SideKeysTip")
    }

    /// The tip points at Install APK exactly where Settings has the row: a
    /// normal session of a runtime that installs apps. Recovery runs no adb,
    /// and a runtime without the install has no row.
    @Test(arguments: [(true, StartMode.normal), (false, StartMode.normal), (true, StartMode.recovery)])
    func theTipPointsAtInstallApkOnlyWhereSettingsHasTheRow(_ installs: Bool, _ mode: StartMode) async throws {
        let run = try await RunningSession(self, capabilities: installs ? .device : withoutApkInstall, mode: mode)
        defer { run.end() }
        try #require(await eventually { run.presented != nil }, "the tip did not show")
        try await Task.sleep(for: .milliseconds(400))
        let view = try #require(run.presented?.viewIfLoaded)
        let labels = Self.elements(in: view).map { $0.label }
        let hint = labels.contains { $0.contains("Install APK") }
        #expect(hint == (installs && mode == .normal), "installs \(installs), \(mode): the tip reads \(labels)")
        #expect(labels.contains { $0.contains("Hold Power for the power menu") }, "the keys line stays: \(labels)")
        // The tip's condition is the row's: Settings of this session has the
        // row exactly when the tip says so.
        #expect(run.model.capabilities.offersApkInstall(in: mode) == hint)
    }

    /// The tip on the smallest phone (375 by 667 points) at the largest
    /// accessibility text size: a card inside the screen, taller than at the
    /// ordinary size (the size reached the popover), that shows its top and
    /// scrolls what does not fit, with and without the line for Install APK.
    @Test(arguments: [true, false])
    func theTipFitsTheSmallestPhoneAtTheLargestTextSize(_ installs: Bool) async throws {
        let phone = CGSize(width: 375, height: 667)
        let capabilities = installs ? Capabilities.device : withoutApkInstall
        let ordinary = try await shownTip(capabilities, contentSize: .large, size: phone)
        let largest = try await shownTip(capabilities, contentSize: .accessibilityExtraExtraExtraLarge, size: phone)
        #expect(
            largest.card.height > ordinary.card.height + 20,
            "the largest size did not reach the tip: \(ordinary.card) at the ordinary size, \(largest.card) at the largest")
        #expect(largest.screen.contains(largest.card), "the card \(largest.card) is not inside the screen \(largest.screen)")
        #expect(largest.card.height <= SideKeysTipCard.maxHeight(in: largest.sessionHeight) + 1, "\(largest.card)")
        largest.expectNothingCutOff()
        #expect(largest.text.contains { $0.label.contains("Install APK") } == installs)
    }

    /// What the popover of the tip shows, in screen coordinates.
    struct ShownTip {
        let card: CGRect
        let screen: CGRect
        let sessionHeight: CGFloat
        /// The card's elements but TipKit's close button, whose touch area
        /// reaches past the card's top edge by design, and the scroll bar.
        let text: [(label: String, frame: CGRect)]
        let close: CGRect?
        let scrollContent: CGFloat
        let preferred: CGSize

        /// Every element is inside the card, and the close button.
        func expectTextInsideCard(sourceLocation: SourceLocation = #_sourceLocation) {
            #expect(!text.isEmpty, "no text found in the tip", sourceLocation: sourceLocation)
            for element in text {
                #expect(
                    card.insetBy(dx: -1, dy: -1).contains(element.frame),
                    "\(element.label) at \(element.frame) is outside the card \(card)", sourceLocation: sourceLocation)
            }
            expectCloseInsideCard(sourceLocation: sourceLocation)
        }

        /// A card that scrolls shows its top: nothing starts above it, the
        /// close button shows, and what does not end inside it is what scrolls.
        func expectNothingCutOff(sourceLocation: SourceLocation = #_sourceLocation) {
            #expect(!text.isEmpty, "no text found in the tip", sourceLocation: sourceLocation)
            for element in text {
                #expect(
                    element.frame.minY >= card.minY - 1 && element.frame.minX >= card.minX - 1 && element.frame.maxX <= card.maxX + 1,
                    "\(element.label) at \(element.frame) starts outside the card \(card)", sourceLocation: sourceLocation)
                #expect(
                    element.frame.maxY <= card.maxY + 1 || scrollContent > card.height + 1,
                    "\(element.label) at \(element.frame) runs past the card \(card) that does not scroll",
                    sourceLocation: sourceLocation)
            }
            expectCloseInsideCard(sourceLocation: sourceLocation)
        }

        private func expectCloseInsideCard(sourceLocation: SourceLocation) {
            if let close {
                #expect(
                    card.contains(CGPoint(x: close.midX, y: close.midY)), "the close button \(close) is outside the card \(card)",
                    sourceLocation: sourceLocation)
            } else {
                Issue.record("the tip has no close button", sourceLocation: sourceLocation)
            }
        }
    }

    /// The production session in a screen of `size`, its tip up.
    fileprivate func shownTip(_ capabilities: Capabilities, contentSize: UIContentSizeCategory, size: CGSize) async throws -> ShownTip {
        let run = try await RunningSession(self, capabilities: capabilities, contentSize: contentSize, size: size)
        defer { run.end() }
        try #require(await eventually { run.presented != nil }, "the tip did not show")
        try await Task.sleep(for: .milliseconds(700))
        let popover = try #require(run.presented)
        let holder = try #require(run.holder)
        return Self.shown(
            popover, screen: UIAccessibility.convertToScreenCoordinates(holder.bounds, in: holder), sessionHeight: run.height,
            test: self)
    }

    static func shown(_ popover: UIViewController, screen: CGRect, sessionHeight: CGFloat, test: SideKeysTouchTests) -> ShownTip {
        let view = popover.view!
        let found = elements(in: view)
        let scroll = test.descendants(of: view).compactMap { $0 as? UIScrollView }.first
        return ShownTip(
            card: UIAccessibility.convertToScreenCoordinates(view.bounds, in: view), screen: screen, sessionHeight: sessionHeight,
            text: found.filter { !$0.button && !$0.label.hasPrefix("Vertical scroll bar") }.map { ($0.label, $0.frame) },
            close: found.first { $0.button }?.frame,
            scrollContent: scroll?.contentSize.height ?? 0, preferred: popover.preferredContentSize)
    }

    // MARK: The tip's card at the default size

    /// The tip alone: the side keys' overlay on a screen `size` big (the
    /// smallest phone's width, or the largest's), its tip in `language`.
    @MainActor
    private final class TipScreen {
        let lease: HostWindow.Lease
        let holder: UIView
        let tip: SideKeysTip
        private let container = UIViewController()
        private let host: UIHostingController<AnyView>

        init(size: CGSize, language: String, contentSize: UIContentSizeCategory = .large, leading: Bool = false) async throws {
            let locale = Locale(identifier: language)
            tip = SideKeysTip(offersInstall: true, locale: locale)
            let nothing = {}
            let overlay = SideKeysOverlay(
                mode: .normal, lit: true, open: .constant(false), leading: leading, tip: tip,
                actions: SideKeyActions(
                    power: nothing, powerHold: nothing, volumeUp: nothing, volumeDown: nothing, back: nothing, home: nothing,
                    settings: nothing))
            lease = try await HostWindow.take()
            lease.window.traitOverrides.preferredContentSizeCategory = contentSize
            host = UIHostingController(rootView: AnyView(overlay.environment(\.locale, locale)))
            host.safeAreaRegions = []
            holder = UIView(frame: CGRect(origin: .zero, size: size))
            lease.window.rootViewController = container
            container.view.addSubview(holder)
            container.addChild(host)
            holder.addSubview(host.view)
            host.view.frame = holder.bounds
            host.didMove(toParent: container)
        }

        var popover: UIViewController? { container.presentedViewController }

        func end() {
            lease.window.traitOverrides.remove(UITraitPreferredContentSizeCategory.self)
            lease.give()
        }
    }

    fileprivate func shownTip(language: String, width: CGFloat, contentSize: UIContentSizeCategory = .large, leading: Bool = false)
        async throws -> (ShownTip, SideKeysTip)
    {
        let size = CGSize(width: width, height: 667)
        let screen = try await TipScreen(size: size, language: language, contentSize: contentSize, leading: leading)
        defer { screen.end() }
        try #require(await eventually { screen.popover != nil }, "\(language): the tip did not show")
        try await Task.sleep(for: .milliseconds(700))
        let popover = try #require(screen.popover)
        let shown = Self.shown(
            popover, screen: UIAccessibility.convertToScreenCoordinates(screen.holder.bounds, in: screen.holder),
            sessionHeight: size.height, test: self)
        return (shown, screen.tip)
    }

    /// At the default size the tip fits its card with no scrolling in every
    /// language, on the smallest phone's width and the largest's: the title,
    /// the close button and the last line of the text all show.
    @Test(arguments: ["en", "zh-Hans", "zh-Hant", "ja"], [375.0, 402.0])
    func theTipFitsItsCardAtTheDefaultTextSizeInEveryLanguage(_ language: String, _ width: Double) async throws {
        let (shown, tip) = try await shownTip(language: language, width: width)
        func plain(_ text: String) -> String { text.filter { $0 != "\u{2060}" } }
        let lines = tip.words.split(separator: "\n").map(String.init)
        let context = "\(language) at \(width): card \(shown.card), asked for \(shown.preferred), text \(shown.scrollContent) high"
        #expect(shown.text.count >= 2, "the title and the text: \(context)")
        for line in lines {
            #expect(shown.text.contains { plain($0.label).contains(plain(line)) }, "no element reads \(line): \(context)")
        }
        shown.expectTextInsideCard()
        #expect(shown.scrollContent <= shown.card.height + 1, "the card scrolls: \(context)")
        // UIKit gave the card the width it was measured at.
        #expect(abs(shown.card.width - SideKeysTipCard.width(in: width)) < 1, "UIKit narrowed the popover: \(context)")
    }

    /// With the keys on the left edge the popover opens to their right and
    /// the card still fits what the popover keeps for it.
    @Test(arguments: ["en", "ja"])
    func theTipFitsItsCardBesideKeysOnTheLeftEdge(_ language: String) async throws {
        let (shown, _) = try await shownTip(language: language, width: 375, leading: true)
        let context = "\(language): card \(shown.card), text \(shown.scrollContent) high"
        shown.expectTextInsideCard()
        #expect(shown.scrollContent <= shown.card.height + 1, "the card scrolls: \(context)")
        #expect(abs(shown.card.width - SideKeysTipCard.width(in: 375)) < 1, "UIKit narrowed the popover: \(context)")
        #expect(shown.card.minX > 0 && shown.card.maxX < 375, "the card \(shown.card) leaves the screen")
    }

    /// The card hugs its text, up to the third accessibility text size (the
    /// larger sizes read as that one, so the tip stays a card), and takes no
    /// more than the height it is given: what does not fit scrolls inside it,
    /// so on the smallest iPhone, and in a fold's half, nothing is cut off.
    @Test func theTipCardHugsItsTextAndScrollsBeyondItsMaxHeight() async throws {
        let lease = try await HostWindow.take()
        defer {
            lease.window.traitOverrides.remove(UITraitPreferredContentSizeCategory.self)
            lease.give()
        }
        func measure(_ size: UIContentSizeCategory, maxHeight: CGFloat) async throws -> (height: CGFloat, scrolls: Bool) {
            lease.window.traitOverrides.preferredContentSizeCategory = size
            let host = UIHostingController(rootView: SideKeysTipCard(tip: SideKeysTip(offersInstall: true), maxHeight: maxHeight, width: 335))
            // The window's safe area would be added to the card's height.
            host.safeAreaRegions = []
            lease.window.rootViewController = host
            try await Task.sleep(for: .milliseconds(500))
            host.view.setNeedsLayout()
            host.view.layoutIfNeeded()
            let fit = host.sizeThatFits(in: CGSize(width: 335, height: 4000))
            let scroll = descendants(of: host.view).compactMap { $0 as? UIScrollView }.first
            return (fit.height, (scroll?.contentSize.height ?? 0) > fit.height + 1)
        }
        let plain = try await measure(.large, maxHeight: 4000)
        let third = try await measure(.accessibilityExtraLarge, maxHeight: 4000)
        let fifth = try await measure(.accessibilityExtraExtraExtraLarge, maxHeight: 4000)
        #expect(plain.height > 100 && !plain.scrolls, "the card at the ordinary size is \(plain)")
        #expect(third.height > plain.height, "larger text makes a taller card: \(plain) \(third)")
        #expect(abs(fifth.height - third.height) < 1, "the fifth size reads as the third: \(third) \(fifth)")
        #expect(!third.scrolls && !fifth.scrolls, "with room the card does not scroll: \(third) \(fifth)")
        let capped = try await measure(.accessibilityExtraExtraExtraLarge, maxHeight: 200)
        #expect(capped.height <= 200.5 && capped.scrolls, "short of room the card scrolls inside 200 points: \(capped)")
        #expect(SideKeysTipCard.maxHeight(in: 667) < 667 && SideKeysTipCard.maxHeight(in: 100) >= 160)
        // The width leaves the room a popover keeps beside the keys (UIKit left
        // 361 points of 402 and 334 of 375), and is the widest where there is
        // more.
        #expect(SideKeysTipCard.width(in: 402) < 361 && SideKeysTipCard.width(in: 375) < 375 - 41)
        #expect(SideKeysTipCard.width(in: 834) == SideKeysTipCard.widest && SideKeysTipCard.width(in: 100) >= 240)
    }

    /// Every accessibility element of a view tree with its label, screen
    /// frame and whether it is a button, in the order UIKit walks them.
    static func elements(in root: UIView) -> [(label: String, frame: CGRect, button: Bool)] {
        var found: [(label: String, frame: CGRect, button: Bool)] = []
        var seen = Set<ObjectIdentifier>()
        func walk(_ node: NSObject, depth: Int) {
            guard depth < 64, seen.insert(ObjectIdentifier(node)).inserted else { return }
            if let label = node.accessibilityLabel, !label.isEmpty {
                found.append((label, node.accessibilityFrame, node.accessibilityTraits.contains(.button)))
            }
            var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
            if let view = node as? UIView { children += view.subviews }
            for child in children { walk(child, depth: depth + 1) }
        }
        walk(root, depth: 0)
        return found
    }
}
