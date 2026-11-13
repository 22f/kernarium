// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// Holds the memory notice's stay after the level has left critical until a test
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
/// read. Only the critical level has a notice.
@MainActor
struct MemoryPressureModelTests {
    @Test func theRuntimesLevelsAreTheApplicationsWords() {
        #expect(LiveRuntime.memoryPressureLevel(runtimeLevel: Int32(KERNARIUM_RUNTIME_MEMORY_PRESSURE_NORMAL)) == .normal)
        #expect(LiveRuntime.memoryPressureLevel(runtimeLevel: Int32(KERNARIUM_RUNTIME_MEMORY_PRESSURE_WARNING)) == .warning)
        #expect(LiveRuntime.memoryPressureLevel(runtimeLevel: Int32(KERNARIUM_RUNTIME_MEMORY_PRESSURE_CRITICAL)) == .critical)
        #expect(LiveRuntime.memoryPressureLevel(runtimeLevel: 99) == nil, "a level it does not know is ignored, not read as normal")
    }

    @Test func theNoticeIsUpOnlyWhileASessionIsOnScreen() {
        let model = makeModel(phase: nil)
        let pace = MemoryNoticePace()
        pace.attach(to: model)
        model.memoryPressureChanged(.critical)
        #expect(model.phase == .home)
        #expect(!model.memoryNoticeShowing, "not on Home")
        model.phase = .booting(.normal)
        #expect(!model.memoryNoticeShowing, "not while the boot screen shows")
        model.phase = .running(.normal)
        #expect(model.memoryNoticeShowing, "the level is still critical: the session shows it")
        model.applicationActive = false
        #expect(!model.memoryNoticeShowing, "not in the background")
        model.applicationActive = true
        #expect(model.memoryNoticeShowing)
        model.phase = .failed
        #expect(!model.memoryNoticeShowing)
    }

    @Test func anEpisodeThatEndedOutOfSightShowsNothingOnReturn() {
        let model = makeModel(phase: nil)
        model.memoryPressureChanged(.critical)
        model.memoryPressureChanged(.warning)
        model.phase = .running(.normal)
        #expect(!model.memoryNoticeShowing, "the level is no longer critical when the session is back")
    }

    @Test func aRecoverySessionShowsItToo() {
        let model = makeModel(phase: .running(.recovery))
        model.memoryPressureChanged(.critical)
        #expect(model.memoryNoticeShowing)
    }

    @Test func aWarningShowsNoNotice() {
        let model = makeModel()
        model.memoryPressureChanged(.warning)
        #expect(!model.memoryNoticeShowing)
        model.memoryPressureChanged(.warning)
        #expect(!model.memoryNoticeShowing, "a repeat changes nothing")
        model.memoryPressureChanged(.critical)
        #expect(model.memoryNoticeShowing, "a rise to critical does")
        #expect(model.memoryNotice.level == .critical)
    }

    @Test func theNoticeStaysWhileTheLevelIsCriticalAndGoesAfterItLeaves() async {
        let model = makeModel()
        let pace = MemoryNoticePace()
        pace.attach(to: model)
        model.memoryPressureChanged(.critical)
        pace.advance(20)
        model.memoryPressureChanged(.critical)
        #expect(!pace.isWaiting, "nothing ends it while the level is critical")
        model.memoryPressureChanged(.warning)
        #expect(await MemoryNoticePace.until { pace.isWaiting })
        #expect(pace.waits == [.seconds(3)], "a few seconds after the fall, as after a return to normal")
        #expect(model.memoryNoticeShowing, "it lingers")
        pace.release(after: 3)
        #expect(await MemoryNoticePace.until { !model.memoryNoticeShowing })
        #expect(!pace.isWaiting)
    }

    @Test func aCriticalLevelThatEndsAtOnceStaysSixSecondsFromItsAppearance() async {
        let model = makeModel()
        let pace = MemoryNoticePace()
        pace.attach(to: model)
        model.memoryPressureChanged(.critical)
        pace.advance(1)
        model.memoryPressureChanged(.normal)
        #expect(await MemoryNoticePace.until { pace.isWaiting })
        #expect(pace.waits == [.seconds(5)])
        pace.release(after: 5)
        #expect(await MemoryNoticePace.until { !model.memoryNoticeShowing })
    }

    @Test func aWakeBeforeTheEndWaitsOutWhatIsLeft() async {
        let model = makeModel()
        let pace = MemoryNoticePace()
        pace.attach(to: model)
        model.memoryPressureChanged(.critical)
        pace.advance(1)
        model.memoryPressureChanged(.normal)
        #expect(await MemoryNoticePace.until { pace.isWaiting })
        pace.release(after: 2)
        #expect(await MemoryNoticePace.until { pace.waits.count == 2 && pace.isWaiting }, "it waited again")
        #expect(pace.waits == [.seconds(5), .seconds(3)])
        #expect(model.memoryNoticeShowing, "the notice was not taken away early")
        pace.release(after: 3)
        #expect(await MemoryNoticePace.until { !model.memoryNoticeShowing })
    }

    @Test func aLevelThatReachesCriticalAgainWhileTheNoticeLingersEndsTheWait() async {
        let model = makeModel()
        let pace = MemoryNoticePace()
        pace.attach(to: model)
        model.memoryPressureChanged(.critical)
        pace.advance(10)
        model.memoryPressureChanged(.warning)
        #expect(await MemoryNoticePace.until { pace.isWaiting })
        pace.advance(1)
        model.memoryPressureChanged(.critical)
        #expect(await MemoryNoticePace.until { !pace.isWaiting }, "the same showing goes on")
        #expect(model.memoryNoticeShowing)
    }

    @Test func aDismissalHoldsUntilTheLevelHasLeftCriticalAndReachedItAgain() {
        let model = makeModel()
        model.memoryPressureChanged(.critical)
        model.dismissMemoryNotice()
        #expect(!model.memoryNoticeShowing)
        model.memoryPressureChanged(.critical)
        #expect(!model.memoryNoticeShowing, "repeated reports in the episode show nothing")
        model.applicationActive = false
        model.applicationActive = true
        #expect(!model.memoryNoticeShowing, "nor does coming back")
        model.memoryPressureChanged(.warning)
        #expect(!model.memoryNoticeShowing, "the dismissed notice does not linger")
        model.memoryPressureChanged(.critical)
        #expect(model.memoryNoticeShowing, "the next episode")
    }

    @Test func theEndOfARunClearsTheNoticeItsDismissalAndItsWait() async {
        let model = makeModel()
        let pace = MemoryNoticePace()
        pace.attach(to: model)
        model.memoryPressureChanged(.critical)
        model.dismissMemoryNotice()
        model.memoryPressureChanged(.normal)
        model.memoryPressureChanged(.critical)
        model.memoryPressureChanged(.normal)
        #expect(model.memoryNoticeShowing)
        #expect(await MemoryNoticePace.until { pace.isWaiting })
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 10)
        #expect(!model.memoryNoticeShowing)
        #expect(await MemoryNoticePace.until { !pace.isWaiting }, "the wait was cancelled")
        // The next run starts clean: the old dismissal is gone.
        model.phase = .running(.normal)
        model.memoryPressureChanged(.critical)
        #expect(model.memoryNoticeShowing)
    }

    @Test func aRestartOfTheGuestClearsTheNoticeItsLevelAndItsWait() async {
        let model = makeModel()
        let pace = MemoryNoticePace()
        pace.attach(to: model)
        model.memoryPressureChanged(.critical)
        model.dismissMemoryNotice()
        model.memoryPressureChanged(.critical)
        model.memoryPressureChanged(.normal)
        model.memoryPressureChanged(.critical)
        #expect(model.memoryNoticeShowing)
        // The guest restarts during the pressure: the restart reports no end of
        // the episode, so the level the notice holds is not known any more.
        model.restart()
        #expect(!model.memoryNoticeShowing)
        #expect(model.memoryNotice.level == .normal)
        #expect(await MemoryNoticePace.until { !pace.isWaiting })
        // It is the same screen on the new boot, with nothing said of memory:
        // the next report starts the episode, and an old dismissal is not held.
        model.phase = .running(.normal)
        #expect(!model.memoryNoticeShowing, "a notice for an episode that ended with the old run does not return")
        model.memoryPressureChanged(.warning)
        #expect(!model.memoryNoticeShowing)
        model.memoryPressureChanged(.critical)
        #expect(model.memoryNoticeShowing)
    }

    @Test func aDismissalDuringTheLingerEndsTheWait() async {
        let model = makeModel()
        let pace = MemoryNoticePace()
        pace.attach(to: model)
        model.memoryPressureChanged(.critical)
        pace.advance(10)
        model.memoryPressureChanged(.normal)
        #expect(await MemoryNoticePace.until { pace.isWaiting })
        model.dismissMemoryNotice()
        #expect(!model.memoryNoticeShowing)
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

    /// The notice in `controller`, waiting for SwiftUI to publish its parts.
    private func parts(in controller: UIViewController) async throws -> Parts? {
        let deadline = ContinuousClock.now + .seconds(5)
        while ContinuousClock.now < deadline {
            controller.view.layoutIfNeeded()
            let title = find(in: controller.view) { $0.accessibilityLabel == MemoryPressureWords.title }
            let detail = find(in: controller.view) { $0.accessibilityLabel == MemoryPressureWords.detail }
            if let title, let detail, let dismiss = memoryDismiss(in: controller) {
                return Parts(title: title, detail: detail, dismiss: dismiss)
            }
            try await Task.sleep(for: .milliseconds(50))
        }
        return nil
    }

    /// Waits for the notice's words to leave the screen, for at most five seconds.
    private func wordsGone(in controller: UIViewController) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(5)
        while ContinuousClock.now < deadline {
            controller.view.layoutIfNeeded()
            if find(in: controller.view, { $0.accessibilityLabel == MemoryPressureWords.title }) == nil {
                return true
            }
            try? await Task.sleep(for: .milliseconds(50))
        }
        return false
    }

    /// The notices in a window of `width` (and `height`, when a test gives the
    /// room a height of its own).
    private func hosted(
        _ model: AppModel, width: CGFloat, height: CGFloat? = nil, size: DynamicTypeSize, in lease: HostWindow.Lease
    ) -> UIViewController {
        let controller = UIHostingController(
            rootView: SessionNoticesOverlay().environment(model).dynamicTypeSize(size)
                .frame(width: width, height: height))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        return controller
    }

    /// The session's overlays as `Live/SessionView` stacks them: the statistics
    /// capsule and the notices from the top, the side keys on the edge over
    /// them.
    private func sessionHosted(
        _ model: AppModel, width: CGFloat, height: CGFloat? = nil, size: DynamicTypeSize, in lease: HostWindow.Lease
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
                .environment(model).dynamicTypeSize(size).frame(width: width, height: height))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        return controller
    }

    /// The scroll views under `view`: the notice column has one only while its
    /// notices do not fit the room.
    private func scrollViews(under view: UIView) -> [UIScrollView] {
        var found: [UIScrollView] = []
        if let scroll = view as? UIScrollView { found.append(scroll) }
        for sub in view.subviews { found += scrollViews(under: sub) }
        return found
    }

    // MARK: Content

    @Test func theNoticeSaysIOSMayCloseAndariumAndTheDismissReachesTheModel() async throws {
        let model = makeModel()
        model.memoryPressureChanged(.critical)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hosted(model, width: 390, size: .large, in: lease)
        guard let found = try await parts(in: controller) else {
            Issue.record("The notice was not found: is application accessibility on in this Simulator?")
            return
        }
        #expect(found.title.accessibilityLabel == "Memory is critically low")
        #expect(found.detail.accessibilityLabel == "iOS may close Andarium.")
        #expect(found.dismiss.accessibilityActivate())
        #expect(!model.memoryNoticeShowing, "dismissing it puts it away")
        #expect(model.memoryNotice.level == .critical, "the level still lasts")
        // The dismissal holds for the episode.
        model.memoryPressureChanged(.critical)
        #expect(!model.memoryNoticeShowing)
        #expect(await wordsGone(in: controller), "the notice is off the screen")
    }

    @Test func aWarningDrawsNothing() async throws {
        let model = makeModel()
        model.memoryPressureChanged(.warning)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hosted(model, width: 390, size: .large, in: lease)
        try await Task.sleep(for: .milliseconds(300))
        controller.view.layoutIfNeeded()
        #expect(find(in: controller.view) { $0.accessibilityLabel == MemoryPressureWords.title } == nil)
        #expect(scrollViews(under: controller.view).isEmpty)
    }

    @Test func theWordsAreNoMoreThanWhatTheAppCanKeep() {
        let words = MemoryPressureWords.title + " " + MemoryPressureWords.detail
        for name in ["Google", "Pixel", "Gemini", "Chrome", "Android"] {
            #expect(!words.contains(name), "\(name) in: \(words)")
        }
        #expect(MemoryPressureWords.detail == "iOS may close Andarium.", "one line, and no advice the app cannot keep")
        #expect(
            MemoryPressureWords.announcement == "Memory is critically low. iOS may close Andarium.",
            "VoiceOver says the title and the line")
    }

    @Test func theNoticeLeavesTheScreenWhenTheLevelHasLeftCriticalAndItsStayEnds() async throws {
        let model = makeModel()
        let pace = MemoryNoticePace()
        pace.attach(to: model)
        model.memoryPressureChanged(.critical)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hosted(model, width: 390, size: .large, in: lease)
        guard try await parts(in: controller) != nil else {
            Issue.record("The notice was not found: is application accessibility on in this Simulator?")
            return
        }
        pace.advance(20)
        model.memoryPressureChanged(.warning)
        #expect(await MemoryNoticePace.until { pace.isWaiting })
        try await Task.sleep(for: .milliseconds(300))
        controller.view.layoutIfNeeded()
        #expect(
            find(in: controller.view) { $0.accessibilityLabel == MemoryPressureWords.title } != nil,
            "still up during the stay")
        pace.release(after: 3)
        #expect(await wordsGone(in: controller), "gone when the stay ends")
    }

    // MARK: Frames

    /// The notice in a window of `width`, its words and button inside the
    /// column and not wider than it, the button under the words at the
    /// accessibility sizes, and all of it inside the window's height.
    @Test(arguments: [DynamicTypeSize.large, .accessibility3, .accessibility5])
    func theNoticeFitsANarrowScreenAtEveryTextSize(size: DynamicTypeSize) async throws {
        let model = makeModel()
        model.memoryPressureChanged(.critical)
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
        let screen = lease.window.bounds
        #expect(
            title.minY >= screen.minY && dismiss.maxY <= screen.maxY,
            "inside the screen's height \(screen.height): \(title.union(dismiss))")
        #expect(scrollViews(under: controller.view).isEmpty, "a notice that fits is not in a scroll view")
        let union = title.union(detail).union(dismiss)
        #expect(union.width <= width, "the notice is no wider than the screen: \(union)")
    }

    // MARK: Coexistence

    /// The memory notice under the Camera-is-off notice, in the column the
    /// session gives its notices: it is the later one, below the camera notice,
    /// nothing overlaps another, and the column stays clear of the statistics
    /// capsule above and the closed side keys on the edge. At the largest size
    /// the two are taller than a phone's window and the column scrolls: it is
    /// then inside the window, and scrolled to its end shows the memory notice
    /// and its button in the window.
    @Test(arguments: [DynamicTypeSize.large, .accessibility5], [true, false])
    func theNoticeJoinsTheStackBelowTheCameraNoticeClearOfTheCapsuleAndTheKeys(
        size: DynamicTypeSize, statsShowing: Bool
    ) async throws {
        let model = makeModel()
        model.cameraAccess = .notDetermined
        model.cameraCaptureChanged(.blockedInUse)
        model.memoryPressureChanged(.critical)
        model.session.showStats = statsShowing
        #expect(model.cameraNoticeKind == .cameraOff)
        #expect(model.memoryNoticeShowing)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let width: CGFloat = 390
        let controller = sessionHosted(model, width: width, size: size, in: lease)
        guard let memory = try await parts(in: controller),
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
        let screen = lease.window.bounds
        let column = controller.view.convert(controller.view.bounds, to: nil)
            .insetBy(dx: (controller.view.bounds.width - width) / 2, dy: 0)
        for frame in [cameraFrame, memoryFrame] {
            #expect(
                frame.minX >= column.minX - 0.5 && frame.maxX <= column.maxX + 0.5, "inside the column: \(frame)")
        }
        var capsuleFrame: CGRect?
        if statsShowing {
            let spoken = model.session.readout.spoken(memoryMiB: 2048, recovery: false)
            guard let capsule = find(in: controller.view, { $0.accessibilityLabel == spoken }) else {
                Issue.record("The statistics capsule was not found")
                return
            }
            capsuleFrame = capsule.accessibilityFrame
            #expect(!capsule.accessibilityFrame.intersects(cameraFrame), "the capsule keeps its place")
            #expect(!capsule.accessibilityFrame.intersects(memoryFrame))
            #expect(cameraFrame.minY >= capsule.accessibilityFrame.maxY, "the notices hang below the capsule")
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
        try await checkColumnReachesItsNotices(
            controller, memoryDismiss: memory.dismiss, screen: screen, below: capsuleFrame, keys: keysFrame)
    }

    /// The column, plain or scrolling, as the room decides: plain when
    /// everything is inside the window; otherwise one scroll view inside the
    /// window, below the capsule, no wider than the notices' glass and clear of
    /// the keys, whose scrolled end puts the last Dismiss inside the window.
    private func checkColumnReachesItsNotices(
        _ controller: UIViewController, memoryDismiss: NSObject, screen: CGRect, below capsule: CGRect?, keys: CGRect
    ) async throws {
        let scrolls = scrollViews(under: controller.view)
        if scrolls.isEmpty {
            #expect(
                screen.contains(memoryDismiss.accessibilityFrame),
                "the column fits, and the last button is in the window: \(memoryDismiss.accessibilityFrame)")
            return
        }
        #expect(scrolls.count == 1, "one scroll view for the column: \(scrolls.count)")
        let scroll = try #require(scrolls.first)
        let frame = scroll.convert(scroll.bounds, to: nil)
        #expect(screen.insetBy(dx: -0.5, dy: -0.5).contains(frame), "the scroll view is in the window: \(frame) in \(screen)")
        if let capsule { #expect(frame.minY >= capsule.maxY, "below the capsule: \(frame) and \(capsule)") }
        #expect(frame.maxX <= keys.maxX - 12 + 0.5, "clear of the keys' capsules: \(frame) and \(keys)")
        #expect(scroll.contentSize.height > scroll.bounds.height, "it scrolls because the notices are taller than the room")
        scroll.setContentOffset(
            CGPoint(x: 0, y: scroll.contentSize.height - scroll.bounds.height + scroll.adjustedContentInset.bottom),
            animated: false)
        controller.view.layoutIfNeeded()
        try await Task.sleep(for: .milliseconds(100))
        let button = memoryDismiss.accessibilityFrame
        #expect(
            frame.insetBy(dx: -0.5, dy: -0.5).contains(button),
            "scrolled to its end, the last button is in the scroll view: \(button) in \(frame)")
    }

    /// The column scrolls when the room is shorter than its notices, at any
    /// text size, and not otherwise.
    @Test func aColumnTallerThanItsRoomScrollsAndAColumnThatFitsDoesNot() async throws {
        let model = makeModel()
        model.cameraAccess = .notDetermined
        model.cameraCaptureChanged(.blockedInUse)
        model.memoryPressureChanged(.critical)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let tall = hosted(model, width: 390, height: 700, size: .large, in: lease)
        guard let memory = try await parts(in: tall) else {
            Issue.record("The notice was not found: is application accessibility on in this Simulator?")
            return
        }
        #expect(scrollViews(under: tall.view).isEmpty, "two notices at the default size fit 700 points")
        #expect(lease.window.bounds.contains(memory.dismiss.accessibilityFrame))
        let short = hosted(model, width: 390, height: 160, size: .large, in: lease)
        _ = try await parts(in: short)
        #expect(scrollViews(under: short.view).count == 1, "two notices do not fit 160 points: they scroll")
    }

    /// Every notice the session has, up together at the default size: the
    /// stack keeps its order, each below the one before, the memory notice last.
    @Test func everyNoticeUpTogetherStacksInOrder() async throws {
        let model = makeModel()
        model.cameraAccess = .notDetermined
        model.cameraCaptureChanged(.blockedInUse)
        model.previewNetworkNotice(.wifi)
        model.previewGraphicsNotice()
        model.previewThermalNotice()
        model.memoryPressureChanged(.critical)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hosted(model, width: 390, height: 1400, size: .large, in: lease)
        guard let memory = try await parts(in: controller),
            let network = find(in: controller.view, { $0.accessibilityLabel == NetworkWords.title(for: .wifi) }),
            let graphics = find(in: controller.view, { $0.accessibilityLabel == GraphicsNotice.title }),
            let thermal = find(in: controller.view, { $0.accessibilityLabel == ThermalNotice.text }),
            let camera = find(in: controller.view, { $0.accessibilityLabel == CameraWords.offTitle })
        else {
            Issue.record("A notice was not found: is application accessibility on in this Simulator?")
            return
        }
        let order = [network, graphics, thermal, camera, memory.title].map { $0.accessibilityFrame.minY }
        #expect(order == order.sorted(), "network, graphics, thermal, camera, then memory: \(order)")
        #expect(Set(order).count == order.count, "each notice has a place of its own")
    }
}
