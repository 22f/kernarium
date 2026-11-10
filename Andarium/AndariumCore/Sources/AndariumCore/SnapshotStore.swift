// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// A point in an operation after which a crash leaves a state the next launch
/// completes or discards. `SnapshotSeam` can stop an operation after any of
/// them, which is how the tests prove it.
enum SnapshotStep: Equatable, Sendable, CaseIterable {
    // Create
    case createdStaging, clonedData, copiedRecord, wroteMetadata, published
    // Delete
    case setAside, removed
    // Restore
    case recordedIntent, replacedData, replacedRecord, removedUpdateCopies, settledSettings, finishedRestore
}

/// What a test replaces: a stop after a step, and the measuring calls.
struct SnapshotSeam: Sendable {
    /// Called after each step; a throw stops the operation there as a crash
    /// would, leaving every file as it is.
    var afterStep: (@Sendable (SnapshotStep) throws -> Void)?
    var measure = SnapshotMeasure.Primitives.live
}

/// The error an operation throws when a seam stopped it.
struct SnapshotSeamStop: Error {
    var step: SnapshotStep
}

/// Files that survive a power loss, which Foundation's atomic write alone does
/// not make them: each commit point flushes the file and its directory.
enum DurableFile {
    static func write(_ data: Data, to url: URL) throws {
        try data.write(to: url, options: .atomic)
        try flush(url)
        flushDirectory(url.deletingLastPathComponent())
    }

    /// `F_FULLFSYNC` where the volume has it, else `fsync`.
    static func flush(_ url: URL) throws {
        let descriptor = open(url.path, O_RDONLY)
        guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        defer { close(descriptor) }
        if fcntl(descriptor, F_FULLFSYNC) != 0, fsync(descriptor) != 0 {
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
    }

    static func flushDirectory(_ url: URL) {
        try? flush(url)
    }
}

/// A snapshot as the store finds it on disk, with what could be read.
struct SnapshotEntry: Sendable {
    var id: SnapshotID
    var record: SnapshotRecord?
    /// The copy of `instance.json`, as bytes and as the record it holds.
    var instanceBytes: Data?
    var state: InstanceState?
    var hasData: Bool

    var isComplete: Bool { record != nil && state != nil && hasData }
}

/// `restore.json`: a restore that has begun and finishes at the next launch
/// if the process ends first.
struct RestoreIntent: Codable, Equatable {
    var schema = 1
    var snapshot: SnapshotID
}

/// The user's snapshots of a guest's data disk, in `Guest/Snapshots/`:
///
/// ```text
/// Snapshots/
///   <uuid>/data.img  instance.json  snapshot.json
///   .new-<uuid>/  .gone-<uuid>/     a create or a delete that did not finish
///   restore.json                    a restore that did not finish
/// ```
///
/// Each operation is a sequence of steps that leave a state `recover` completes
/// or discards, so nothing reads a half-made snapshot. The store never looks
/// at the guest's choices, which `TwoDiskStorage` settles through the
/// closure of a restore, and it does not know whether the guest runs: the
/// application's state decides that (`SnapshotRules.gate`).
struct SnapshotStore: Sendable {
    static let directoryName = "Snapshots"
    static let restoreFile = "restore.json"
    static let metadataFile = "snapshot.json"
    static let stagingPrefix = ".new-"
    static let removalPrefix = ".gone-"

    var locations: GuestLocations
    var release: ReleaseIdentity
    var seam = SnapshotSeam()

    private var files: InstanceFiles { InstanceFiles(directory: locations.directory) }
    var root: URL { files.snapshots }
    private var restoreURL: URL { root.appendingPathComponent(Self.restoreFile) }

    func directory(_ id: SnapshotID) -> URL {
        root.appendingPathComponent(id.rawValue, isDirectory: true)
    }

    private func step(_ step: SnapshotStep) throws {
        do { try seam.afterStep?(step) } catch { throw SnapshotSeamStop(step: step) }
    }

    /// Keeps `url` out of iCloud and computer backups.
    static func excludeFromBackup(_ url: URL) {
        var target = url
        var excluded = URLResourceValues()
        excluded.isExcludedFromBackup = true
        try? target.setResourceValues(excluded)
    }

    /// An error that is a refusal as a value: no room, or a volume that cannot
    /// clone. Any other error stays what it is.
    private func translated(_ error: Error) -> Error {
        if let refusal = refusal(for: error) { return refusal }
        return error
    }

    func refusal(for error: Error) -> SnapshotRefusal? {
        if let failure = error as? CloneFile.Failure {
            switch failure.code {
            case ENOTSUP: return .volumeCannotClone
            case ENOSPC, EDQUOT: return .notEnoughSpace
            default: return nil
            }
        }
        if let cocoa = error as? CocoaError, cocoa.code == .fileWriteOutOfSpace { return .notEnoughSpace }
        if let posix = error as? POSIXError, posix.code == .ENOSPC || posix.code == .EDQUOT { return .notEnoughSpace }
        return nil
    }

    // MARK: Reading

    func entry(_ id: SnapshotID) -> SnapshotEntry? {
        let manager = FileManager.default
        let folder = directory(id)
        var isDirectory: ObjCBool = false
        guard manager.fileExists(atPath: folder.path, isDirectory: &isDirectory), isDirectory.boolValue else { return nil }
        let record = (try? Data(contentsOf: folder.appendingPathComponent(Self.metadataFile)))
            .flatMap { try? JSONDecoder().decode(SnapshotRecord.self, from: $0) }
        let bytes = try? Data(contentsOf: folder.appendingPathComponent(InstanceFiles.stateFile))
        let state = bytes.flatMap { try? JSONDecoder().decode(InstanceState.self, from: $0) }
        let data = manager.fileExists(atPath: folder.appendingPathComponent(InstanceFiles.dataDisk).path)
        return SnapshotEntry(id: id, record: record, instanceBytes: bytes, state: state, hasData: data)
    }

    /// Every snapshot, complete or not, newest first; a damaged one (no
    /// readable record) is last.
    func entries() -> [SnapshotEntry] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? []
        let found = names.compactMap { name in SnapshotID(name).flatMap { entry($0) } }
        return found.sorted {
            switch ($0.record?.created, $1.record?.created) {
            case let (a?, b?): a != b ? a > b : $0.id.rawValue < $1.id.rawValue
            case (nil, nil): $0.id.rawValue < $1.id.rawValue
            case (_?, nil): true
            case (nil, _?): false
            }
        }
    }

    /// The data disk of a snapshot, a file for the measures.
    func dataURL(_ id: SnapshotID) -> URL {
        directory(id).appendingPathComponent(InstanceFiles.dataDisk)
    }

    /// The small files of a snapshot.
    func smallFiles(_ id: SnapshotID) -> [URL] {
        [InstanceFiles.stateFile, Self.metadataFile].map { directory(id).appendingPathComponent($0) }
    }

    private enum Intent {
        case none
        case readable(SnapshotID)
        case unreadable
    }

    private func intent() -> Intent {
        guard FileManager.default.fileExists(atPath: restoreURL.path) else { return .none }
        guard let data = try? Data(contentsOf: restoreURL),
            let intent = try? JSONDecoder().decode(RestoreIntent.self, from: data), intent.schema == 1
        else { return .unreadable }
        return .readable(intent.snapshot)
    }

    /// Whether a restore began and did not finish.
    var restoreUnderWay: Bool {
        if case .none = intent() { false } else { true }
    }

    // MARK: Create

    private struct CreateInputs {
        var instanceBytes: Data
        var diskBytes: Int64
    }

    /// What Create needs from the guest's files, or the reason it refuses.
    private func checkCreate() throws -> CreateInputs {
        if restoreUnderWay { throw SnapshotRefusal.restoreUnfinished }
        var details = stat()
        let dataExists = lstat(files.data.path, &details) == 0 && (details.st_mode & mode_t(S_IFMT)) == mode_t(S_IFREG)
        var state: InstanceState?
        var bytes: Data?
        if FileManager.default.fileExists(atPath: files.state.path) {
            guard let read = try? Data(contentsOf: files.state),
                let decoded = try? JSONDecoder().decode(InstanceState.self, from: read)
            else { throw SnapshotRefusal.unreadableRecord }
            bytes = read
            state = decoded
        }
        if let refusal = SnapshotRules.createRefusal(state: state, dataDiskExists: dataExists, release: release) {
            throw refusal
        }
        guard let bytes else { throw SnapshotRefusal.mismatch }
        return CreateInputs(instanceBytes: bytes, diskBytes: Int64(details.st_size))
    }

    /// Why Create refuses on the guest's files now, or nil.
    func createRefusal() -> SnapshotRefusal? {
        do {
            _ = try checkCreate()
            return nil
        } catch let refusal as SnapshotRefusal {
            return refusal
        } catch {
            return .unreadableRecord
        }
    }

    /// A snapshot of the data as it is: a clone of `data.img` and a copy of
    /// `instance.json`, in a directory that takes its name only when it is
    /// whole.
    @discardableResult
    func create(name rawName: String, now: Date = Date()) throws -> SnapshotID {
        guard let name = SnapshotRules.normalizedName(rawName) else { throw SnapshotRefusal.invalidName }
        let inputs = try checkCreate()
        let state = try JSONDecoder().decode(InstanceState.self, from: inputs.instanceBytes)
        let id = SnapshotID()
        let staging = root.appendingPathComponent(Self.stagingPrefix + id.rawValue, isDirectory: true)
        let manager = FileManager.default
        let gib = Int64(Limits.bytesPerGiB)
        let record = SnapshotRecord(
            name: name, created: now, releaseID: state.releaseID, buildIncremental: state.buildIncremental,
            diskGiB: Int((inputs.diskBytes + gib - 1) / gib)
        )
        do {
            let rootIsNew = !manager.fileExists(atPath: root.path)
            try manager.createDirectory(at: staging, withIntermediateDirectories: true)
            Self.excludeFromBackup(root)
            Self.excludeFromBackup(staging)
            if rootIsNew { DurableFile.flushDirectory(locations.directory) }
            try step(.createdStaging)

            let clone = staging.appendingPathComponent(InstanceFiles.dataDisk)
            try CloneFile.clone(files.data, to: clone)
            Self.excludeFromBackup(clone)
            try DurableFile.flush(clone)
            try step(.clonedData)

            let copy = staging.appendingPathComponent(InstanceFiles.stateFile)
            try DurableFile.write(inputs.instanceBytes, to: copy)
            Self.excludeFromBackup(copy)
            try step(.copiedRecord)

            let metadata = staging.appendingPathComponent(Self.metadataFile)
            try DurableFile.write(Self.encoded(record), to: metadata)
            Self.excludeFromBackup(metadata)
            DurableFile.flushDirectory(staging)
            try step(.wroteMetadata)

            guard Darwin.rename(staging.path, directory(id).path) == 0 else {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
            DurableFile.flushDirectory(root)
            try step(.published)
        } catch let stop as SnapshotSeamStop {
            throw stop
        } catch {
            try? manager.removeItem(at: staging)
            throw translated(error)
        }
        return id
    }

    static func encoded(_ record: SnapshotRecord) throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        return try encoder.encode(record)
    }

    // MARK: Delete and rename

    func delete(_ id: SnapshotID) throws {
        if restoreUnderWay { throw SnapshotRefusal.restoreUnfinished }
        let manager = FileManager.default
        guard manager.fileExists(atPath: directory(id).path) else { throw SnapshotRefusal.missing }
        let gone = root.appendingPathComponent(Self.removalPrefix + id.rawValue, isDirectory: true)
        try? manager.removeItem(at: gone)
        guard Darwin.rename(directory(id).path, gone.path) == 0 else {
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
        DurableFile.flushDirectory(root)
        try step(.setAside)
        try manager.removeItem(at: gone)
        try step(.removed)
    }

    func rename(_ id: SnapshotID, to rawName: String) throws {
        guard let name = SnapshotRules.normalizedName(rawName) else { throw SnapshotRefusal.invalidName }
        guard let found = entry(id) else { throw SnapshotRefusal.missing }
        guard var record = found.record else { throw SnapshotRefusal.incomplete }
        record.name = name
        let metadata = directory(id).appendingPathComponent(Self.metadataFile)
        do {
            try DurableFile.write(Self.encoded(record), to: metadata)
        } catch {
            throw translated(error)
        }
        Self.excludeFromBackup(metadata)
    }

    // MARK: Restore

    /// Puts the snapshot's data and record in place of the guest's. `settle`
    /// is the step that touches the guest's choices (the capacity, the
    /// backup policy); it runs once the files are in place and may run again
    /// after a crash. The snapshot stays, and the state it replaces is gone.
    func restore(_ id: SnapshotID, settle: (SnapshotRecord) throws -> Void) throws {
        if restoreUnderWay { throw SnapshotRefusal.restoreUnfinished }
        guard let found = entry(id) else { throw SnapshotRefusal.missing }
        guard found.isComplete, let state = found.state, let record = found.record else { throw SnapshotRefusal.incomplete }
        if case .refused(let reason) = SnapshotRules.restorePlan(of: state, diskGiB: record.diskGiB, release: release) {
            throw reason
        }
        try DurableFile.write(JSONEncoder().encode(RestoreIntent(snapshot: id)), to: restoreURL)
        Self.excludeFromBackup(restoreURL)
        try step(.recordedIntent)
        do {
            try replaceData(of: id)
        } catch let stop as SnapshotSeamStop {
            throw stop
        } catch {
            // Nothing has changed until the clone is in place: forget the
            // restore rather than leave one to finish at the next launch.
            try? FileManager.default.removeItem(at: restoreURL)
            throw translated(error)
        }
        try step(.replacedData)
        try finishRestore(found, settle: settle)
    }

    /// Step 2: the clone of the snapshot's data, renamed over `data.img`.
    private func replaceData(of id: SnapshotID) throws {
        try InstanceFiles.clone(dataURL(id), to: files.data)
        try DurableFile.flush(files.data)
        DurableFile.flushDirectory(locations.directory)
    }

    /// Steps 3 to 6, each of which can run again with the same result while
    /// the snapshot is whole, because it is only read.
    private func finishRestore(_ found: SnapshotEntry, settle: (SnapshotRecord) throws -> Void) throws {
        guard let bytes = found.instanceBytes, let record = found.record else { throw SnapshotRefusal.incomplete }
        try DurableFile.write(bytes, to: files.state)
        try step(.replacedRecord)
        // The replaced state's update copy has no record naming it now.
        let manager = FileManager.default
        for name in (try? manager.contentsOfDirectory(atPath: locations.directory.path)) ?? []
        where name.hasPrefix(InstanceFiles.snapshotPrefix) {
            try manager.removeItem(at: locations.directory.appendingPathComponent(name))
        }
        try step(.removedUpdateCopies)
        try settle(record)
        try step(.settledSettings)
        try manager.removeItem(at: restoreURL)
        DurableFile.flushDirectory(root)
        try step(.finishedRestore)
    }

    // MARK: Launch

    /// Completes what a crash left, before the guest is read: a restore that
    /// began is carried through (the snapshot is intact, so every step can run
    /// again), a clone's temporary file is dropped, and a create or delete
    /// that did not finish is removed. A restore whose snapshot is gone or
    /// damaged cannot be carried on and is forgotten.
    func recover(settle: (SnapshotRecord) throws -> Void) throws {
        let manager = FileManager.default
        try? manager.removeItem(at: files.data.appendingPathExtension("tmp"))
        switch intent() {
        case .none:
            break
        case .unreadable:
            try? manager.removeItem(at: restoreURL)
        case .readable(let id):
            if let found = entry(id), found.isComplete {
                try replaceData(of: id)
                try finishRestore(found, settle: settle)
            } else {
                try? manager.removeItem(at: restoreURL)
            }
        }
        for name in (try? manager.contentsOfDirectory(atPath: root.path)) ?? []
        where name.hasPrefix(Self.stagingPrefix) || name.hasPrefix(Self.removalPrefix) {
            try? manager.removeItem(at: root.appendingPathComponent(name))
        }
    }
}
