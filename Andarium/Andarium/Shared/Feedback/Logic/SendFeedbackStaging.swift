// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import Foundation

/// Where the files of a draft wait: one folder of the temporary directory per
/// open sheet, so they are never in Documents (the harness's) or a backup. The
/// sheet removes its folder when it closes and when the mail has been sent, and
/// a launch removes what an ended process left (`sweep`).
nonisolated struct SendFeedbackStaging: Sendable {
    let root: URL

    init(root: URL = FileManager.default.temporaryDirectory.appendingPathComponent("Feedback", isDirectory: true)) {
        self.root = root
    }

    /// Where the system's picker leaves a copy of a file until the sheet takes
    /// it into its folder.
    var incoming: URL { root.appendingPathComponent("incoming", isDirectory: true) }

    /// Removes everything under the root.
    func sweep() {
        try? FileManager.default.removeItem(at: root)
    }

    /// A folder for one sheet; made when its first file arrives.
    func makeFolder() -> SendFeedbackFolder {
        SendFeedbackFolder(url: root.appendingPathComponent(UUID().uuidString, isDirectory: true))
    }
}

/// One sheet's folder of staged files.
nonisolated final class SendFeedbackFolder: @unchecked Sendable {
    let url: URL
    private let lock = NSLock()

    init(url: URL) { self.url = url }

    /// A path in the folder for a file called `name`, made unique and with the
    /// folder in place; nothing is written yet.
    func reserve(_ name: String) throws -> URL {
        try lock.withLock {
            try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
            let safe = Self.safeName(name)
            let base = (safe as NSString).deletingPathExtension
            let ext = (safe as NSString).pathExtension
            var candidate = url.appendingPathComponent(safe)
            var number = 2
            while FileManager.default.fileExists(atPath: candidate.path) {
                candidate = url.appendingPathComponent(ext.isEmpty ? "\(base)-\(number)" : "\(base)-\(number).\(ext)")
                number += 1
            }
            return candidate
        }
    }

    /// Moves `file` (on the same volume) into the folder under `name`.
    func adopt(_ file: URL, named name: String) throws -> URL {
        let target = try reserve(name)
        try FileManager.default.moveItem(at: file, to: target)
        return target
    }

    func remove(_ file: URL) {
        try? FileManager.default.removeItem(at: file)
    }

    /// Removes the folder and what is in it.
    func removeAll() {
        lock.withLock { try? FileManager.default.removeItem(at: url) }
    }

    var exists: Bool { FileManager.default.fileExists(atPath: url.path) }

    /// A file name safe in a folder and in a mail: the characters of a plain
    /// name, at most 100 of them.
    static func safeName(_ name: String) -> String {
        var text = String(name.unicodeScalars.map { scalar -> Character in
            switch scalar {
            case "a"..."z", "A"..."Z", "0"..."9", ".", "-", "_": Character(scalar)
            default: "_"
            }
        })
        while text.hasPrefix(".") { text.removeFirst() }
        return text.isEmpty ? "file" : String(text.prefix(100))
    }
}

#endif
