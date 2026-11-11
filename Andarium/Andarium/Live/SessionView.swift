// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// A run: Android's own display fills the screen, and Andarium keeps only
/// the side keys on its edge. The keys are Android's: Power and volume, Back
/// and Home, as the keyboard events a phone's hardware keys would raise.
struct SessionView: View {
    @Environment(AppModel.self) private var model
    let mode: StartMode

    var body: some View {
        @Bindable var model = model
        Color.clear
            .overlay(alignment: .top) {
                if model.session.showStats {
                    StatsOverlay(memoryMiB: model.guest?.memoryMiB ?? Limits.defaultMemoryRangeMiB.lowerBound, recovery: mode == .recovery)
                        .transition(.move(edge: .top).combined(with: .opacity))
                        .padding(.top, StatsPlacement.top)
                        .allowsHitTesting(false)
                }
            }
            .overlay(alignment: .top) {
                SessionNoticesOverlay()
            }
            .overlay {
                SideKeysOverlay(mode: mode, lit: true, open: $model.session.sideKeysOpen, actions: actions)
            }
            .statusBarHidden(true)
            // The system honours the bottom edge's deferral only while the
            // home indicator is visible: with it hidden, one swipe goes Home.
            .persistentSystemOverlays(.visible)
            // The model knows Settings is open, because the guest gives the
            // sheet the keys meanwhile (`GuestLayer`).
            .sessionSettings(isPresented: $model.sessionSettingsOpen, large: false, model: model)
            // The screen over the session says why the guest is paused, and a
            // sheet would cover it.
            .onChange(of: model.spacePause != nil) { _, paused in
                if paused { model.sessionSettingsOpen = false }
            }
            #if !APP_STORE
            .overlay(alignment: .bottomLeading) {
                if let probe = model.inputProbe { InputProbeReadout(probe: probe) }
            }
            #endif
    }

    private var runtime: LiveRuntime? { model.driver as? LiveRuntime }

    private var actions: SideKeyActions {
        SideKeyActions(
            power: { runtime?.press(.power) },
            powerHold: { runtime?.hold(.power) },
            volumeUp: { runtime?.press(.volumeUp) },
            volumeDown: { runtime?.press(.volumeDown) },
            back: { runtime?.press(.back) },
            home: { runtime?.press(.home) },
            settings: { model.sessionSettingsOpen = true }
        )
    }
}

#if !APP_STORE

/// What the guest's keyboard and mouse received, for a UI test to read: one
/// accessibility element whose label is the log. It is small and takes no
/// touch.
private struct InputProbeReadout: View {
    let probe: InputProbe

    var body: some View {
        Text(probe.text)
            .font(.system(size: 6, design: .monospaced))
            .foregroundStyle(.white.opacity(0.6))
            .lineLimit(1)
            .frame(width: 60, height: 8, alignment: .leading)
            .clipped()
            .accessibilityElement()
            .accessibilityLabel(probe.text)
            .accessibilityIdentifier("inputProbeLog")
            .allowsHitTesting(false)
    }
}

#endif
