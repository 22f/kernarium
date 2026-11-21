// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The words of Home's AOT row. The owner chooses between two titles and two
/// notes: `chosenTitle` and `chosenNote` are the one place the choice is made,
/// and the Simulator preview shows any pair through the model.
enum TranslatedLibrariesWords {
    /// The row's title: the name AOT, which stays as it is as JIT does, or
    /// what the row is in words.
    enum Title: CaseIterable {
        case aot
        case translatedLibraries
    }

    /// The line under the title while the switch works: what it does and that
    /// it is faster, or only that it is faster, and in both when to turn it off.
    enum Note: CaseIterable {
        case explained
        case short
    }

    static let chosenTitle = Title.aot
    static let chosenNote = Note.short

    static func text(for title: Title) -> String {
        switch title {
        case .aot:
            return "AOT"
        case .translatedLibraries:
            return String(
                localized: "Translated system libraries",
                comment: "Title of the page of notices, of the row that opens it, and of the AOT switch on Home when Home words it in place of the name AOT, for the Android system libraries that Andarium translates to machine code ahead of time.")
        }
    }

    static func text(for note: Note) -> String {
        switch note {
        case .explained:
            return String(
                localized: "Android\u{2019}s system libraries run from translated code, which is faster. Turn this off only to troubleshoot.",
                comment: "Under the AOT switch on Home. AOT is a name and stays as it is. Says that Android's system libraries run from machine code translated ahead of time, that this is faster, and that the switch is for trouble-shooting.")
        case .short:
            return String(
                localized: "Faster. Turn this off only to troubleshoot.",
                comment: "Under the AOT switch on Home. AOT is a name and stays as it is. Says that the switch makes Android faster and is for trouble-shooting.")
        }
    }

    /// Under the title while the next start runs on the JIT engine, which does
    /// not use them: the switch is dimmed and changes nothing.
    static let notUsedWithJit = String(
        localized: "Not used while JIT is on.", table: "JIT",
        comment: "Under the AOT switch on Home while JIT is on and ready for the next start. AOT and JIT are names and stay as they are.")
}

/// Whether Android's system libraries run from the code translated from them
/// ahead of time (`AppModel.translatedLibrariesOn`), the second row of Home's
/// Performance section, between "120 Hz" and JIT. It is drawn only in a build
/// that carries the translated-code framework, and applies when Android next
/// starts: Home shows only while no guest runs. While the next start runs on
/// the JIT engine, which does not use the translated code, the row is dimmed,
/// its switch cannot be changed, and the line under the title says so.
struct TranslatedLibrariesRow: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let unused = model.jitInUseForNextStart
        let title = TranslatedLibrariesWords.text(for: model.translatedLibrariesTitle)
        let detail = unused
            ? TranslatedLibrariesWords.notUsedWithJit
            : TranslatedLibrariesWords.text(for: model.translatedLibrariesNote)
        let spoken: String = "\(title), \(detail)"
        OptionRow(title: title, symbol: "books.vertical.fill", detail: detail, dimmed: unused) {
            Toggle(isOn: Binding(
                get: { model.translatedLibrariesOn },
                set: { model.setTranslatedLibraries($0) }
            )) { Text(verbatim: title) }
            .labelsHidden()
            .disabled(unused)
            .accessibilityIdentifier("home-aot-switch")
        }
        // VoiceOver says the name first and then the line under it (the note, or
        // why the switch is dimmed), as a label, so that it is heard with hints
        // off; the switch's own state follows. The label is a String, so that
        // no catalog key is extracted from it.
        .accessibilityLabel(spoken)
        .id("aot")
    }
}
