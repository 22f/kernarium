// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Reads that cannot take more memory than they are given, for the files the
/// application keeps of a session.
enum BoundedFile {
    /// The first `limit + 1` bytes of the file at `url`, or fewer when it is
    /// shorter, so that a caller can tell a file of exactly `limit` bytes from
    /// a longer one; nil when it cannot be opened.
    static func read(_ url: URL, limit: Int) -> Data? {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? handle.close() }
        return (try? handle.read(upToCount: limit + 1)) ?? Data()
    }

    /// The first `limit` bytes of the file as text; a cut that falls inside a
    /// character, and any byte that is not text, becomes U+FFFD.
    static func text(_ url: URL, limit: Int) -> String? {
        read(url, limit: limit).map { String(decoding: $0.prefix(limit), as: UTF8.self) }
    }
}
