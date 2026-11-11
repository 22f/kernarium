// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The guest's display, kept under the boot and the run so it is one view
/// from the first frame to the last. The runtime draws into it; the boot
/// screen covers it until the guest has something to show.
struct GuestLayer: View {
    @Environment(AppModel.self) private var model
    /// Whether the window scene fills the screen. Until the view is in a
    /// scene, it is taken to be a window, which is the system's own default.
    @State private var coversScreen = false

    var body: some View {
        ZStack {
            // Home states the resolution the guest will get, which is this
            // display's size at the choice, so the display's size is read
            // whether or not the guest is shown.
            DisplayAreaProbe(cornerEdges: SceneCoverage.cornerOffsetEdges(coversScreen: coversScreen))
            if showsGuest {
                ZStack {
                    Color.black.ignoresSafeArea()
                    display
                    // In a window, iPadOS draws its window controls over the
                    // top-left corner of the content, which would cover the
                    // guest's status bar; the system knows where they are. A
                    // scene that covers the screen has none.
                    .containerCornerOffset(SceneCoverage.cornerOffsetEdges(coversScreen: coversScreen), sizeToFit: true)
                }
            }
        }
        .background {
            SceneCoverageReader { coversScreen = $0 }
        }
    }

    private var display: GuestDisplayView {
        var display = GuestDisplayView(
            scale: model.resolution,
            // A touch beside the side keys reaches the guest, and
            // puts the keys away.
            touchDidBegin: { model.session.closeSideKeys() },
            // The hardware keyboard and mouse: the display takes the
            // keys and the system pointer, except while a sheet or an
            // alert needs them.
            hardwareInput: (model.driver as? LiveRuntime)?.keyboardMouse.input,
            hardwareFocus: model.guestHasKeys
        ) { view, geometry in
            (model.driver as? LiveRuntime)?.viewDidLayout(view, geometry)
        }
        #if !APP_STORE
        // A UI test's launch sees which touches reach the guest's
        // touchscreen. An App Store build has no such seam.
        display.contactDidChange = { [model] slot, pressed in
            model.inputProbe?.record("touch \(slot) \(pressed ? "down" : "up")")
        }
        #endif
        return display
    }

    private var showsGuest: Bool { model.phase.showsGuestDisplay }
}
