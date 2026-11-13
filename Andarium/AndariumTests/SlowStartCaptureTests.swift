// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// Opt-in captures of the slow-start notice in windows the Simulator's own
/// capture cannot take: an iPad turned to landscape, a narrow window as Split
/// View gives, the Duo's poses (`FoldSessionHostingTests.Pose`), and the
/// largest text size scrolled to the buttons, which a launch cannot do. With
/// `TEST_RUNNER_ANDARIUM_CAPTURE_DIR` naming a folder, each saves a PNG of the
/// root in a window of that shape, named by shape and by the language the run
/// was given (`xcodebuild -testLanguage`); without it the test does nothing.
/// `TEST_RUNNER_ANDARIUM_CAPTURE_SHAPES` names the shapes to take.
@MainActor @Suite(.serialized)
struct SlowStartCaptureTests {
    private static let folder = ProcessInfo.processInfo.environment["ANDARIUM_CAPTURE_DIR"]

    private struct Shape {
        let name: String
        /// The window's own size when nil.
        let size: CGSize?
        let pose: FoldSessionHostingTests.Pose?
        var largestText = false
        var stopping = false

        init(name: String, size: CGSize?, pose: FoldSessionHostingTests.Pose?, largestText: Bool = false, stopping: Bool = false) {
            self.name = name
            self.size = size
            self.pose = pose
            self.largestText = largestText
            self.stopping = stopping
        }
    }

    private static let shapes = [
        Shape(name: "ipad-landscape", size: CGSize(width: 1376, height: 1032), pose: nil),
        Shape(name: "ipad-narrow", size: CGSize(width: 375, height: 1032), pose: nil),
        Shape(name: "duo-upright-book", size: CGSize(width: 951, height: 669), pose: .uprightBook),
        Shape(name: "duo-standing-book", size: CGSize(width: 669, height: 951), pose: .turnedBook),
        Shape(name: "largest-text-buttons", size: nil, pose: nil, largestText: true),
        Shape(name: "largest-text-stopping", size: nil, pose: nil, largestText: true, stopping: true),
    ]

    private func scrollViews(in view: UIView) -> [UIScrollView] {
        let own: [UIScrollView] = (view as? UIScrollView).map { [$0] } ?? []
        return own + view.subviews.flatMap { scrollViews(in: $0) }
    }

    /// The accessibility element the application named by `identifier`; failing
    /// that, the button of the label the application's own words give it.
    private func element(identified identifier: String, labelled label: String, in node: NSObject, depth: Int = 0) -> NSObject? {
        if (node as? UIAccessibilityIdentification)?.accessibilityIdentifier == identifier { return node }
        if node.accessibilityLabel == label, node.accessibilityTraits.contains(.button) { return node }
        guard depth < 64 else { return nil }
        var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = node as? UIView { children += view.subviews }
        for child in children {
            if let found = element(identified: identifier, labelled: label, in: child, depth: depth + 1) { return found }
        }
        return nil
    }

    /// The shapes to take, by name and comma-separated, in
    /// `ANDARIUM_CAPTURE_SHAPES`; all of them without it.
    private static let wanted = ProcessInfo.processInfo.environment["ANDARIUM_CAPTURE_SHAPES"]?
        .split(separator: ",").map(String.init)

    @Test(
        .enabled(if: SlowStartCaptureTests.folder != nil),
        arguments: shapes.map(\.name).filter { SlowStartCaptureTests.wanted?.contains($0) ?? true })
    func theNoticeInAWindowOfThisShape(name: String) async throws {
        let shape = try #require(Self.shapes.first { $0.name == name })
        let directory = URL(fileURLWithPath: try #require(Self.folder), isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let host = HostDevice(
            name: "iPad", processorCount: 8, memory: MemoryPolicy(maximumsMiB: [8192]), freeStorageBytes: 41_000_000_000)
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(
                version: "17", release: "20261031-los24-r4", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages",
                appVersion: "1.0 (9)"),
            capabilities: .device, createRequirementBytes: 1)
        model.launchApplied = true
        model.driver = StubDriver(plan: .normal)
        model.phase = .booting(.normal)
        model.frozenBootStage = 3
        model.frozenElapsed = 197
        model.slowStart.begin(at: model.clock() - 197)
        model.updateSlowStart()
        #if FEEDBACK_MAIL
        var services = SendFeedbackServices()
        services.address = { "feedback@example.com" }
        model.feedbackServices = services
        #endif
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let container = UIViewController()
        container.view.backgroundColor = .black
        let holder = UIView(frame: CGRect(origin: .zero, size: shape.size ?? lease.window.bounds.size))
        let entry = AppEntry(overrides: LaunchOverrides())
        let controller = AndariumHostingController(
            rootView: AnyView(RootView(entry: entry).environment(model)), model: model)
        controller.foldReader.stub = { shape.pose?.reading }
        if shape.largestText { controller.traitOverrides.preferredContentSizeCategory = .accessibilityExtraExtraExtraLarge }
        if shape.stopping {
            model.askToStop()
            model.stopStart()
        }
        lease.window.rootViewController = container
        container.view.addSubview(holder)
        container.addChild(controller)
        holder.addSubview(controller.view)
        controller.view.frame = holder.bounds
        controller.didMove(toParent: container)
        controller.view.layoutIfNeeded()
        try await Task.sleep(for: .milliseconds(1500))
        controller.view.layoutIfNeeded()
        if shape.largestText {
            // The notice is taller than the screen here: scroll until the buttons
            // are on it (Stop a third of the way down, the paragraph's end above
            // it and Send feedback below), as a finger would.
            let label = shape.stopping
                ? NSLocalizedString("Stopping\u{2026}", bundle: Bundle(for: AppModel.self), comment: "")
                : NSLocalizedString("Stop", bundle: Bundle(for: AppModel.self), comment: "")
            var found: NSObject?
            let deadline = ContinuousClock.now + .seconds(5)
            while found == nil, ContinuousClock.now < deadline {
                found = element(identified: "boot-stop", labelled: label, in: controller.view)
                if found == nil { try await Task.sleep(for: .milliseconds(100)) }
            }
            let button = try #require(found, "no Stop button")
            let scroll = try #require(
                scrollViews(in: controller.view).max { $0.contentSize.height < $1.contentSize.height }, "no scroll view")
            let screen = try #require(holder.window?.screen)
            let frame = scroll.convert(button.accessibilityFrame, from: screen.coordinateSpace)
            let top = -scroll.adjustedContentInset.top
            let bottom = max(top, scroll.contentSize.height - scroll.bounds.height + scroll.adjustedContentInset.bottom)
            scroll.contentOffset.y = min(max(frame.minY - scroll.bounds.height * 0.3, top), bottom)
            controller.view.layoutIfNeeded()
            try await Task.sleep(for: .milliseconds(500))
        }
        let image = UIGraphicsImageRenderer(bounds: holder.bounds).image { _ in
            holder.drawHierarchy(in: holder.bounds, afterScreenUpdates: true)
        }
        let language = Locale.current.language.languageCode?.identifier ?? "en"
        let script = Locale.current.language.script?.identifier
        let tag = script.map { "\(language)-\($0)" } ?? language
        try #require(image.pngData()).write(to: directory.appendingPathComponent("\(shape.name)-boot-slow-\(tag).png"))
        model.phase = .home
        controller.view.setNeedsLayout()
        controller.view.layoutIfNeeded()
    }
}
