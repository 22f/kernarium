// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The boot screen's notice for a slow start, in the application: when the
/// model puts it up (by a clock the test moves, so nothing waits three
/// minutes), what Stop does, which controls the screen has with and without a
/// receiver for feedback, and that the notice goes with the boot.
@Suite(.serialized)
@MainActor
struct SlowStartScreenTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 41_000_000_000
    )
    private let t0: TimeInterval = 1_000

    /// A clock the test moves.
    private final class Clock {
        var now: TimeInterval
        init(_ now: TimeInterval) { self.now = now }
        func advance(_ seconds: TimeInterval) { now += seconds }
    }

    private struct Fixture {
        let model: AppModel
        let driver: StubDriver
        let clock: Clock
    }

    private func fixture(driver attached: Bool = true) -> Fixture {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1.0 (9)")
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        if attached { model.driver = driver }
        let clock = Clock(t0)
        model.clock = { clock.now }
        return Fixture(model: model, driver: driver, clock: clock)
    }

    /// A start under way, as `start` leaves it.
    private func begun(_ fixture: Fixture, kind: BootKind = .normal) {
        fixture.model.phase = .booting(kind)
        fixture.model.slowStart.begin(at: fixture.clock.now)
    }

    // MARK: When

    @Test func theStartBeginsTheTimeAndThreeMinutesPutTheNoticeUp() {
        let fixture = fixture()
        fixture.model.start(.normal)
        #expect(fixture.driver.begun.count == 1)
        #expect(fixture.model.slowStart.began == t0)
        fixture.clock.advance(179)
        #expect(!fixture.model.updateSlowStart() && !fixture.model.slowStart.showing)
        #expect(fixture.model.bootElapsedSeconds() == 179)
        fixture.clock.advance(1)
        #expect(fixture.model.updateSlowStart() && fixture.model.slowStart.showing)
        #expect(!fixture.model.updateSlowStart(), "the notice came twice")
    }

    @Test func noNoticeOutsideAStart() {
        let fixture = fixture()
        // Home, a session and a failed run have no start to call slow.
        for phase in [Phase.home, .running(.normal), .failed] {
            fixture.model.phase = phase
            fixture.clock.advance(1_000)
            #expect(!fixture.model.updateSlowStart(), "\(phase)")
            #expect(!fixture.model.slowStart.showing)
        }
    }

    @Test func aRestartBeforeTheBootFinishedKeepsTheStartsTime() {
        let fixture = fixture()
        fixture.model.start(.normal)
        fixture.clock.advance(100)
        fixture.model.restart()
        #expect(fixture.model.phase == .booting(.restart))
        #expect(fixture.model.slowStart.began == t0 && fixture.model.slowStart.restarts == 1)
        fixture.clock.advance(80)
        #expect(fixture.model.updateSlowStart(), "a slow loop of restarts must reach the notice")
    }

    @Test func aRestartOfARunningSessionIsABootOfItsOwn() {
        let fixture = fixture()
        fixture.model.start(.normal)
        fixture.clock.advance(30)
        fixture.model.finishBoot(.normal)
        #expect(fixture.model.slowStart == SlowStart())
        fixture.clock.advance(4_000)
        fixture.model.restart()
        #expect(fixture.model.slowStart.began == fixture.clock.now)
        fixture.clock.advance(179)
        #expect(!fixture.model.updateSlowStart())
    }

    @Test func theNoticeGoesWhenTheBootCompletesOrTheRunEnds() {
        let fixture = fixture()
        fixture.model.start(.normal)
        fixture.clock.advance(200)
        #expect(fixture.model.updateSlowStart())
        fixture.model.finishBoot(.normal)
        #expect(fixture.model.slowStart == SlowStart() && fixture.model.phase == .running(.normal))

        fixture.model.runEnded(RunEnd(result: 0), uptimeSeconds: 1)
        fixture.model.start(.normal)
        fixture.clock.advance(200)
        #expect(fixture.model.updateSlowStart())
        fixture.model.runEnded(RunEnd(result: -5), uptimeSeconds: 200)
        #expect(fixture.model.slowStart == SlowStart() && fixture.model.phase == .failed)
    }

    // MARK: Stop

    /// The boot line never comes and the guest is otherwise up: the tracker
    /// has seen the screen start and nothing after it. The notice comes from
    /// the clock, and Stop reaches the runtime.
    @Test func stopWorksWhenTheBootLineNeverComes() {
        let fixture = fixture()
        fixture.model.start(.normal)
        for milestone in [BootMilestone.kernelRunning, .androidStarting, .screenStarting] {
            fixture.model.bootTracker.reach(milestone, atSeconds: 20)
        }
        #expect(!fixture.model.bootTracker.isReady)
        fixture.clock.advance(181)
        #expect(fixture.model.updateSlowStart())
        fixture.model.stopStart()
        #expect(fixture.driver.guestStops == 1 && fixture.model.slowStart.stopping)
        // Asked again, it is not asked of the runtime again.
        fixture.model.stopStart()
        #expect(fixture.driver.guestStops == 1)
        // The runtime's run returns and the application says whose choice it was.
        fixture.model.runEnded(RunEnd(result: -5, stoppedByUser: true), uptimeSeconds: 181)
        #expect(fixture.model.phase == .failed)
        #expect(fixture.model.runEnd.stoppedByUser && !fixture.model.runEnd.poweredOff)
        #expect(fixture.model.runReport.hasSuffix("Guest ended: it was stopped from the boot screen after 3:01"))
        #expect(fixture.model.slowStart == SlowStart())
    }

    @Test func stopDuringAnUpdateKeepsTheUpdatePending() {
        let fixture = fixture()
        fixture.model.plan = .update(from: "r1", to: "r2")
        fixture.driver.plan = .update(from: "r1", to: "r2")
        fixture.model.start(.normal)
        fixture.clock.advance(200)
        fixture.model.updateSlowStart()
        fixture.model.stopStart()
        #expect(fixture.driver.guestStops == 1)
        fixture.model.runEnded(RunEnd(result: -5, stoppedByUser: true), uptimeSeconds: 200)
        // The update has not finished: what the files say is still an update,
        // which the next launch offers again (`InstanceRules`).
        #expect(fixture.model.plan == .update(from: "r1", to: "r2"))
    }

    @Test func stopAsksNothingOutsideAStart() {
        let fixture = fixture()
        fixture.model.stopStart()
        fixture.model.phase = .running(.normal)
        fixture.model.stopStart()
        #expect(fixture.driver.guestStops == 0)
    }

    @Test func thePreviewEndsTheStartItself() {
        let fixture = fixture(driver: false)
        begun(fixture)
        fixture.clock.advance(181)
        fixture.model.updateSlowStart()
        fixture.model.stopStart()
        #expect(fixture.model.phase == .failed && fixture.model.runEnd.stoppedByUser)
    }

    /// A start the runtime has not reached (the display is not laid out, or
    /// the disk is being prepared) ends in the driver, at Home, and the
    /// preparation then launches nothing.
    @Test func stopBeforeTheRuntimeRunsEndsAtHomeAndLaunchesNothing() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let systemDirectory = try #require(Bundle.main.resourceURL).appendingPathComponent(SystemFolder.name)
        let release = try SystemFolder.release(in: systemDirectory)
        let storage = TwoDiskStorage(
            locations: GuestLocations(directory: directory), release: release.identity, releaseDirectory: systemDirectory)
        let fixture = fixture(driver: false)
        let runtime = LiveRuntime(
            model: fixture.model, storage: storage,
            marker: PauseMarker(url: directory.appendingPathComponent("pause.json")))
        fixture.model.driver = runtime
        // The runtime counts by its own clock; the test moves this one.
        fixture.model.clock = { [clock = fixture.clock] in clock.now }
        let guest = try #require(fixture.model.guest)
        begun(fixture)
        runtime.begin(guest: guest, mode: .normal, kind: .normal)
        fixture.clock.advance(181)
        fixture.model.updateSlowStart()
        fixture.model.stopStart()
        #expect(fixture.model.phase == .home)
        #expect(fixture.model.runEnd.stoppedByUser && fixture.model.runEnd.poweredOff)
        // A display that lays out now finds no start to run.
        autoreleasepool {
            let view = GuestMetalView(frame: .zero, device: nil)
            runtime.viewDidLayout(view, GuestDisplayGeometry(pixelSize: CGSize(width: 100, height: 100), dotsPerInch: 160))
        }
        #expect(runtime.view == nil)
    }

    // MARK: The screen

    private func button(labelled label: String, in node: NSObject, depth: Int = 0) -> NSObject? {
        if node.accessibilityLabel == label, node.accessibilityTraits.contains(.button) { return node }
        guard depth < 64 else { return nil }
        var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = node as? UIView { children += view.subviews }
        for child in children {
            if let found = button(labelled: label, in: child, depth: depth + 1) { return found }
        }
        return nil
    }

    /// The boot screen in the application's window. SwiftUI publishes its
    /// accessibility elements only while application accessibility is on (the
    /// Simulator preference `ApplicationAccessibilityEnabled`); a run without
    /// it finds none and the test says so.
    private func screen(
        _ fixture: Fixture, in lease: HostWindow.Lease, finding labels: [String]
    ) async throws -> (UIViewController, [String: NSObject]) {
        let controller = UIHostingController(rootView: BootView(kind: .normal).environment(fixture.model))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        var found: [String: NSObject] = [:]
        let deadline = ContinuousClock.now + .seconds(5)
        while found.count < labels.count, ContinuousClock.now < deadline {
            for label in labels where found[label] == nil {
                if let element = button(labelled: label, in: controller.view) { found[label] = element }
            }
            if found.count < labels.count { try await Task.sleep(for: .milliseconds(50)) }
        }
        return (controller, found)
    }

    @Test func theNoticeHasStopAndItReachesTheRuntime() async throws {
        let fixture = fixture()
        begun(fixture)
        fixture.clock.advance(185)
        fixture.model.updateSlowStart()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (_, found) = try await screen(fixture, in: lease, finding: ["Stop"])
        guard let stop = found["Stop"] else {
            Issue.record("Stop was not found: is application accessibility on in this Simulator?")
            return
        }
        #expect(stop.accessibilityActivate())
        #expect(fixture.driver.guestStops == 1)
        // Chosen, it says so and takes no second tap.
        let (_, after) = try await screen(fixture, in: lease, finding: ["Stopping\u{2026}"])
        #expect(after["Stopping\u{2026}"] != nil)
        #expect(fixture.driver.guestStops == 1)
    }

    @Test func aBootBeforeThreeMinutesShowsNoNotice() async throws {
        let fixture = fixture()
        begun(fixture)
        fixture.clock.advance(120)
        fixture.model.updateSlowStart()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = UIHostingController(rootView: BootView(kind: .normal).environment(fixture.model))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        try await Task.sleep(for: .milliseconds(300))
        #expect(button(labelled: "Stop", in: controller.view) == nil)
    }

    #if FEEDBACK_MAIL
    @Test func sendFeedbackIsOfferedWhereTheReceiverIs() async throws {
        let fixture = fixture()
        var services = SendFeedbackServices()
        services.address = { "feedback@example.com" }
        fixture.model.feedbackServices = services
        begun(fixture)
        fixture.clock.advance(185)
        fixture.model.updateSlowStart()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (_, found) = try await screen(fixture, in: lease, finding: ["Stop", "Send feedback"])
        guard let feedback = found["Send feedback"] else {
            Issue.record("Send feedback was not found: is application accessibility on in this Simulator?")
            return
        }
        #expect(found["Stop"] != nil)
        #expect(feedback.accessibilityActivate())
        #expect(fixture.model.feedbackSheetOpen)
        fixture.model.feedbackSheetOpen = false
    }

    @Test func withoutAReceiverTheNoticeHasStopAlone() async throws {
        let fixture = fixture()
        var services = SendFeedbackServices()
        services.address = { nil }
        fixture.model.feedbackServices = services
        begun(fixture)
        fixture.clock.advance(185)
        fixture.model.updateSlowStart()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (controller, found) = try await screen(fixture, in: lease, finding: ["Stop"])
        #expect(found["Stop"] != nil, "is application accessibility on in this Simulator?")
        #expect(button(labelled: "Send feedback", in: controller.view) == nil)
    }
    #else
    @Test func aBuildWithoutSendFeedbackHasStopAlone() async throws {
        let fixture = fixture()
        begun(fixture)
        fixture.clock.advance(185)
        fixture.model.updateSlowStart()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (controller, found) = try await screen(fixture, in: lease, finding: ["Stop"])
        #expect(found["Stop"] != nil, "is application accessibility on in this Simulator?")
        #expect(button(labelled: "Send feedback", in: controller.view) == nil)
    }
    #endif
}
