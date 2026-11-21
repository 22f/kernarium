// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The words of Home's AOT row.
enum TranslatedLibrariesWords {
    /// The row's title. AOT is a name and stays as it is, as JIT does; the note
    /// says what it is.
    static let title = "AOT"

    /// Under the title: what the row does, that it is faster, and when to turn
    /// it off.
    static let note = String(
        localized: "Android\u{2019}s system libraries run from translated code, which is faster. Turn this off only to troubleshoot.",
        comment: "Under the AOT switch on Home. AOT is a name and stays as it is. Says that Android's system libraries run from machine code translated ahead of time, that this is faster, and that the switch is for trouble-shooting.")

    /// Under the title while the next start runs on the JIT engine, which does
    /// not use them: the switch is dimmed and changes nothing.
    static let notUsedWithJit = String(
        localized: "Not used while JIT is on.", table: "JIT",
        comment: "Under the AOT switch on Home while JIT is on and ready for the next start. AOT and JIT are names and stay as they are.")
}

/// Whether Android's system libraries run from the code translated from them
/// ahead of time (`AppModel.translatedLibrariesOn`), the third row of the
/// section that holds the JIT row. It is drawn only in a build that carries the
/// translated-code framework, and applies when Android next starts: Home shows
/// only while no guest runs. While the next start runs on the JIT engine, which
/// does not use the translated code, the row is dimmed, its switch cannot be
/// changed, and the line under the title says so.
struct TranslatedLibrariesRow: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let unused = model.jitInUseForNextStart
        OptionRow(
            title: TranslatedLibrariesWords.title, symbol: "books.vertical.fill",
            detail: unused ? TranslatedLibrariesWords.notUsedWithJit : TranslatedLibrariesWords.note,
            detailLines: nil, dimmed: unused
        ) {
            Toggle(isOn: Binding(
                get: { model.translatedLibrariesOn },
                set: { model.setTranslatedLibraries($0) }
            )) { Text(verbatim: TranslatedLibrariesWords.title) }
            .labelsHidden()
            .disabled(unused)
            .accessibilityIdentifier("home-aot-switch")
        }
        // VoiceOver says the name first and the line under it after the state.
        .accessibilityLabel(TranslatedLibrariesWords.title)
        .accessibilityHint(unused ? TranslatedLibrariesWords.notUsedWithJit : TranslatedLibrariesWords.note)
        .id("aot")
    }
}
