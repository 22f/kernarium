// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// The file calls the snapshot store makes, as values, so that a test can have
/// any of them fail with a real error and can look at the guest's files after
/// each one (`SnapshotStore`). The application uses `live`. A call that reads
/// (a record, the size of a file) is not here: only what changes files, and
/// the listing of a directory.
struct SnapshotFileOperations: Sendable {
    var createDirectory: @Sendable (_ directory: URL) throws -> Void
    /// An APFS clone of `source` at the absent `destination`
    /// (`CloneFile.Failure` carries the errno).
    var clone: @Sendable (_ source: URL, _ destination: URL) throws -> Void
    var rename: @Sendable (_ source: URL, _ destination: URL) throws -> Void
    /// Replaces the file's contents atomically; not yet flushed.
    var write: @Sendable (_ data: Data, _ file: URL) throws -> Void
    /// Makes a file's contents, or a directory's entries, survive a power loss.
    var flush: @Sendable (_ url: URL) throws -> Void
    var remove: @Sendable (_ url: URL) throws -> Void
    var list: @Sendable (_ directory: URL) throws -> [String]
    var measure = SnapshotMeasure.Primitives.live

    static let live = SnapshotFileOperations(
        createDirectory: { try FileManager.default.createDirectory(at: $0, withIntermediateDirectories: true) },
        clone: { try CloneFile.clone($0, to: $1) },
        rename: { source, destination in
            guard Darwin.rename(source.path, destination.path) == 0 else {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
        },
        write: { try $0.write(to: $1, options: .atomic) },
        flush: { url in
            let descriptor = open(url.path, O_RDONLY)
            guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
            defer { close(descriptor) }
            // `F_FULLFSYNC` where the volume has it, else `fsync`.
            if fcntl(descriptor, F_FULLFSYNC) != 0, fsync(descriptor) != 0 {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
        },
        remove: { try FileManager.default.removeItem(at: $0) },
        list: { try FileManager.default.contentsOfDirectory(atPath: $0.path) }
    )
}
