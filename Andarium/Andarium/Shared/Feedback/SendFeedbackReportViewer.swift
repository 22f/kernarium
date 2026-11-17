// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import AndariumCore
import SwiftUI

/// What an attached bug report holds, to read before the mail goes: over the sheet, the entries of its
/// archive with their sizes, and a text entry in windows. The start log, which is a text, opens the same
/// page. Reading only: nothing here changes, shares, exports or copies a file, and the archive is never
/// extracted. Everything in it was written by the virtual machine, so it is shown as plain text, in
/// windows read on demand, by the bounded reader of `Logic/SendFeedbackArchive.swift`.
struct SendFeedbackReportViewer: View {
    let attachment: SendFeedbackAttachment
    #if !APP_STORE
    /// The Simulator preview opens the entry of this name at once (`-screen feedback-viewer-text`).
    var startsAt: String?
    #endif
    @Environment(\.dismiss) private var dismiss
    @State private var opened: Opened = .opening

    private enum Opened {
        case opening
        case archive(SendFeedbackArchiveReader)
        case text(SendFeedbackTextReader)
        case failed(LocalizedStringResource)
    }

    var body: some View {
        NavigationStack {
            content
                .navigationTitle(Text(Self.title(attachment.kind)))
                .navigationBarTitleDisplayMode(.inline)
                .toolbar { SendFeedbackViewerClose(close: { dismiss() }) }
        }
        .task { await open() }
        .accessibilityIdentifier("feedback-viewer")
    }

    @ViewBuilder
    private var content: some View {
        switch opened {
        case .opening:
            ProgressView()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        case .archive(let reader):
            #if !APP_STORE
            SendFeedbackEntryList(reader: reader, close: { dismiss() }, startsAt: startsAt)
            #else
            SendFeedbackEntryList(reader: reader, close: { dismiss() })
            #endif
        case .text(let reader):
            SendFeedbackTextPage(reader: reader)
        case .failed(let message):
            SendFeedbackViewerMessage(message: message)
        }
    }

    private static func title(_ kind: SendFeedbackAttachment.Kind) -> LocalizedStringResource {
        kind == .startLog
            ? LocalizedStringResource("Start log", table: "Feedback")
            : LocalizedStringResource("Bug report", table: "Feedback")
    }

    private func open() async {
        if attachment.kind == .startLog {
            if let reader = SendFeedbackTextReader(file: attachment.url) {
                opened = .text(reader)
            } else {
                opened = .failed(Self.unreadable)
            }
            return
        }
        do {
            opened = .archive(try await SendFeedbackArchiveReader.open(file: attachment.url))
        } catch let error as SendFeedbackArchiveError {
            opened = .failed(Self.message(for: error))
        } catch {
            opened = .failed(Self.unreadable)
        }
    }

    private static let unreadable = LocalizedStringResource(
        "The file couldn\u{2019}t be read.", table: "Feedback")

    static func message(for error: SendFeedbackArchiveError) -> LocalizedStringResource {
        switch error {
        case .notAnArchive, .damaged:
            LocalizedStringResource(
                "This bug report can\u{2019}t be read. It may be damaged or cut short.", table: "Feedback")
        case .unsupported, .tooLarge:
            LocalizedStringResource("This bug report can\u{2019}t be shown here.", table: "Feedback")
        case .unavailable:
            unreadable
        }
    }
}

/// The toolbar's Close, on every page of the viewer.
private struct SendFeedbackViewerClose: ToolbarContent {
    let close: () -> Void

    var body: some ToolbarContent {
        ToolbarItem(placement: .cancellationAction) {
            Button(LocalizedStringResource("Close", table: "Feedback"), systemImage: "xmark", role: .cancel, action: close)
                .accessibilityIdentifier("feedback-viewer-close")
        }
    }
}

/// A line of words in the middle of a page: what cannot be shown, and why.
private struct SendFeedbackViewerMessage: View {
    let message: LocalizedStringResource

    var body: some View {
        Text(String(localized: message).keepingTogether)
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .multilineTextAlignment(.center)
            .padding(24)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .accessibilityIdentifier("feedback-viewer-message")
    }
}

// MARK: The entries

/// The entries of an archive: name and size of each, the ones that can be opened leading to their text or,
/// for an archive stored inside, to its own list.
struct SendFeedbackEntryList: View {
    let reader: SendFeedbackArchiveReader
    let close: () -> Void
    #if !APP_STORE
    /// The Simulator preview opens the entry of this name at once.
    var startsAt: String?
    @State private var started: SendFeedbackArchiveEntry?
    @State private var didStart = false
    #endif

    private var listing: SendFeedbackArchiveListing { reader.listing }

    var body: some View {
        List {
            Section {
                ForEach(listing.entries) { entry in
                    if entry.isFolder || entry.problem != nil {
                        row(entry)
                    } else {
                        NavigationLink {
                            SendFeedbackEntryPage(reader: reader, entry: entry, close: close)
                        } label: {
                            row(entry)
                        }
                        .accessibilityIdentifier("feedback-viewer-entry")
                    }
                }
            } header: {
                summary
                    .textCase(nil)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .padding(.bottom, 4)
                    .accessibilityIdentifier("feedback-viewer-summary")
            }
        }
        .listStyle(.insetGrouped)
        .overlay {
            if listing.entries.isEmpty {
                SendFeedbackViewerMessage(message: LocalizedStringResource(
                    "This archive has no entries.", table: "Feedback"))
            }
        }
        #if !APP_STORE
        .navigationDestination(item: $started) { entry in
            SendFeedbackEntryPage(reader: reader, entry: entry, close: close)
        }
        .task {
            guard !didStart, let startsAt, let entry = listing.entries.first(where: { $0.name == startsAt }) else { return }
            didStart = true
            started = entry
        }
        #endif
    }

    private var summary: some View {
        let count = String(AttributedString(
            localized: "^[\(listing.declaredCount) entry](inflect: true)", table: "Feedback").characters)
        let total = Formats.size(bytes: listing.totalBytes)
        return VStack(alignment: .leading, spacing: 2) {
            Text("\(count) \u{00B7} \(total)", tableName: "Feedback")
            if listing.unlistedCount > 0 {
                Text(String(
                    localized: "Only the first \(listing.entries.count) are listed.", table: "Feedback").keepingTogether)
            }
        }
    }

    private func row(_ entry: SendFeedbackArchiveEntry) -> some View {
        let name = entry.name.isEmpty ? String(localized: Self.unnamed) : entry.name
        let note = Self.note(entry)
        let size = entry.isFolder ? nil : Formats.size(bytes: entry.size)
        return VStack(alignment: .leading, spacing: 1) {
            Text(verbatim: name)
                .font(.subheadline)
                .lineLimit(2)
                .truncationMode(.middle)
            Text(verbatim: [size, note].compactMap { $0 }.joined(separator: " \u{00B7} "))
                .font(.caption)
                .foregroundStyle(.secondary)
                .monospacedDigit()
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(verbatim: ([name] + [size, note].compactMap { $0 }).joined(separator: ", ")))
    }

    private static let unnamed = LocalizedStringResource("Unnamed entry", table: "Feedback")

    /// What the row says besides its size, for an entry that is not shown as text.
    private static func note(_ entry: SendFeedbackArchiveEntry) -> String? {
        if entry.isFolder { return String(localized: "Folder", table: "Feedback") }
        switch entry.problem {
        case .none: return nil
        case .encrypted: return String(localized: "Encrypted, can\u{2019}t be shown", table: "Feedback")
        case .unsupportedCompression, .zip64: return String(localized: "Not supported, can\u{2019}t be shown", table: "Feedback")
        case .tooLarge: return String(localized: "Too large to show", table: "Feedback")
        case .damaged: return String(localized: "Damaged, can\u{2019}t be shown", table: "Feedback")
        }
    }
}

/// One entry opened: its text, the list of an archive inside it, or why there is nothing to show.
struct SendFeedbackEntryPage: View {
    let reader: SendFeedbackArchiveReader
    let entry: SendFeedbackArchiveEntry
    let close: () -> Void
    @State private var loaded: Loaded = .loading

    private enum Loaded {
        case loading
        case text(SendFeedbackTextReader)
        case archive(SendFeedbackArchiveReader)
        case message(LocalizedStringResource)
    }

    var body: some View {
        Group {
            switch loaded {
            case .loading:
                ProgressView()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            case .text(let text):
                SendFeedbackTextPage(reader: text)
            case .archive(let nested):
                SendFeedbackEntryList(reader: nested, close: close)
            case .message(let message):
                SendFeedbackViewerMessage(message: message)
            }
        }
        .navigationTitle(Text(verbatim: entry.name.isEmpty ? String(localized: "Unnamed entry", table: "Feedback") : entry.name))
        .navigationBarTitleDisplayMode(.inline)
        .toolbar { SendFeedbackViewerClose(close: close) }
        .task { await load() }
    }

    private func load() async {
        do {
            switch try await reader.content(of: entry.id) {
            case .text(let text): loaded = .text(text)
            case .archive(let nested): loaded = .archive(nested)
            case .empty: loaded = .message(LocalizedStringResource("This entry is empty.", table: "Feedback"))
            case .picture: loaded = .message(Self.message(.notText))
            case .unavailable(let reason): loaded = .message(Self.message(reason))
            }
        } catch is CancellationError {
            return
        } catch let error as SendFeedbackArchiveError {
            loaded = .message(SendFeedbackReportViewer.message(for: error))
        } catch {
            loaded = .message(LocalizedStringResource("The file couldn\u{2019}t be read.", table: "Feedback"))
        }
    }

    private static func message(_ reason: SendFeedbackEntryContent.Reason) -> LocalizedStringResource {
        switch reason {
        case .folder, .notText, .pictureRefused:
            LocalizedStringResource("This entry isn\u{2019}t text, so it can\u{2019}t be shown here.", table: "Feedback")
        case .compressedArchive:
            LocalizedStringResource(
                "This archive is compressed inside the bug report, so it can\u{2019}t be shown here.", table: "Feedback")
        case .tooDeep:
            LocalizedStringResource("An archive inside this archive can\u{2019}t be shown here.", table: "Feedback")
        case .problem:
            LocalizedStringResource("This entry can\u{2019}t be shown.", table: "Feedback")
        }
    }
}

// MARK: The text

/// A text read a window at a time, with the way to the next window and back. Only the window on screen is
/// in memory; the reader keeps a few of the last ones.
struct SendFeedbackTextPage: View {
    let reader: SendFeedbackTextReader
    @State private var index = 0
    @State private var window: SendFeedbackTextWindow?
    @State private var failed = false

    var body: some View {
        ScrollView {
            if let window, window.index == index {
                // Plain text: no markup, no links, nothing to select, share or copy out.
                Text(verbatim: window.text)
                    .font(.system(.caption, design: .monospaced))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(16)
                    .accessibilityIdentifier("feedback-viewer-text")
            } else if failed {
                SendFeedbackViewerMessage(message: LocalizedStringResource(
                    "This part can\u{2019}t be read. The bug report may be damaged or cut short.", table: "Feedback"))
                    .frame(minHeight: 240)
            } else {
                ProgressView()
                    .frame(maxWidth: .infinity, minHeight: 240)
            }
        }
        .id(index)
        .toolbar {
            if reader.windowCount > 1 {
                ToolbarItemGroup(placement: .bottomBar) {
                    Button(LocalizedStringResource("First part", table: "Feedback"), systemImage: "chevron.backward.2") {
                        index = 0
                    }
                    .disabled(index == 0)
                    Button(LocalizedStringResource("Previous part", table: "Feedback"), systemImage: "chevron.backward") {
                        index = max(0, index - 1)
                    }
                    .disabled(index == 0)
                    Spacer()
                    // The numbers alone fit the bar in every language; VoiceOver reads the words.
                    Text(verbatim: "\(index + 1) / \(reader.windowCount)")
                        .font(.footnote.monospacedDigit())
                        .accessibilityLabel(Text("Part \(index + 1) of \(reader.windowCount)", tableName: "Feedback"))
                        .accessibilityIdentifier("feedback-viewer-position")
                    Spacer()
                    Button(LocalizedStringResource("Next part", table: "Feedback"), systemImage: "chevron.forward") {
                        index = min(reader.windowCount - 1, index + 1)
                    }
                    .disabled(index >= reader.windowCount - 1)
                    Button(LocalizedStringResource("Last part", table: "Feedback"), systemImage: "chevron.forward.2") {
                        index = reader.windowCount - 1
                    }
                    .disabled(index >= reader.windowCount - 1)
                }
            }
        }
        .task(id: index) {
            failed = false
            do {
                window = try await reader.window(index)
            } catch is CancellationError {
                return
            } catch {
                failed = true
            }
        }
    }
}

#endif
