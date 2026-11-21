// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

// MARK: Root

/// Root access (Magisk) for the guest: one row with a switch and a note under
/// it, in a section of its own after Capabilities. It is shown only while the
/// release this copy carries has root (`AppModel.rootAvailable`), so a copy
/// without it has no trace of the feature. The switch applies at the next
/// start; Home is only shown while Android is stopped.
struct RootSection: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        if model.guest != nil, model.rootAvailable {
            HomeSection(title: RootWords.sectionTitle) {
                RootRow()
            } footer: {
                SectionNote(RootWords.note)
            }
            .id("root")
        }
    }
}

struct RootRow: View {
    @Environment(AppModel.self) private var model

    /// A key, which is what the person holds to Android as root.
    static let symbol = "key.fill"

    var body: some View {
        OptionRow(
            title: RootWords.rowTitle, symbol: Self.symbol,
            detail: model.rootAttention == .failedStart ? RootWords.paused : nil
        ) {
            Toggle(RootWords.rowTitle, isOn: Binding(
                get: { model.rootOn },
                set: { model.setRoot($0) }
            ))
            .labelsHidden()
            .accessibilityIdentifier("home-root-switch")
        }
    }
}

/// The alert that asks before root is turned on: what a granted app can do.
struct RootWarningAlert: ViewModifier {
    @Environment(AppModel.self) private var model

    func body(content: Content) -> some View {
        content.alert(
            RootWords.warningTitle,
            isPresented: Binding(
                get: { model.rootWarningShowing },
                set: { if !$0 { model.cancelRootWarning() } }
            )
        ) {
            Button("Turn On") { model.confirmRoot() }
                .accessibilityIdentifier("home-root-warning-turn-on")
            Button("Cancel", role: .cancel) {}
        } message: {
            Text(RootWords.warningBody.keepingTogether)
        }
    }
}

extension View {
    func rootWarningAlert() -> some View { modifier(RootWarningAlert()) }
}
