// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// What the application chose for the start of a session, in a small file
/// beside the runtime's session record (`session.txt`), so that a bug report
/// says whether a feature that changes how Android runs was on for the start
/// it describes. It holds one plain field per choice and nothing that identifies
/// the device or the person. The next launch keeps it with the session record it
/// belongs to (`PreviousSession`), and the bug report copies both
/// (`BugReportAttachments`).
public struct StartRecord: Equatable, Sendable {
    /// The first line.
    public static let layout = "uml-start 1"

    /// The longest a record is read: it has a few short lines.
    public static let maximumBytes = 1024

    /// Whether the translated system libraries were on for the start.
    public var translatedLibraries: TranslatedLibrariesStart.Word

    public init(translatedLibraries: TranslatedLibrariesStart.Word) {
        self.translatedLibraries = translatedLibraries
    }

    /// The file's text.
    public var text: String {
        "\(Self.layout)\ntranslated-libraries \(translatedLibraries.rawValue)\n"
    }

    /// Reads a record's text; nil when it is not one.
    public init?(text: String) {
        let lines = text.split(separator: "\n", omittingEmptySubsequences: true)
        guard lines.first == Substring(Self.layout) else { return nil }
        var word: TranslatedLibrariesStart.Word?
        for line in lines.dropFirst() {
            guard let space = line.firstIndex(of: " ") else { continue }
            if line[..<space] == "translated-libraries" {
                word = TranslatedLibrariesStart.Word(rawValue: String(line[line.index(after: space)...]))
            }
        }
        guard let word else { return nil }
        translatedLibraries = word
    }

    /// The record at `url`, or nil when there is none, it is longer than
    /// `maximumBytes` or it is not a record.
    public static func read(at url: URL) -> StartRecord? {
        guard let data = BoundedFile.read(url, limit: maximumBytes), data.count <= maximumBytes,
            let text = String(data: data, encoding: .utf8)
        else { return nil }
        return StartRecord(text: text)
    }

    /// Writes the record, replacing the one there; false when it could not be
    /// written, which costs the bug report this one field and nothing else.
    @discardableResult
    public func write(to url: URL) -> Bool {
        do {
            try Data(text.utf8).write(to: url, options: .atomic)
            return true
        } catch {
            print("Andarium: the start record was not written: \(error.localizedDescription)")
            return false
        }
    }
}
