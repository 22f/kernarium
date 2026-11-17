// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import Foundation

// What the viewer makes of the bytes of an archive: which entries are text,
// text in windows, and the readers that keep one open archive or entry each,
// off the main thread. Text is shown as plain text only: what is not valid
// UTF-8 and what a screen would obey (control and formatting characters, the
// ones that reverse text among them) becomes U+FFFD.

nonisolated enum SendFeedbackText {
    /// Plain text for the screen: invalid bytes were already replaced by the decoder; line ends become
    /// one kind, a tab stays, and any other control or formatting character becomes U+FFFD.
    static func sanitize(_ text: String) -> String {
        var result = String.UnicodeScalarView()
        var previousWasReturn = false
        for scalar in text.unicodeScalars {
            defer { previousWasReturn = scalar == "\r" }
            switch scalar {
            case "\n":
                if !previousWasReturn { result.append("\n") }
            case "\r":
                result.append("\n")
            case "\t":
                result.append(scalar)
            default:
                result.append(SendFeedbackNames.isControl(scalar) ? "\u{FFFD}" : scalar)
            }
        }
        return String(result)
    }

    /// The start of an entry looks like text: almost every byte is valid UTF-8 and almost none is a control.
    static func looksLikeText(_ head: Data) -> Bool {
        guard !head.isEmpty else { return true }
        var invalid = 0
        var controls = 0
        var total = 0
        for scalar in String(decoding: head, as: UTF8.self).unicodeScalars {
            total += 1
            if scalar == "\u{FFFD}" {
                invalid += 1
            } else if scalar.value < 0x20 && scalar != "\n" && scalar != "\r" && scalar != "\t" && scalar != "\u{0C}" {
                controls += 1
            }
        }
        return controls * 50 <= total && invalid * 20 <= total
    }

    /// The start of an entry is a zip: an archive inside the archive.
    static func looksLikeArchive(_ head: Data) -> Bool {
        let bytes = [UInt8](head.prefix(4))
        return bytes == [0x50, 0x4B, 0x03, 0x04] || bytes == [0x50, 0x4B, 0x05, 0x06]
    }

    /// A continuation byte of UTF-8: the middle or the end of a character, never its start.
    static func isContinuation(_ byte: UInt8) -> Bool { byte & 0xC0 == 0x80 }
}

/// One window of an entry's text: `index` of `count`, and what it says.
nonisolated struct SendFeedbackTextWindow: Equatable, Sendable {
    var index: Int
    var count: Int
    var text: String
}

/// Reads the text of one entry (or of a plain file) in windows of `windowBytes`.
/// A window ends at the first character boundary at or after its nominal end, and the next one starts
/// after it, so no character is cut, and a window is the same whichever way it was reached.
actor SendFeedbackTextReader {
    private let stream: SendFeedbackEntryStream
    private var cached: [Int: SendFeedbackTextWindow] = [:]
    private var cachedOrder: [Int] = []
    private static let cacheCount = 4

    nonisolated let byteCount: UInt64
    nonisolated let windowBytes: Int

    init(_ location: SendFeedbackEntryLocation, windowBytes: Int = SendFeedbackArchiveLimits.windowBytes) {
        stream = SendFeedbackEntryStream(location)
        byteCount = location.size
        self.windowBytes = windowBytes
    }

    /// A plain file, shown the same way (the start log).
    init?(file url: URL) {
        guard let source = SendFeedbackFileBytes(url: url), source.length > 0 else { return nil }
        self.init(SendFeedbackEntryLocation(plain: source))
    }

    nonisolated var windowCount: Int {
        max(1, Int((byteCount + UInt64(windowBytes) - 1) / UInt64(windowBytes)))
    }

    func window(_ index: Int) throws -> SendFeedbackTextWindow {
        let index = min(max(0, index), windowCount - 1)
        if let hit = cached[index] { return hit }
        let start = UInt64(index) * UInt64(windowBytes)
        // Up to three bytes more: the end of a character that begins inside the window.
        let data = try stream.read(at: start, count: windowBytes + 3)
        var lower = 0
        if index > 0 {
            while lower < min(3, data.count), SendFeedbackText.isContinuation(data[data.startIndex + lower]) { lower += 1 }
        }
        var upper = min(windowBytes, data.count)
        while upper < min(windowBytes + 3, data.count), SendFeedbackText.isContinuation(data[data.startIndex + upper]) {
            upper += 1
        }
        let slice = data.subdata(in: (data.startIndex + lower)..<(data.startIndex + max(lower, upper)))
        let made = SendFeedbackTextWindow(
            index: index, count: windowCount, text: SendFeedbackText.sanitize(String(decoding: slice, as: UTF8.self)))
        cached[index] = made
        cachedOrder.append(index)
        if cachedOrder.count > Self.cacheCount { cached[cachedOrder.removeFirst()] = nil }
        return made
    }
}

/// What opening an entry gives.
nonisolated enum SendFeedbackEntryContent: Sendable {
    case text(SendFeedbackTextReader)
    case archive(SendFeedbackArchiveReader)
    case empty
    case unavailable(Reason)

    enum Reason: Equatable, Sendable {
        case folder
        case problem(SendFeedbackArchiveEntry.Problem)
        /// Not text: shown by name and size alone.
        case notText
        /// An archive that is itself deflated: reading its directory would mean inflating all of it.
        case compressedArchive
        /// An archive inside an archive inside the report.
        case tooDeep
    }
}

/// An open archive: its listing, and the content of an entry on request. Reads happen on its own
/// executor, never on the main thread.
actor SendFeedbackArchiveReader {
    private let archive: SendFeedbackArchive

    nonisolated var listing: SendFeedbackArchiveListing { archive.listing }
    nonisolated var depth: Int { archive.depth }

    init(_ archive: SendFeedbackArchive) {
        self.archive = archive
    }

    /// Opens the archive in `url`; throws what is wrong with it.
    static func open(file url: URL) async throws -> SendFeedbackArchiveReader {
        try await Task.detached(priority: .userInitiated) {
            guard let source = SendFeedbackFileBytes(url: url) else { throw SendFeedbackArchiveError.unavailable }
            return SendFeedbackArchiveReader(try SendFeedbackArchive.open(source))
        }.value
    }

    /// The whole of an entry, for a viewer that draws it itself (a picture), and only when it declares at most
    /// `limit` bytes, and never above `SendFeedbackArchiveLimits.maxWholeEntryBytes`. A larger entry throws
    /// `.tooLarge`; a folder, an encrypted entry and the like throw `.unsupported`; an entry whose bytes do
    /// not match the directory throws `.damaged`. Nothing is written to disk.
    func bytes(of entryID: Int, limit: Int) throws -> Data {
        guard let entry = archive.listing.entries.first(where: { $0.id == entryID }) else {
            throw SendFeedbackArchiveError.damaged
        }
        if entry.isFolder || entry.problem != nil { throw SendFeedbackArchiveError.unsupported }
        let allowed = UInt64(min(max(0, limit), SendFeedbackArchiveLimits.maxWholeEntryBytes))
        guard entry.size <= allowed else { throw SendFeedbackArchiveError.tooLarge }
        if entry.size == 0 { return Data() }
        let stream = SendFeedbackEntryStream(try archive.location(of: entry))
        return try stream.read(at: 0, count: Int(entry.size))
    }

    func content(of entryID: Int) throws -> SendFeedbackEntryContent {
        guard let entry = archive.listing.entries.first(where: { $0.id == entryID }) else {
            throw SendFeedbackArchiveError.damaged
        }
        if entry.isFolder { return .unavailable(.folder) }
        if let problem = entry.problem { return .unavailable(.problem(problem)) }
        if entry.size == 0 { return .empty }
        let location = try archive.location(of: entry)
        let stream = SendFeedbackEntryStream(location)
        let head = try stream.read(at: 0, count: SendFeedbackArchiveLimits.sniffBytes)
        if SendFeedbackText.looksLikeArchive(head) {
            if archive.depth >= 1 { return .unavailable(.tooDeep) }
            guard !location.deflated else { return .unavailable(.compressedArchive) }
            if let inner = SendFeedbackRangeBytes(parent: archive.source, start: location.dataStart, length: location.size),
               let opened = try? SendFeedbackArchive.open(inner, depth: archive.depth + 1) {
                return .archive(SendFeedbackArchiveReader(opened))
            }
            return .unavailable(.notText)
        }
        guard SendFeedbackText.looksLikeText(head) else { return .unavailable(.notText) }
        return .text(SendFeedbackTextReader(location))
    }
}

#endif
