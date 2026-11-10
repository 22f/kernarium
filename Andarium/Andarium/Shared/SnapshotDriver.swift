// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// What the screens call to work with the snapshots of the guest's data disk
/// (`Andarium/README.md#model-and-storage`). The runtime's driver acts on the
/// guest's files; the preview's plays mock snapshots. Nothing here is words:
/// a refusal is a `SnapshotRefusal`, and each screen says it in its own.
@MainActor
protocol SnapshotDriver: AnyObject {
    /// Why Create, Restore and Delete cannot run now, or nil when the guest
    /// is stopped and nothing else is under way: Home with no run active,
    /// pending or launching. Rename and the list do not depend on it. Every
    /// operation checks it again and refuses with it.
    var snapshotGate: SnapshotRefusal? { get }
    /// The snapshots newest first, each with what deleting it frees and what
    /// it would take alone, the space they use together and what Create would
    /// say. The figures are measured off the main actor; a listing waits for
    /// an operation under way, so it never reads one half done.
    func snapshotListing() async -> SnapshotListing
    /// A snapshot of the data as it is. Allowed only while the guest is
    /// stopped and after Android's own shut down.
    func createSnapshot(named name: String) async -> SnapshotResult<SnapshotSummary>
    /// Puts the snapshot's data back in place of the guest's. `guest` is the
    /// guest as the screens hold it; the result is the guest with the capacity
    /// the restored disk needs. The state it replaces is not kept.
    func restoreSnapshot(_ id: SnapshotID, guest: Guest) async -> SnapshotResult<Guest>
    /// Deletes a snapshot, which frees what it alone held.
    func deleteSnapshot(_ id: SnapshotID) async -> SnapshotResult<Void>
    /// Renames a snapshot; allowed at any time, after an operation under way.
    func renameSnapshot(_ id: SnapshotID, to name: String) async -> SnapshotResult<Void>
}

/// The screens' side of the snapshots: the driver's operations with what the
/// model must do after each. With no driver (a model that plays nothing),
/// every operation refuses with `noData`.
extension AppModel {
    func snapshotListing() async -> SnapshotListing {
        guard let snapshotDriver else { return SnapshotListing(createRefusal: .noData, whileNotStopped: .noData) }
        return await snapshotDriver.snapshotListing()
    }

    /// Why Create, Restore and Delete cannot run now (`SnapshotDriver.snapshotGate`).
    var snapshotGate: SnapshotRefusal? {
        snapshotDriver?.snapshotGate ?? .noData
    }

    func takeSnapshot(named name: String) async -> SnapshotResult<SnapshotSummary> {
        guard let snapshotDriver else { return .refused(.noData) }
        return await snapshotDriver.createSnapshot(named: name)
    }

    /// Restores the snapshot and adopts what changed: the guest's capacity and
    /// its used space, and what a start would now be (an older snapshot
    /// updates at the next start).
    func restoreSnapshot(_ id: SnapshotID) async -> SnapshotResult<Void> {
        guard let snapshotDriver, let guest else { return .refused(.noData) }
        // The choices on disk are the ones the restore settles; a change
        // made a moment ago is not lost.
        persistNow()
        let result = await snapshotDriver.restoreSnapshot(id, guest: guest)
        switch result {
        case .done(let restored):
            lifecycle.guest = restored
            refreshPlan()
            return .done(())
        case .refused(let refusal):
            return .refused(refusal)
        case .failed(let error):
            // A failure after the data was replaced leaves the files for
            // the next start or launch to finish.
            refreshPlan()
            return .failed(error)
        }
    }

    func deleteSnapshot(_ id: SnapshotID) async -> SnapshotResult<Void> {
        guard let snapshotDriver else { return .refused(.noData) }
        return await snapshotDriver.deleteSnapshot(id)
    }

    func renameSnapshot(_ id: SnapshotID, to name: String) async -> SnapshotResult<Void> {
        guard let snapshotDriver else { return .refused(.noData) }
        return await snapshotDriver.renameSnapshot(id, to: name)
    }
}
