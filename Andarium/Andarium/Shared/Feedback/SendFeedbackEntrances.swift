// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import SwiftUI

/// The row that opens Send feedback: a section of its own under Info's header,
/// and the first row of the session's Settings Diagnostics, above Create bug
/// report.
struct SendFeedbackEntrance: View {
    enum Place { case info, settings }

    @Environment(AppModel.self) private var model
    let place: Place

    var body: some View {
        Button {
            model.feedbackSheetOpen = true
        } label: {
            HStack(spacing: 12) {
                IconTile(symbol: "envelope.fill")
                    .accessibilityHidden(true)
                switch place {
                case .info:
                    Text("Send feedback", tableName: "Feedback")
                        .foregroundStyle(Color(.label))
                case .settings:
                    Text("Send feedback\u{2026}", tableName: "Feedback")
                        .foregroundStyle(Color.accentColor)
                }
                Spacer()
            }
            .contentShape(.rect)
        }
        .accessibilityIdentifier(place == .info ? "info-send-feedback" : "settings-send-feedback")
    }
}

extension View {
    /// Presents Send feedback over Info or Settings. It stays over Settings,
    /// which does not close beneath it, so a bug report made from the sheet
    /// leaves the session as it was.
    func sendFeedbackSheet(_ app: AppModel) -> some View {
        sheet(isPresented: Binding(get: { app.feedbackSheetOpen }, set: { app.feedbackSheetOpen = $0 })) {
            SendFeedbackSheet(app: app)
                .environment(app)
                .statusBarHidden(app.virtualMachineRuns)
        }
        #if !APP_STORE
        .task {
            // The Simulator preview opens it once Info or Settings has
            // settled, as a tap would; a sheet presented while its presenter
            // still moves is laid out inside it.
            guard app.feedbackServices.previewOpensAtLaunch else { return }
            try? await Task.sleep(for: .milliseconds(1600))
            app.feedbackSheetOpen = true
        }
        #endif
    }
}

#endif
