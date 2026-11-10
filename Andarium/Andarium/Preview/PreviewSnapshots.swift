// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// What a preview's snapshots start as: the list, the figures around it, and
/// the state that gates the operations, so a screen's every state can be set
/// up from a launch argument or a test (`PreviewSnapshotDriver`).
struct PreviewSnapshotSeed {
    var items: [SnapshotSummary] = []
    /// What an unfinished update's own copy holds; nil when none is.
    var updateCopy: SnapshotFigure?
    /// The space the snapshots use is shown as a lower bound, as where the
    /// extent map is refused.
    var useIsLowerBound = false
    /// Why Create refuses on the guest's files (`SnapshotListing.createRefusal`).
    var createRefusal: SnapshotRefusal?
    /// Why Create, Restore and Delete cannot run now, as with a guest that
    /// runs (`SnapshotDriver.snapshotGate`).
    var gate: SnapshotRefusal?
    /// How long an operation takes, which a capture of its overlay needs.
    var latency: Duration = .milliseconds(400)
    /// The failure the next operation ends with, once.
    var failure: (any Error)?
}

extension PreviewSnapshotSeed {
    private static func gibibytes(_ count: Double) -> UInt64 { UInt64(count * Double(Limits.bytesPerGiB)) }

    /// A guest with three snapshots: one of the Android before the current one
    /// (restoring it updates at the next start), the current one's, unnamed,
    /// and one of a newer Android that this build cannot restore.
    static func sample(now: Date = Date()) -> PreviewSnapshotSeed {
        func item(
            _ name: String, daysAgo: Double, release: String, build: Int, frees: Double, kept: Double,
            restore: SnapshotRestorePlan
        ) -> SnapshotSummary {
            SnapshotSummary(
                id: SnapshotID(), name: name, created: now.addingTimeInterval(-daysAgo * 86400), releaseID: release,
                buildNumber: build, diskGiB: 16, restore: restore, kept: SnapshotFigure(bytes: gibibytes(kept)),
                freesWhenDeleted: SnapshotFigure(bytes: gibibytes(frees)))
        }
        return PreviewSnapshotSeed(
            items: [
                item("", daysAgo: 0.1, release: PreviewData.systemRelease, build: 1_793_463_800, frees: 0.2, kept: 5.4,
                    restore: .asIs),
                item("Before the update", daysAgo: 2, release: PreviewData.previousSystemRelease, build: 1_793_400_000,
                    frees: 1.4, kept: 5.1,
                    restore: .updatesAtNextStart(from: PreviewData.previousSystemRelease, to: PreviewData.systemRelease)),
                item("From a newer app", daysAgo: 9, release: "20261201-los24-r30", build: 1_794_000_000, frees: 0.6,
                    kept: 4.8, restore: .refused(.newerAndroid)),
            ])
    }
}

/// The preview's snapshots: seeded ones that the operations change, with the
/// refusals the real driver gives and an optional delay. Nothing touches a file.
@MainActor
final class PreviewSnapshotDriver: SnapshotDriver {
    private(set) var seed: PreviewSnapshotSeed
    private var operationUnderWay = false

    init(_ seed: PreviewSnapshotSeed = PreviewSnapshotSeed()) {
        self.seed = seed
    }

    /// Changes the state that gates the operations, as the guest starting or
    /// stopping would.
    func setGate(_ refusal: SnapshotRefusal?) {
        seed.gate = refusal
    }

    var snapshotGate: SnapshotRefusal? {
        seed.gate ?? (operationUnderWay ? .busy : nil)
    }

    func snapshotListing() async -> SnapshotListing {
        let items = seed.items.sorted { $0.created > $1.created }
        let total = items.reduce(0) { $0 + $1.freesWhenDeleted.bytes }
        return SnapshotListing(
            items: items,
            space: SnapshotSpace(
                snapshotsUse: SnapshotFigure(bytes: total, isLowerBound: seed.useIsLowerBound), updateCopy: seed.updateCopy),
            createRefusal: seed.createRefusal, whileNotStopped: snapshotGate)
    }

    func createSnapshot(named name: String) async -> SnapshotResult<SnapshotSummary> {
        await perform(gated: true) {
            guard let name = SnapshotRules.normalizedName(name) else { return .refused(.invalidName) }
            if let refusal = self.seed.createRefusal { return .refused(refusal) }
            let kept = self.seed.items.map(\.kept.bytes).max() ?? 0
            let made = SnapshotSummary(
                id: SnapshotID(), name: name, created: Date(), releaseID: PreviewData.systemRelease,
                buildNumber: 1_793_463_800, diskGiB: 16, restore: .asIs, kept: SnapshotFigure(bytes: kept),
                freesWhenDeleted: SnapshotFigure(bytes: 16384))
            self.seed.items.append(made)
            return .done(made)
        }
    }

    func restoreSnapshot(_ id: SnapshotID, guest: Guest) async -> SnapshotResult<Guest> {
        await perform(gated: true) {
            guard let item = self.seed.items.first(where: { $0.id == id }) else { return .refused(.missing) }
            if case .refused(let reason) = item.restore { return .refused(reason) }
            var restored = guest
            restored.capacityGiB = max(guest.capacityGiB, item.diskGiB)
            restored.usedBytes = item.kept.bytes
            return .done(restored)
        }
    }

    func deleteSnapshot(_ id: SnapshotID) async -> SnapshotResult<Void> {
        await perform(gated: true) {
            guard self.seed.items.contains(where: { $0.id == id }) else { return .refused(.missing) }
            self.seed.items.removeAll { $0.id == id }
            return .done(())
        }
    }

    func renameSnapshot(_ id: SnapshotID, to name: String) async -> SnapshotResult<Void> {
        await perform(gated: false) {
            guard let name = SnapshotRules.normalizedName(name) else { return .refused(.invalidName) }
            guard let index = self.seed.items.firstIndex(where: { $0.id == id }) else { return .refused(.missing) }
            self.seed.items[index].name = name
            return .done(())
        }
    }

    /// Runs one operation: refused while the gate says so, after the seeded
    /// latency, or failed with the seeded failure.
    private func perform<Value: Sendable>(
        gated: Bool, _ body: @MainActor () -> SnapshotResult<Value>
    ) async -> SnapshotResult<Value> {
        if gated, let refusal = snapshotGate { return .refused(refusal) }
        operationUnderWay = true
        defer { operationUnderWay = false }
        try? await Task.sleep(for: seed.latency)
        if let failure = seed.failure {
            seed.failure = nil
            return .failed(failure)
        }
        return body()
    }
}
