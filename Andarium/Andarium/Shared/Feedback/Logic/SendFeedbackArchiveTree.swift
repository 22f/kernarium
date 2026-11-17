// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import Foundation

// The names of an archive are paths, and the viewer shows them as folders and files, as Files shows a folder.
// The names are data the archive carries: they group entries and are shown, and no path of the file system is
// ever made from one.

/// The folders and files of a listing, level by level.
nonisolated struct SendFeedbackArchiveTree: Sendable {
    struct Folder: Hashable, Sendable {
        /// Where the folder is: its names joined by "/", the key to ask `level(at:)` for what is inside.
        let path: String
        /// What the row says: the names from the level it is in to here, joined by "/" (a folder with nothing but
        /// one folder in it is shown as one row with that one inside).
        let label: String
        /// The files in it and in every folder under it.
        let files: Int
        let bytes: UInt64
    }

    struct File: Hashable, Sendable {
        let entry: SendFeedbackArchiveEntry
        /// The last name of the path.
        let label: String
    }

    struct Level: Equatable, Sendable {
        var folders: [Folder]
        var files: [File]
    }

    private struct Node {
        var folders: [String] = []
        var files: [Int] = []
        /// The files in it and in every folder under it.
        var fileCount = 0
        var bytes: UInt64 = 0
    }

    private var nodes: [String: Node] = ["": Node()]
    private let entries: [SendFeedbackArchiveEntry]

    init(entries: [SendFeedbackArchiveEntry]) {
        self.entries = entries
        for (index, entry) in entries.enumerated() {
            let names = entry.path.split(separator: "/", omittingEmptySubsequences: true).map(String.init)
            if entry.isFolder {
                _ = makeChain(names)
                continue
            }
            let parent = makeChain(names.dropLast())
            nodes[parent.last!]!.files.append(index)
            // Every folder above it counts it.
            for path in parent {
                nodes[path]!.fileCount += 1
                nodes[path]!.bytes += entry.size
            }
        }
    }

    /// The nodes from the top (the empty path) down to the folder the names make, made where they are not yet.
    private mutating func makeChain(_ names: some Collection<String>) -> [String] {
        var path = ""
        var chain = [""]
        for name in names {
            let child = path.isEmpty ? name : path + "/" + name
            if nodes[child] == nil {
                nodes[child] = Node()
                nodes[path]!.folders.append(name)
            }
            path = child
            chain.append(path)
        }
        return chain
    }

    /// What is directly in the folder at `path` ("" is the top): the folders first, then the files, each by name.
    func level(at path: String) -> Level {
        guard let node = nodes[path] else { return Level(folders: [], files: []) }
        let folders = node.folders.map { name -> Folder in
            var at = path.isEmpty ? name : path + "/" + name
            var label = SendFeedbackArchiveNames.shortened(name)
            while let inner = nodes[at], inner.files.isEmpty, inner.folders.count == 1 {
                at += "/" + inner.folders[0]
                label += "/" + SendFeedbackArchiveNames.shortened(inner.folders[0])
            }
            let found = nodes[at]!
            return Folder(path: at, label: label, files: found.fileCount, bytes: found.bytes)
        }
        .sorted { $0.label.localizedStandardCompare($1.label) == .orderedAscending }
        let files = node.files.map { index -> File in
            let entry = entries[index]
            let last = entry.path.split(separator: "/", omittingEmptySubsequences: true).last.map(String.init) ?? entry.path
            return File(entry: entry, label: SendFeedbackArchiveNames.shortened(last))
        }
        .sorted {
            let order = $0.label.localizedStandardCompare($1.label)
            return order == .orderedSame ? $0.entry.id < $1.entry.id : order == .orderedAscending
        }
        return Level(folders: folders, files: files)
    }
}

#endif
