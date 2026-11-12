// SPDX-License-Identifier: GPL-2.0-or-later

#if TIP_JAR

import AndariumCore
import SwiftUI

/// What the Buy me tokens menu item looks like; the toolbar menu's glyph is
/// `HomeMenuSymbols.menu`.
enum TipJarSymbols {
    /// Provisional; the designer's pick replaces this one line.
    static let menuItem = "cup.and.heat.waves"
}

/// One opening of the sheet: a model made when the sheet appears and closed
/// when it goes, so that nothing outlives it.
private struct TipJarPresentation: View {
    let app: AppModel
    @State private var jar: TipJarModel?

    init(app: AppModel) {
        self.app = app
        _jar = State(initialValue: app.makeTipJarModel())
    }

    var body: some View {
        if let jar {
            TipJarSheet(jar: jar, device: app.host.name) { app.tipJarSheetOpen = false }
                .onDisappear { jar.close() }
                #if !APP_STORE
                .task { await app.tipJarServices.previewPlays(jar) }
                #endif
        }
    }
}

extension View {
    /// Presents Buy me tokens over Home. Only the menu item opens it.
    func tipJarSheet(_ app: AppModel) -> some View {
        sheet(isPresented: Binding(get: { app.tipJarSheetOpen }, set: { app.tipJarSheetOpen = $0 })) {
            TipJarPresentation(app: app)
                .environment(app)
        }
        #if !APP_STORE
        .task {
            // The Simulator preview opens it once Home has settled, as a tap
            // would; a sheet presented while its presenter still moves is
            // laid out inside it.
            guard app.tipJarServices.previewOpensSheet else { return }
            try? await Task.sleep(for: .milliseconds(1200))
            app.tipJarSheetOpen = true
        }
        #endif
    }
}

#endif
