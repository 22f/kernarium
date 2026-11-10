// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// The runtime's gate on the snapshots: an operation that changes the data
/// runs only while the guest is stopped, one at a time. A guest on a temporary
/// directory that has run once; nothing boots.
@MainActor
struct SnapshotRuntimeTests {
    /// A runtime and its model. The model plays the runtime's driver only
    /// when asked: with it, the model's phase changes start the readings that
    /// a run has.
    private func make(_ fixture: SnapshotTestGuest, asDriver: Bool = false) -> (LiveRuntime, AppModel) {
        let model = AppModel(
            guest: fixture.guest, host: AppEntry.hostDevice(),
            system: SystemInfo(version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: fixture.storage.createRequirementBytes)
        let runtime = LiveRuntime(
            model: model, storage: fixture.storage,
            marker: PauseMarker(url: fixture.directory.appendingPathComponent("pause.json")))
        if asDriver { model.driver = runtime }
        model.snapshotDriver = runtime
        return (runtime, model)
    }

    @Test func dataChangingOperationsAreRefusedUnlessTheGuestIsStopped() async throws {
        let fixture = try await SnapshotTestGuest.make()
        defer { fixture.remove() }
        let (runtime, model) = make(fixture)
        let made = try #require(await runtime.createSnapshot(named: "kept").value)
        #expect(runtime.snapshotGate == nil)
        #expect(await runtime.snapshotListing().whileNotStopped == nil)

        for phase: Phase in [.booting(.normal), .running(.normal), .running(.recovery), .booting(.restart), .failed] {
            model.phase = phase
            #expect(runtime.snapshotGate == .guestNotStopped, "\(phase)")
            #expect(await runtime.createSnapshot(named: "x").refusal == .guestNotStopped)
            #expect(await runtime.restoreSnapshot(made.id, guest: fixture.guest).refusal == .guestNotStopped)
            #expect(await runtime.deleteSnapshot(made.id).refusal == .guestNotStopped)
            #expect(await runtime.snapshotListing().whileNotStopped == .guestNotStopped)
            // Rename and the list do not wait for a stopped guest.
            #expect(await runtime.renameSnapshot(made.id, to: "named while \(phase)").value != nil)
        }
        // Nothing was changed while it ran.
        model.phase = .home
        let listing = await runtime.snapshotListing()
        #expect(listing.items.count == 1 && listing.items.first?.name == "named while failed")
        #expect(await runtime.deleteSnapshot(made.id).value != nil)
        #expect(await runtime.snapshotListing().items.isEmpty)
    }

    @Test func aRestoreIsAdoptedByTheModelAndItsPlanIsRead() async throws {
        let fixture = try await SnapshotTestGuest.make()
        defer { fixture.remove() }
        let (_, model) = make(fixture, asDriver: true)
        let made = try #require(await model.takeSnapshot(named: "good").value)
        try SnapshotTestDisk.write(fixture.disk, at: 0, count: 1 << 20, value: 2)
        // A growth the person asked for stays; the capacity is at least the disk's.
        model.guest?.pendingCapacityGiB = 16
        let result = await model.restoreSnapshot(made.id)
        #expect(result.value != nil)
        #expect(try SnapshotTestDisk.read(fixture.disk, at: 0) == 1)
        #expect(model.guest?.capacityGiB == 8 && model.guest?.pendingCapacityGiB == 16)
        #expect(try fixture.storage.load()?.pendingCapacityGiB == 16)
        #expect(model.plan == .normal)
        #expect(model.guest?.usedBytes ?? 0 > 0)
    }

    @Test func oneOperationRunsAtATime() async throws {
        let fixture = try await SnapshotTestGuest.make()
        defer { fixture.remove() }
        // The runtime holds its model weakly: without one it is not stopped.
        let (runtime, model) = make(fixture)
        defer { withExtendedLifetime(model) {} }
        async let first = runtime.createSnapshot(named: "first")
        async let second = runtime.createSnapshot(named: "second")
        let results = await [first.refusal, second.refusal]
        let made = await [first.value != nil, second.value != nil]
        // One went ahead and the other found it under way, whichever was first.
        #expect(results.filter { $0 == .busy }.count == 1)
        #expect(made.filter { $0 }.count == 1)
        #expect(await runtime.snapshotListing().items.count == 1)
        #expect(runtime.snapshotGate == nil, "the operation under way is cleared before it returns")
    }

    @Test func aModelWithNoDriverRefusesEveryOperation() async {
        let model = AppModel(
            guest: nil, host: AppEntry.hostDevice(),
            system: SystemInfo(version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 0)
        #expect(await model.takeSnapshot(named: "x").refusal == .noData)
        #expect(await model.restoreSnapshot(SnapshotID()).refusal == .noData)
        #expect(await model.deleteSnapshot(SnapshotID()).refusal == .noData)
        #expect(await model.renameSnapshot(SnapshotID(), to: "x").refusal == .noData)
        #expect(model.snapshotGate == .noData)
        #expect(await model.snapshotListing().createRefusal == .noData)
    }
}
