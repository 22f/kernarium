// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// Holds the memory notice's stay after the pressure has passed until a test
/// ends it, and gives the model the clock a test moves by hand.
@MainActor
final class MemoryNoticePace {
    private(set) var waits: [Duration] = []
    private var held: CheckedContinuation<Void, Never>?
    var now = ContinuousClock.now

    func attach(to model: AppModel) {
        model.memoryNoticeSleep = { [self] duration in await sleep(duration) }
        model.memoryNoticeClock = { [self] in now }
    }

    private func sleep(_ duration: Duration) async {
        waits.append(duration)
        await withTaskCancellationHandler {
            await withCheckedContinuation { held = $0 }
        } onCancel: {
            Task { @MainActor in self.letGo() }
        }
    }

    var isWaiting: Bool { held != nil }

    func advance(_ seconds: Int) { now = now.advanced(by: .seconds(seconds)) }

    /// The stay ends `seconds` later by the model's clock.
    func release(after seconds: Int) {
        advance(seconds)
        letGo()
    }

    private func letGo() {
        let continuation = held
        held = nil
        continuation?.resume()
    }

    /// Lets the model's task run until `condition` holds, for at most five seconds.
    static func until(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(5)
        while !condition() {
            if ContinuousClock.now > deadline { return false }
            try? await Task.sleep(for: .milliseconds(1))
        }
        return true
    }
}

private let phoneHost = HostDevice(
    name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
)

/// A model with a guest, as a session has it.
@MainActor
private func makeModel(phase: Phase? = .running(.normal)) -> AppModel {
    let system = SystemInfo(
        version: "17", release: "r2", kernel: "Linux 6.18",
        architecture: "arm64, 4 KiB pages", appVersion: "1")
    let model = AppModel(
        guest: Guest.fresh(on: phoneHost), host: phoneHost, system: system, capabilities: .device,
        createRequirementBytes: 1)
    if let phase { model.phase = phase }
    return model
}

/// What iOS says about memory reaches the session: when the notice is up, how
/// it follows the levels, and what a dismissal holds, on the model the screens
/// read.
@MainActor
struct MemoryPressureModelTests {
    @Test func theRuntimesLevelsAreTheApplicationsWords() {
        #expect(LiveRuntime.memoryPressureLevel(runtimeLevel: Int32(KERNARIUM_RUNTIME_MEMORY_PRESSURE_NORMAL)) == .normal)
        #expect(LiveRuntime.memoryPressureLevel(runtimeLevel: Int32(KERNARIUM_RUNTIME_MEMORY_PRESSURE_WARNING)) == .warning)
        #expect(LiveRuntime.memoryPressureLevel(runtimeLevel: Int32(KERNARIUM_RUNTIME_MEMORY_PRESSURE_CRITICAL)) == .critical)
        #expect(LiveRuntime.memoryPressureLevel(runtimeLevel: 99) == .normal, "a level it does not know raises nothing")
    }

    @Test func theNoticeIsUpOnlyWhileASessionIsOnScreen() {
        let model = makeModel(phase: nil)
        let pace = MemoryNoticePace()
        pace.attach(to: model)
        model.memoryPressureChanged(.warning)
        #expect(model.phase == .home)
        #expect(model.memoryNoticeKind == nil, "not on Home")
        model.phase = .booting(.normal)
        #expect(model.memoryNoticeKind == nil, "not while the boot screen shows")
        model.phase = .running(.normal)
        #expect(model.memoryNoticeKind == .low, "the pressure lasts: the session shows it")
        model.applicationActive = false
        #expect(model.memoryNoticeKind == nil, "not in the background")
        model.applicationActive = true
        #expect(model.memoryNoticeKind == .low)
        model.phase = .failed
        #expect(model.memoryNoticeKind == nil)
    }

    @Test func aRecoverySessionShowsItToo() {
        let model = makeModel(phase: .running(.recovery))
        model.memoryPressureChanged(.critical)
        #expect(model.memoryNoticeKind == .critical)
    }

    @Test func aWarningThenCriticalIsOneNoticeThatChangesItsWords() {
        let model = makeModel()
        model.memoryPressureChanged(.warning)
        #expect(model.memoryNoticeKind == .low)
        model.memoryPressureChanged(.warning)
        #expect(model.memoryNoticeKind == .low, "a repeat changes nothing")
        model.memoryPressureChanged(.critical)
        #expect(model.memoryNoticeKind == .critical)
        #expect(model.memoryNoticeShowing)
    }

    @Test func theNoticeStaysWhileThePressureLastsAndGoesAfterItsReturnToNormal() async {
        let model = makeModel()
        let pace = MemoryNoticePace()
        pace.attach(to: model)
        model.memoryPressureChanged(.warning)
        pace.advance(20)
        model.memoryPressureChanged(.warning)
        #expect(!pace.isWaiting, "nothing ends it while the pressure lasts")
        model.memoryPressureChanged(.normal)
        #expect(await MemoryNoticePace.until { pace.isWaiting })
        #expect(pace.waits == [.seconds(3)], "a few seconds after the return")
        #expect(model.memoryNoticeKind == .low, "it lingers")
        pace.release(after: 3)
        #expect(await MemoryNoticePace.until { model.memoryNoticeKind == nil })
        #expect(!pace.isWaiting)
    }

    @Test func aWarningThatEndsAtOnceStaysSixSecondsFromItsAppearance() async {
        let model = makeModel()
        let pace = MemoryNoticePace()
        pace.attach(to: model)
        model.memoryPressureChanged(.warning)
        pace.advance(1)
        model.memoryPressureChanged(.normal)
        #expect(await MemoryNoticePace.until { pace.isWaiting })
        #expect(pace.waits == [.seconds(5)])
        pace.release(after: 5)
        #expect(await MemoryNoticePace.until { model.memoryNoticeKind == nil })
    }

    @Test func aLevelThatRisesAgainWhileTheNoticeLingersEndsTheWait() async {
        let model = makeModel()
        let pace = MemoryNoticePace()
        pace.attach(to: model)
        model.memoryPressureChanged(.warning)
        pace.advance(10)
        model.memoryPressureChanged(.normal)
        #expect(await MemoryNoticePace.until { pace.isWaiting })
        pace.advance(1)
        model.memoryPressureChanged(.warning)
        #expect(await MemoryNoticePace.until { !pace.isWaiting }, "the same showing goes on")
        #expect(model.memoryNoticeKind == .low)
    }

    @Test func aDismissalHoldsUntilTheLevelHasReturnedToNormalAndRisenAgain() {
        let model = makeModel()
        model.memoryPressureChanged(.warning)
        model.dismissMemoryNotice()
        #expect(model.memoryNoticeKind == nil)
        model.memoryPressureChanged(.warning)
        #expect(model.memoryNoticeKind == nil, "repeated warnings in the episode show nothing")
        model.applicationActive = false
        model.applicationActive = true
        #expect(model.memoryNoticeKind == nil, "nor does coming back")
        model.memoryPressureChanged(.critical)
        #expect(model.memoryNoticeKind == .critical, "a rise to critical shows once more")
        model.dismissMemoryNotice()
        model.memoryPressureChanged(.warning)
        model.memoryPressureChanged(.critical)
        #expect(model.memoryNoticeKind == nil)
        model.memoryPressureChanged(.normal)
        model.memoryPressureChanged(.warning)
        #expect(model.memoryNoticeKind == .low, "the next episode")
    }

    @Test func theEndOfARunClearsTheNoticeItsDismissalAndItsWait() async {
        let model = makeModel()
        let pace = MemoryNoticePace()
        pace.attach(to: model)
        model.memoryPressureChanged(.warning)
        model.dismissMemoryNotice()
        model.memoryPressureChanged(.normal)
        model.memoryPressureChanged(.critical)
        model.memoryPressureChanged(.normal)
        #expect(model.memoryNoticeKind == .critical)
        #expect(await MemoryNoticePace.until { pace.isWaiting })
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 10)
        #expect(model.memoryNoticeKind == nil)
        #expect(await MemoryNoticePace.until { !pace.isWaiting }, "the wait was cancelled")
        // The next run starts clean: the old dismissal is gone.
        model.phase = .running(.normal)
        model.memoryPressureChanged(.warning)
        #expect(model.memoryNoticeKind == .low)
    }

    @Test func aDismissalDuringTheLingerEndsTheWait() async {
        let model = makeModel()
        let pace = MemoryNoticePace()
        pace.attach(to: model)
        model.memoryPressureChanged(.warning)
        pace.advance(10)
        model.memoryPressureChanged(.normal)
        #expect(await MemoryNoticePace.until { pace.isWaiting })
        model.dismissMemoryNotice()
        #expect(model.memoryNoticeKind == nil)
        #expect(await MemoryNoticePace.until { !pace.isWaiting })
    }
}

/// The words of the notice, and the notice drawn in a window as the session
/// draws it, with the notices it shares the column with. SwiftUI publishes its
/// accessibility elements only while application accessibility is on (the
/// Simulator preference `ApplicationAccessibilityEnabled`, as `Kernarium/
/// scripts/run-ios-simulator-tests.sh` sets it); a run without it finds none
/// and the tests fail, for they check nothing then.
@MainActor
struct MemoryPressureNoticeBannerTests {
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

    private func count(in node: NSObject, depth: Int = 0, _ match: (NSObject) -> Bool) -> Int {
        guard depth < 64 else { return 0 }
        var total = match(node) ? 1 : 0
        var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = node as? UIView { children += view.subviews }
        for child in children { total += count(in: child, depth: depth + 1, match) }
        return total
    }

    private struct Parts {
        var title: NSObject
        var detail: NSObject
        var dismiss: NSObject
    }

    private func identifier(of node: NSObject) -> String? {
        (node as? UIAccessibilityIdentification)?.accessibilityIdentifier
    }

    /// Every element of the tree under `node` that `match` accepts.
    private func all(in node: NSObject, depth: Int = 0, _ match: (NSObject) -> Bool) -> [NSObject] {
        guard depth < 64 else { return [] }
        var found = match(node) ? [node] : []
        var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = node as? UIView { children += view.subviews }
        for child in children { found += all(in: child, depth: depth + 1, match) }
        return found
    }

    /// The memory notice's button: by its identifier, or else the lowest
    /// Dismiss button on the screen, for the notice is the last of the stack.
    private func memoryDismiss(in controller: UIViewController) -> NSObject? {
        if let button = find(in: controller.view, { identifier(of: $0) == "memory-notice-dismiss" }) {
            return button
        }
        return all(in: controller.view) {
            $0.accessibilityLabel == NetworkWords.dismissLabel && $0.accessibilityTraits.contains(.button)
        }.max { $0.accessibilityFrame.minY < $1.accessibilityFrame.minY }
    }

    /// The notice of `kind` in `controller`, waiting for SwiftUI to publish its parts.
    private func parts(
        of kind: MemoryPressureNotice, in controller: UIViewController
    ) async throws -> Parts? {
        let deadline = ContinuousClock.now + .seconds(5)
        while ContinuousClock.now < deadline {
            controller.view.layoutIfNeeded()
            let title = find(in: controller.view) { $0.accessibilityLabel == MemoryPressureWords.title(for: kind) }
            let detail = find(in: controller.view) { $0.accessibilityLabel == MemoryPressureWords.detail(for: kind) }
            if let title, let detail, let dismiss = memoryDismiss(in: controller) {
                return Parts(title: title, detail: detail, dismiss: dismiss)
            }
            try await Task.sleep(for: .milliseconds(50))
        }
        return nil
    }

    /// Waits for the notice's words to leave the screen, for at most five seconds.
    private func wordsGone(of kind: MemoryPressureNotice, in controller: UIViewController) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(5)
        while ContinuousClock.now < deadline {
            controller.view.layoutIfNeeded()
            if find(in: controller.view, { $0.accessibilityLabel == MemoryPressureWords.title(for: kind) }) == nil {
                return true
            }
            try? await Task.sleep(for: .milliseconds(50))
        }
        return false
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

    /// The session's overlays as `Live/SessionView` stacks them: the statistics
    /// capsule and the notices from the top, the side keys on the edge over
    /// them.
    private func sessionHosted(
        _ model: AppModel, width: CGFloat, size: DynamicTypeSize, in lease: HostWindow.Lease
    ) -> UIViewController {
        let actions = SideKeyActions(
            power: {}, powerHold: {}, volumeUp: {}, volumeDown: {}, back: {}, home: {}, settings: {})
        let controller = UIHostingController(
            rootView: Color.clear
                .overlay(alignment: .top) {
                    StatsPill(readout: model.session.readout, memoryMiB: 2048)
                        .padding(.top, StatsPlacement.top)
                        .allowsHitTesting(false)
                }
                .overlay(alignment: .top) { SessionNoticesOverlay() }
                .overlay { SideKeysOverlay(mode: .normal, lit: true, open: .constant(false), actions: actions) }
                .environment(model).dynamicTypeSize(size).frame(width: width))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        return controller
    }

    // MARK: Content

    @Test func theWarningSaysIOSAskedForMemoryAndTheDismissReachesTheModel() async throws {
        let model = makeModel()
        model.memoryPressureChanged(.warning)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hosted(model, width: 390, size: .large, in: lease)
        guard let found = try await parts(of: .low, in: controller) else {
            Issue.record("The notice was not found: is application accessibility on in this Simulator?")
            return
        }
        #expect(found.title.accessibilityLabel == "Memory is low")
        #expect(
            found.detail.accessibilityLabel
                == "iOS asked Andarium to free memory. The virtual machine may slow down for a moment.")
        #expect(found.dismiss.accessibilityActivate())
        #expect(model.memoryNoticeKind == nil, "dismissing it puts it away")
        #expect(model.memoryNotice.level == .warning, "the pressure still lasts")
        // The dismissal holds for the episode.
        model.memoryPressureChanged(.warning)
        #expect(model.memoryNoticeKind == nil)
        #expect(await wordsGone(of: .low, in: controller), "the notice is off the screen")
    }

    @Test func criticalSaysIOSMayCloseAndarium() async throws {
        let model = makeModel()
        model.memoryPressureChanged(.critical)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hosted(model, width: 390, size: .large, in: lease)
        guard let found = try await parts(of: .critical, in: controller) else {
            Issue.record("The notice was not found: is application accessibility on in this Simulator?")
            return
        }
        #expect(found.title.accessibilityLabel == "Memory is critically low")
        #expect(
            found.detail.accessibilityLabel
                == "iOS may close Andarium. Close other apps, or shut down the virtual machine and lower its memory with Edit under Virtual machine.")
    }

    @Test func theWordsAreNoMoreThanWhatTheAppCanKeep() {
        for notice in [MemoryPressureNotice.low, .critical] {
            let words = MemoryPressureWords.title(for: notice) + " " + MemoryPressureWords.detail(for: notice)
            for name in ["Google", "Pixel", "Gemini", "Chrome", "Android"] {
                #expect(!words.contains(name), "\(name) in: \(words)")
            }
        }
        // Where memory is set, as the app names it on Home.
        #expect(MemoryPressureWords.criticalDetail.contains("Edit under Virtual machine"))
        #expect(!MemoryPressureWords.lowDetail.contains("close"), "a warning promises nothing about closing")
    }

    @Test func aRiseToCriticalUpdatesTheOneNoticeOnTheScreen() async throws {
        let model = makeModel()
        model.memoryPressureChanged(.warning)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hosted(model, width: 390, size: .large, in: lease)
        guard try await parts(of: .low, in: controller) != nil else {
            Issue.record("The notice was not found: is application accessibility on in this Simulator?")
            return
        }
        model.memoryPressureChanged(.critical)
        guard try await parts(of: .critical, in: controller) != nil else {
            Issue.record("The critical words were not found")
            return
        }
        #expect(await wordsGone(of: .low, in: controller), "the warning's words are replaced")
        // One Dismiss button is one notice: nothing else is up in this model.
        let notices = count(in: controller.view) {
            $0.accessibilityLabel == NetworkWords.dismissLabel && $0.accessibilityTraits.contains(.button)
        }
        #expect(notices == 1, "one notice, not two: \(notices)")
    }

    @Test func theNoticeLeavesTheScreenWhenThePressureHasPassedAndItsStayEnds() async throws {
        let model = makeModel()
        let pace = MemoryNoticePace()
        pace.attach(to: model)
        model.memoryPressureChanged(.warning)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hosted(model, width: 390, size: .large, in: lease)
        guard try await parts(of: .low, in: controller) != nil else {
            Issue.record("The notice was not found: is application accessibility on in this Simulator?")
            return
        }
        pace.advance(20)
        model.memoryPressureChanged(.normal)
        #expect(await MemoryNoticePace.until { pace.isWaiting })
        try await Task.sleep(for: .milliseconds(300))
        controller.view.layoutIfNeeded()
        #expect(
            find(in: controller.view) { $0.accessibilityLabel == MemoryPressureWords.lowTitle } != nil,
            "still up during the stay")
        pace.release(after: 3)
        #expect(await wordsGone(of: .low, in: controller), "gone when the stay ends")
    }

    // MARK: Frames

    /// The notice in a window of `width`, its words and button inside the
    /// column and not wider than it, the button under the words at the
    /// accessibility sizes.
    @Test(arguments: [DynamicTypeSize.large, .accessibility3, .accessibility5], [MemoryPressureNotice.low, .critical])
    func theNoticeFitsANarrowScreenAtEveryTextSize(size: DynamicTypeSize, kind: MemoryPressureNotice) async throws {
        let model = makeModel()
        model.memoryPressureChanged(kind == .low ? .warning : .critical)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let width: CGFloat = 320
        let controller = hosted(model, width: width, size: size, in: lease)
        guard let found = try await parts(of: kind, in: controller) else {
            Issue.record("The notice was not found: is application accessibility on in this Simulator?")
            return
        }
        let title = found.title.accessibilityFrame
        let detail = found.detail.accessibilityFrame
        let dismiss = found.dismiss.accessibilityFrame
        // The column the notice is laid out in, in the window's coordinates: the hosted view is
        // centred in the window, so the frames sit inside it and not only no wider than it.
        let column = controller.view.convert(controller.view.bounds, to: nil)
            .insetBy(dx: (controller.view.bounds.width - width) / 2, dy: 0)
        for frame in [title, detail, dismiss] {
            #expect(frame.width > 0 && frame.width <= width, "inside the width: \(frame)")
            #expect(
                frame.minX >= column.minX - 0.5 && frame.maxX <= column.maxX + 0.5,
                "inside the column \(column): \(frame)")
        }
        #expect(detail.minY >= title.maxY - 1, "the detail is under the title")
        if size.isAccessibilitySize {
            #expect(dismiss.minY >= detail.maxY - 1, "the button goes under the words")
        }
        // The critical words at the largest size are taller than a phone's
        // window at this narrow width, and the stack of notices does not
        // scroll: every other case keeps inside the height.
        let screen = lease.window.bounds
        if !(size == .accessibility5 && kind == .critical) {
            #expect(
                title.minY >= screen.minY && dismiss.maxY <= screen.maxY,
                "inside the screen's height \(screen.height): \(title.union(dismiss))")
        }
        let union = title.union(detail).union(dismiss)
        #expect(union.width <= width, "the notice is no wider than the screen: \(union)")
    }

    // MARK: Coexistence

    /// The memory notice under the Camera-is-off notice, in the column the
    /// session gives its notices: the later one joins the stack below the ones
    /// up, nothing overlaps another, and nothing leaves the screen. The
    /// statistics capsule above and the closed side keys on the edge keep
    /// their places.
    @Test(arguments: [DynamicTypeSize.large, .accessibility5], [true, false])
    func theNoticeJoinsTheStackBelowTheCameraNoticeClearOfTheCapsuleAndTheKeys(
        size: DynamicTypeSize, statsShowing: Bool
    ) async throws {
        let model = makeModel()
        model.cameraAccess = .notDetermined
        model.cameraCaptureChanged(.blockedInUse)
        model.memoryPressureChanged(.warning)
        model.session.showStats = statsShowing
        #expect(model.cameraNoticeKind == .cameraOff)
        #expect(model.memoryNoticeKind == .low)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let width: CGFloat = 390
        let controller = sessionHosted(model, width: width, size: size, in: lease)
        guard let memory = try await parts(of: .low, in: controller),
            let cameraTitle = find(in: controller.view, { $0.accessibilityLabel == CameraWords.offTitle }),
            let cameraDetail = find(in: controller.view, { $0.accessibilityLabel == CameraWords.offDetail })
        else {
            Issue.record("A notice was not found: is application accessibility on in this Simulator?")
            return
        }
        let cameraFrame = cameraTitle.accessibilityFrame.union(cameraDetail.accessibilityFrame)
        let memoryFrame = memory.title.accessibilityFrame.union(memory.detail.accessibilityFrame)
            .union(memory.dismiss.accessibilityFrame)
        #expect(memoryFrame.minY >= cameraFrame.maxY, "the memory notice is below the camera notice")
        #expect(!cameraFrame.intersects(memoryFrame), "they do not overlap")
        // Two notices at the largest size are taller than a phone's window;
        // the one above is dismissed first, and the window of an iPad is tall
        // enough for both.
        let screen = lease.window.bounds
        if size != .accessibility5 {
            #expect(memoryFrame.maxY <= screen.maxY, "inside the screen: \(memoryFrame) in \(screen)")
        }
        let column = controller.view.convert(controller.view.bounds, to: nil)
            .insetBy(dx: (controller.view.bounds.width - width) / 2, dy: 0)
        for frame in [cameraFrame, memoryFrame] {
            #expect(
                frame.minX >= column.minX - 0.5 && frame.maxX <= column.maxX + 0.5, "inside the column: \(frame)")
        }
        if statsShowing {
            let spoken = model.session.readout.spoken(memoryMiB: 2048, recovery: false)
            guard let capsule = find(in: controller.view, { $0.accessibilityLabel == spoken }) else {
                Issue.record("The statistics capsule was not found")
                return
            }
            let capsuleFrame = capsule.accessibilityFrame
            #expect(!capsuleFrame.intersects(cameraFrame), "the capsule keeps its place: \(capsuleFrame)")
            #expect(!capsuleFrame.intersects(memoryFrame))
            #expect(cameraFrame.minY >= capsuleFrame.maxY, "the notices hang below the capsule")
        }
        guard let keys = find(in: controller.view, { $0.accessibilityLabel == "Side keys" }) else {
            Issue.record("The side keys were not found")
            return
        }
        // The closed keys' capsules sit in the last 12 points of their 36-point frame, on the edge the
        // session's column keeps a gutter from.
        let keysFrame = keys.accessibilityFrame
        for frame in [cameraFrame, memoryFrame] {
            #expect(frame.maxX <= keysFrame.maxX - 12 + 0.5, "short of the keys' capsules: \(frame) and \(keysFrame)")
        }
    }

    /// Every notice the session has, up together at the default size: the
    /// stack keeps its order, each below the one before, the memory notice last.
    @Test func everyNoticeUpTogetherStacksInOrder() async throws {
        let model = makeModel()
        model.cameraAccess = .notDetermined
        model.cameraCaptureChanged(.blockedInUse)
        model.previewGraphicsNotice()
        model.memoryPressureChanged(.critical)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hosted(model, width: 390, size: .large, in: lease)
        guard let memory = try await parts(of: .critical, in: controller),
            let graphics = find(in: controller.view, { $0.accessibilityLabel == GraphicsNotice.title }),
            let camera = find(in: controller.view, { $0.accessibilityLabel == CameraWords.offTitle })
        else {
            Issue.record("A notice was not found: is application accessibility on in this Simulator?")
            return
        }
        let order = [graphics, camera, memory.title].map { $0.accessibilityFrame.minY }
        #expect(order == order.sorted(), "graphics, then camera, then memory: \(order)")
    }
}
