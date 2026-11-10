// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

private let createSteps: [SnapshotStep] = [.createdStaging, .clonedData, .copiedRecord, .wroteMetadata, .published]
private let deleteSteps: [SnapshotStep] = [.setAside, .removed]
private let restoreSteps: [SnapshotStep] = [
    .recordedIntent, .replacedData, .replacedRecord, .removedUpdateCopies, .settledSettings, .finishedRestore,
]
/// What a recovery runs again: the steps of a restore after its intent.
private let recoverySteps = Array(restoreSteps.dropFirst())

/// A seam that stops an operation after `target`, as a crash would: the files
/// stay as that step left them.
private func stopping(after target: SnapshotStep) -> @Sendable (SnapshotStep) throws -> Void {
    { step in if step == target { throw CancellationError() } }
}

private func exists(_ url: URL) -> Bool { FileManager.default.fileExists(atPath: url.path) }

/// Every step of every operation, stopped there and then recovered by the next
/// launch, ends in the state an uninterrupted run reaches or in none at all.
@Suite(.enabled(if: temporaryVolumeClones, "needs a volume that clones files"))
struct SnapshotCrashTests {
    /// What a person has on disk of a guest, for comparing before and after.
    private struct Live: Equatable {
        var data: String
        var record: Data
        var settings: Data
    }

    private func live(_ fixture: SnapshotFixture) throws -> Live {
        Live(
            data: TestDisk.fingerprint(fixture.data), record: try Data(contentsOf: fixture.files.state),
            settings: try Data(contentsOf: fixture.storage.locations.settings))
    }

    private func leftovers(_ fixture: SnapshotFixture) -> [String] {
        fixture.names().filter { $0.contains(".new-") || $0.contains(".gone-") || $0.hasSuffix(".tmp") }
    }

    // MARK: Create

    @Test(arguments: createSteps)
    func aCreateStoppedAfterAStepIsWholeOrNothingAfterTheNextLaunch(step: SnapshotStep) async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let before = try live(fixture)
        var storage = fixture.storage
        storage.snapshotSeam.afterStep = stopping(after: step)
        #expect(throws: SnapshotSeamStop.self) { try storage.createSnapshot(named: "crash") }
        // Whatever the stop left, no reader sees a half-made snapshot.
        #expect(fixture.storage.snapshotListing().items.count == (step == .published ? 1 : 0))

        try fixture.storage.recoverSnapshots()
        let listing = fixture.storage.snapshotListing()
        if step == .published {
            let only = try #require(listing.items.first)
            #expect(listing.items.count == 1 && only.name == "crash" && !only.isDamaged && only.restore == .asIs)
        } else {
            #expect(listing.items.isEmpty)
        }
        #expect(leftovers(fixture).isEmpty)
        #expect(try live(fixture) == before)
        // The next create is as if nothing had happened.
        let next = try fixture.storage.createSnapshot(named: "after")
        #expect(fixture.storage.snapshotListing().items.contains { $0.id == next.id })
    }

    @Test func aFailureThatIsNotACrashRemovesItsStagingAtOnce() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        // A destination that cannot be made: a file where the staging folder's parent should be.
        try Data("x".utf8).write(to: fixture.snapshots)
        #expect(throws: (any Error).self) { try fixture.storage.createSnapshot(named: "") }
        #expect(leftovers(fixture).isEmpty)
    }

    // MARK: Delete

    @Test(arguments: deleteSteps)
    func aDeleteStoppedAfterAStepIsGoneAfterTheNextLaunch(step: SnapshotStep) async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        let kept = try fixture.storage.createSnapshot(named: "kept")
        let doomed = try fixture.storage.createSnapshot(named: "doomed")
        let before = try live(fixture)
        var storage = fixture.storage
        storage.snapshotSeam.afterStep = stopping(after: step)
        #expect(throws: SnapshotSeamStop.self) { try storage.deleteSnapshot(doomed.id) }
        // Set aside, it is not listed any more.
        #expect(fixture.storage.snapshotListing().items.map(\.id) == [kept.id])

        try fixture.storage.recoverSnapshots()
        #expect(fixture.storage.snapshotListing().items.map(\.id) == [kept.id])
        #expect(leftovers(fixture).isEmpty)
        #expect(try live(fixture) == before)
    }

    // MARK: Restore

    /// The state a restore replaces and what it must end in.
    private struct Scenario {
        var fixture: SnapshotFixture
        var app: TwoDiskStorage
        var snapshot: SnapshotID
        var record: Data

        static func make() async throws -> Scenario {
            // A disk of 8 GiB, snapshotted, then moved on: other data, an
            // update that began and did not finish, and settings that say 4 GiB.
            let fixture = try await SnapshotFixture.make(gib: 8, marker: 1)
            let record = try Data(contentsOf: fixture.files.state)
            let made = try fixture.storage.createSnapshot(named: "good")
            try TestDisk.write(fixture.data, at: 0, count: 1 << 20, value: 2)
            let app = fixture.app(on: snapshotNewer)
            try await app.prepareStart(fixture.guest)
            try TestDisk.write(fixture.data, at: 1 << 30, count: 1 << 20, value: 3)
            var choices = fixture.guest
            choices.capacityGiB = 4
            choices.pendingCapacityGiB = 6
            try app.save(choices)
            return Scenario(fixture: fixture, app: app, snapshot: made.id, record: record)
        }

        var guest: Guest {
            var choices = fixture.guest
            choices.capacityGiB = 4
            choices.pendingCapacityGiB = 6
            return choices
        }
    }

    private func expectRestored(_ scenario: Scenario, sourceLocation: SourceLocation = #_sourceLocation) throws {
        let fixture = scenario.fixture
        #expect(try TestDisk.read(fixture.data, at: 0) == [1], sourceLocation: sourceLocation)
        #expect(try TestDisk.read(fixture.data, at: 1 << 30) == [1], sourceLocation: sourceLocation)
        #expect(try Data(contentsOf: fixture.files.state) == scenario.record, sourceLocation: sourceLocation)
        #expect(!fixture.names().contains { $0.hasPrefix("data.img.pre-") }, sourceLocation: sourceLocation)
        #expect(!exists(fixture.snapshots.appendingPathComponent("restore.json")), sourceLocation: sourceLocation)
        #expect(leftovers(fixture).isEmpty, sourceLocation: sourceLocation)
        // The capacity is the snapshot's disk and the stale growth is gone.
        let settings = try #require(try scenario.app.load(), sourceLocation: sourceLocation)
        #expect(settings.capacityGiB == 8 && settings.pendingCapacityGiB == nil, sourceLocation: sourceLocation)
        // The snapshot is still whole and can be restored again.
        let items = scenario.app.snapshotListing().items
        #expect(items.map(\.id) == [scenario.snapshot] && items.first?.isDamaged == false, sourceLocation: sourceLocation)
        #expect(scenario.app.startPlan() == .update(from: "20261031-los24-r4", to: "20261102-los24-r5"), sourceLocation: sourceLocation)
    }

    @Test func anUninterruptedRestoreReachesTheFinalState() async throws {
        let scenario = try await Scenario.make()
        defer { scenario.fixture.cleanUp() }
        _ = try scenario.app.restoreSnapshot(scenario.snapshot, guest: scenario.guest)
        try expectRestored(scenario)
    }

    @Test(arguments: restoreSteps)
    func aRestoreStoppedAfterAStepIsCarriedThroughByTheNextLaunch(step: SnapshotStep) async throws {
        var scenario = try await Scenario.make()
        defer { scenario.fixture.cleanUp() }
        scenario.app.snapshotSeam.afterStep = stopping(after: step)
        #expect(throws: SnapshotSeamStop.self) { try scenario.app.restoreSnapshot(scenario.snapshot, guest: scenario.guest) }
        // A restore began: it is the one thing that may be under way.
        if step != .finishedRestore {
            #expect(scenario.fixture.storage.snapshotListing().createRefusal == .restoreUnfinished)
        }

        scenario.app.snapshotSeam.afterStep = nil
        try scenario.app.recoverSnapshots()
        try expectRestored(scenario)
        // Launching again changes nothing.
        try scenario.app.recoverSnapshots()
        try expectRestored(scenario)
    }

    /// A crash during the recovery itself: the steps run again from the top.
    @Test(arguments: [SnapshotStep.recordedIntent, .replacedRecord, .settledSettings],
        [SnapshotStep.replacedData, .replacedRecord, .removedUpdateCopies, .settledSettings])
    func aCrashInTheRecoveryIsRecoveredAgain(first: SnapshotStep, second: SnapshotStep) async throws {
        var scenario = try await Scenario.make()
        defer { scenario.fixture.cleanUp() }
        scenario.app.snapshotSeam.afterStep = stopping(after: first)
        #expect(throws: SnapshotSeamStop.self) { try scenario.app.restoreSnapshot(scenario.snapshot, guest: scenario.guest) }

        scenario.app.snapshotSeam.afterStep = stopping(after: second)
        // The first stop may already have passed the second's step: nothing then stops the recovery.
        _ = try? scenario.app.recoverSnapshots()
        scenario.app.snapshotSeam.afterStep = nil
        try scenario.app.recoverSnapshots()
        try expectRestored(scenario)
    }

    @Test func aRestoreThatFailsBeforeTheDataIsReplacedLeavesNothingToFinish() async throws {
        let scenario = try await Scenario.make()
        defer { scenario.fixture.cleanUp() }
        // The clone cannot be made: its source is missing the data.
        let data = scenario.fixture.snapshots.appendingPathComponent(scenario.snapshot.rawValue).appendingPathComponent("data.img")
        try FileManager.default.removeItem(at: data)
        #expect(throws: SnapshotRefusal.incomplete) { try scenario.app.restoreSnapshot(scenario.snapshot, guest: scenario.guest) }
        #expect(!exists(scenario.fixture.snapshots.appendingPathComponent("restore.json")))
        #expect(try TestDisk.read(scenario.fixture.data, at: 0) == [2])
    }
}
