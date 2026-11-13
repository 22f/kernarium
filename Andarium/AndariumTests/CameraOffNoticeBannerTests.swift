// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The notice over the running guest that Camera is off, drawn in a window as
/// the session draws it. SwiftUI publishes its accessibility elements only
/// while application accessibility is on (the Simulator preference
/// `ApplicationAccessibilityEnabled`, as `Kernarium/scripts/
/// run-ios-simulator-tests.sh` sets it); a run without it finds none and the
/// tests fail, for they check nothing then.
@MainActor
struct CameraOffNoticeBannerTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    /// A running guest with Camera off on a device that has a camera, whose guest
    /// has just opened one.
    private func model() -> AppModel {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1")
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device,
            createRequirementBytes: 1)
        model.cameraAccess = .notDetermined
        model.phase = .running(.normal)
        model.cameraCaptureChanged(.blockedInUse)
        return model
    }

    private func find(in node: NSObject, depth: Int = 0, _ match: (NSObject) -> Bool) -> NSObject? {
        if match(node) { return node }
        guard depth < 64 else { return nil }
        var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = node as? UIView { children += view.subviews }
        for child in children {
            if let found = find(in: child, depth: depth + 1, match) { return found }
        }
        return nil
    }

    private struct Parts {
        var title: NSObject
        var detail: NSObject
        var dismiss: NSObject
    }

    /// The notice's words and its button in `controller`, waiting for SwiftUI to publish them.
    private func parts(in controller: UIViewController) async throws -> Parts? {
        let deadline = ContinuousClock.now + .seconds(5)
        while ContinuousClock.now < deadline {
            controller.view.layoutIfNeeded()
            let title = find(in: controller.view) { $0.accessibilityLabel == CameraWords.offTitle }
            let detail = find(in: controller.view) { $0.accessibilityLabel == CameraWords.offDetail }
            let dismiss = find(in: controller.view) {
                $0.accessibilityLabel == NetworkWords.dismissLabel && $0.accessibilityTraits.contains(.button)
            }
            if let title, let detail, let dismiss { return Parts(title: title, detail: detail, dismiss: dismiss) }
            try await Task.sleep(for: .milliseconds(50))
        }
        return nil
    }

    private func hosted(
        _ model: AppModel, width: CGFloat, size: DynamicTypeSize, in lease: HostWindow.Lease
    ) -> UIViewController {
        let controller = UIHostingController(
            rootView: SessionNoticesOverlay().environment(model).dynamicTypeSize(size).frame(width: width))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        return controller
    }

    @Test func theNoticeSaysTheCameraIsOffAndHowToTurnItOnAndItsDismissReachesTheModel() async throws {
        let model = model()
        #expect(model.cameraNoticeKind == .cameraOff)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hosted(model, width: 390, size: .large, in: lease)
        guard let found = try await parts(in: controller) else {
            Issue.record("The notice was not found: is application accessibility on in this Simulator?")
            return
        }
        #expect(found.title.accessibilityLabel == "Camera is off")
        #expect(found.detail.accessibilityLabel == "Turn on Camera in Capabilities, then start the virtual machine again.")
        #expect(found.dismiss.accessibilityActivate())
        #expect(model.cameraNoticeKind == nil, "dismissing it puts it away")
        #expect(model.cameraCapture == .blockedInUse, "the guest still has the camera open")
        // A new open raises it again, and the guest closing the camera takes it away.
        model.cameraCaptureChanged(.idle)
        model.cameraCaptureChanged(.blockedInUse)
        #expect(model.cameraNoticeKind == .cameraOff)
        model.cameraCaptureChanged(.idle)
        #expect(model.cameraNoticeKind == nil)
    }

    @Test func theNoticeShowsNothingForADeviceWithoutACamera() async throws {
        let model = model()
        model.cameraAccess = .unavailable
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hosted(model, width: 390, size: .large, in: lease)
        try await Task.sleep(for: .milliseconds(400))
        controller.view.layoutIfNeeded()
        let title = find(in: controller.view) { $0.accessibilityLabel == CameraWords.offTitle }
        #expect(title == nil)
    }

    /// At the largest accessibility size the words keep the width and the
    /// button goes under them, as the camera notice's does, and nothing leaves
    /// the screen's width.
    @Test(arguments: [DynamicTypeSize.large, .accessibility5])
    func theNoticeFitsANarrowScreenAtEveryTextSize(size: DynamicTypeSize) async throws {
        let model = model()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let width: CGFloat = 320
        let controller = hosted(model, width: width, size: size, in: lease)
        guard let found = try await parts(in: controller) else {
            Issue.record("The notice was not found: is application accessibility on in this Simulator?")
            return
        }
        let title = found.title.accessibilityFrame
        let detail = found.detail.accessibilityFrame
        let dismiss = found.dismiss.accessibilityFrame
        for frame in [title, detail, dismiss] {
            #expect(frame.width > 0 && frame.width <= width, "inside the width: \(frame)")
        }
        #expect(detail.minY >= title.maxY - 1, "the detail is under the title")
        if size.isAccessibilitySize {
            #expect(dismiss.minY >= detail.maxY - 1, "the button goes under the words")
        }
        let union = title.union(detail).union(dismiss)
        #expect(union.width <= width, "the notice is no wider than the screen: \(union)")
    }
}
