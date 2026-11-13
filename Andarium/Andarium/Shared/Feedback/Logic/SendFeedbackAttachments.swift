// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import AndariumCore
import Foundation
import UniformTypeIdentifiers

/// One file that goes with the mail: staged in the sheet's folder, which is
/// removed when the sheet closes.
nonisolated struct SendFeedbackAttachment: Identifiable, Equatable, Sendable {
    enum Kind: Equatable, Sendable {
        case bugReport, screenshot, photo, video
        /// What the sheet attaches by itself when it opens during a start.
        case startLog
    }

    let id: UUID
    var kind: Kind
    var url: URL
    var bytes: UInt64
    /// The picture's size in pixels, for a screenshot or a photo.
    var pixels: SendFeedbackPixels?
    /// A video's length.
    var durationSeconds: Int?

    init(
        id: UUID = UUID(), kind: Kind, url: URL, bytes: UInt64, pixels: SendFeedbackPixels? = nil,
        durationSeconds: Int? = nil
    ) {
        self.id = id
        self.kind = kind
        self.url = url
        self.bytes = bytes
        self.pixels = pixels
        self.durationSeconds = durationSeconds
    }

    var fileName: String { url.lastPathComponent }

    /// The type the composer is told the file has.
    var mimeType: String {
        UTType(filenameExtension: url.pathExtension)?.preferredMIMEType ?? "application/octet-stream"
    }

    var size: String { Formats.size(bytes: bytes) }
}

nonisolated struct SendFeedbackPixels: Equatable, Sendable {
    var width: Int
    var height: Int
}

/// The files of a draft, in the order they were added, and what the mail
/// service may think of them together.
nonisolated struct SendFeedbackAttachments: Equatable, Sendable {
    /// Above this many bytes in all, a mail service may refuse the mail. The
    /// composer applies its own rules; Send is never blocked by it.
    static let warningBytes: UInt64 = 20 * Limits.bytesPerMiB

    private(set) var items: [SendFeedbackAttachment] = []

    var isEmpty: Bool { items.isEmpty }
    var count: Int { items.count }
    var totalBytes: UInt64 { items.reduce(0) { $0 + $1.bytes } }
    var exceedsWarningSize: Bool { totalBytes > Self.warningBytes }
    var bugReport: SendFeedbackAttachment? { items.first { $0.kind == .bugReport } }

    /// Adds a file at the end, except a bug report, which goes first and
    /// replaces the one already there. Returns the file it replaced.
    @discardableResult
    mutating func add(_ attachment: SendFeedbackAttachment) -> SendFeedbackAttachment? {
        guard attachment.kind == .bugReport else {
            items.append(attachment)
            return nil
        }
        var replaced: SendFeedbackAttachment?
        if let index = items.firstIndex(where: { $0.kind == .bugReport }) {
            replaced = items.remove(at: index)
        }
        items.insert(attachment, at: 0)
        return replaced
    }

    @discardableResult
    mutating func remove(_ id: SendFeedbackAttachment.ID) -> SendFeedbackAttachment? {
        guard let index = items.firstIndex(where: { $0.id == id }) else { return nil }
        return items.remove(at: index)
    }

    mutating func removeAll() -> [SendFeedbackAttachment] {
        defer { items = [] }
        return items
    }
}

#endif
