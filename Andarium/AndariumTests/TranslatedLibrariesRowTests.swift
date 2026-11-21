// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// Home's AOT row as the application draws it: the third row of the section
/// that holds the JIT row, below it; there only in a build that carries the
/// translated-code framework; its switch is the person's choice (on for a
/// fresh model); and while the next start runs on the JIT engine, which does
/// not use the translated code, it is dimmed and says so. What a start states
/// to the runtime follows the same facts.
@MainActor
@Suite(.serialized)
struct TranslatedLibrariesRowTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private func fixture(carried: Bool, jit: Bool = true) -> (model: AppModel, driver: StubDriver) {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.jitAvailable = jit
        model.translatedLibrariesAvailable = carried
        return (model, driver)
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    /// The switches under `view`, top to bottom.
    private func switches(in view: UIView) -> [UISwitch] {
        var found: [UISwitch] = []
        func walk(_ node: UIView) {
            if let control = node as? UISwitch { found.append(control) }
            for child in node.subviews { walk(child) }
        }
        walk(view)
        return found.sorted {
            $0.convert($0.bounds, to: view).minY < $1.convert($1.bounds, to: view).minY
        }
    }

    /// Hosts the section the rows are in, on the application's window at a
    /// phone's width. A section that is not there draws nothing, so the caller
    /// says how many switches to wait for, and an absent one is given the time
    /// it would take to appear.
    private func hosted(
        _ model: AppModel, switches count: Int, in lease: HostWindow.Lease
    ) async throws -> UIHostingController<some View> {
        let controller = UIHostingController(rootView: JitSection().environment(model).frame(width: 390))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        let drawn = await eventually {
            controller.view.setNeedsLayout()
            controller.view.layoutIfNeeded()
            return switches(in: controller.view).count == count
        }
        try #require(drawn, "the section drew \(switches(in: controller.view).count) switches, not \(count)")
        if count == 0 {
            // Give a section that should not be there the time it would take.
            try? await Task.sleep(for: .milliseconds(400))
            controller.view.layoutIfNeeded()
            #expect(switches(in: controller.view).isEmpty)
        }
        return controller
    }

    @Test func aFreshModelWantsTheLibrariesAndCarriesNoRow() {
        let fresh = fixture(carried: false).model
        #expect(fresh.translatedLibrariesOn)
        #expect(!fresh.translatedLibrariesAvailable)
        #expect(fresh.translatedLibrariesToKeep == true)
    }

    @Test func theRowIsThereOnlyInABuildThatCarriesTheLibraries() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        // The JIT row alone: no framework, no row.
        _ = try await hosted(fixture(carried: false).model, switches: 1, in: lease)
        // With the framework, a second switch is there.
        let carried = try await hosted(fixture(carried: true).model, switches: 2, in: lease)
        let control = try #require(switches(in: carried.view).last)
        #expect(control.isOn)
        #expect(control.isEnabled)
    }

    @Test func theRowIsTheSecondOneBelowJitInTheSameSection() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (model, _) = fixture(carried: true)
        model.jitOn = true
        model.jitDebuggerAttached = false
        let controller = try await hosted(model, switches: 2, in: lease)
        let all = switches(in: controller.view)
        // JIT's switch is on (the person asked for it) and AOT's is below it:
        // JitSection draws both, so the AOT row is a row of that section.
        #expect(all.first?.isOn == true)
        let frames = all.map { $0.convert($0.bounds, to: controller.view) }
        #expect(frames[1].minY > frames[0].maxY, "the AOT switch is not below the JIT switch")
        // They are in one panel: the same width and trailing edge.
        #expect(abs(frames[0].maxX - frames[1].maxX) < 1)
    }

    @Test func aBuildWithoutAJitHasTheRowAloneInTheSection() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (model, _) = fixture(carried: true, jit: false)
        let controller = try await hosted(model, switches: 1, in: lease)
        #expect(switches(in: controller.view).first?.isOn == true)
        // Neither framework nor JIT: no section at all.
        let bare = fixture(carried: false, jit: false).model
        _ = try await hosted(bare, switches: 0, in: lease)
    }

    @Test func noGuestNoRow() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (empty, _) = fixture(carried: true)
        empty.guest = nil
        _ = try await hosted(empty, switches: 0, in: lease)
    }

    @Test func theSwitchIsThePersonsChoiceAndRoundTrips() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (model, _) = fixture(carried: true)
        let before = model.guest
        let controller = try await hosted(model, switches: 2, in: lease)
        var control = try #require(switches(in: controller.view).last)
        control.setOn(false, animated: false)
        control.sendActions(for: .valueChanged)
        #expect(await eventually { !model.translatedLibrariesOn })
        // It changes nothing in the guest or in the JIT until a start.
        #expect(model.guest == before)
        #expect(!model.jitOn)

        control = try #require(switches(in: controller.view).last)
        #expect(!control.isOn)
        control.setOn(true, animated: false)
        control.sendActions(for: .valueChanged)
        #expect(await eventually { model.translatedLibrariesOn })
    }

    @Test func theSwitchShowsTheChoiceItIsGiven() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (model, _) = fixture(carried: true)
        model.translatedLibrariesOn = false
        let controller = try await hosted(model, switches: 2, in: lease)
        #expect(try #require(switches(in: controller.view).last).isOn == false)
    }

    @Test func whileTheNextStartRunsOnTheJitTheRowIsDimmedAndTheSwitchDisabled() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (model, driver) = fixture(carried: true)
        driver.debuggerAttached = true
        // The JIT is wanted and a debugger is attached: the next start runs on it.
        model.jitOn = true
        model.jitDebuggerAttached = true
        #expect(model.jitInUseForNextStart)
        let controller = try await hosted(model, switches: 2, in: lease)
        let control = try #require(switches(in: controller.view).last)
        #expect(!control.isEnabled)
        // The choice itself is kept as it was.
        #expect(control.isOn)
        #expect(model.translatedLibrariesOn)

        // The JIT off, or no debugger to prepare it: the start runs on the engine
        // that uses them, and the switch works.
        model.jitOn = false
        #expect(await eventually { switches(in: controller.view).last?.isEnabled == true })
        model.jitOn = true
        driver.debuggerAttached = false
        model.refreshJit()
        #expect(!model.jitInUseForNextStart)
        #expect(await eventually { switches(in: controller.view).last?.isEnabled == true })
    }

    @Test func aBuildWithoutAJitNeverRunsOnIt() {
        let (model, _) = fixture(carried: true)
        // Not available: the person's wish for the JIT means nothing.
        model.jitAvailable = false
        model.jitOn = true
        model.jitDebuggerAttached = true
        #expect(!model.jitInUseForNextStart)
    }

    @Test func theLineUnderTheTitleIsTheNoteOrTheReasonItIsNotUsed() {
        #expect(!TranslatedLibrariesWords.note.isEmpty)
        #expect(TranslatedLibrariesWords.notUsedWithJit == "Not used while JIT is on.")
        #expect(TranslatedLibrariesWords.title == "AOT")
    }

    @Test func aStartStatesTheReleaseOnlyWhileTheSwitchIsOnInABuildThatCarriesIt() {
        let (carried, _) = fixture(carried: true)
        #expect(carried.translatedLibrariesStart(jitInUse: false).statesRelease)
        #expect(carried.translatedLibrariesStart(jitInUse: false).word == .on)
        carried.setTranslatedLibraries(false)
        #expect(!carried.translatedLibrariesStart(jitInUse: false).statesRelease)
        #expect(carried.translatedLibrariesStart(jitInUse: false).word == .off)
        #expect(carried.translatedLibrariesStart(jitInUse: true).word == .notUsedWithJit)

        // A build without the framework launches as it always did.
        let (bare, _) = fixture(carried: false)
        bare.setTranslatedLibraries(false)
        #expect(bare.translatedLibrariesStart(jitInUse: false).statesRelease)
        #expect(bare.translatedLibrariesStart(jitInUse: false).word == .unavailable)
    }
}
