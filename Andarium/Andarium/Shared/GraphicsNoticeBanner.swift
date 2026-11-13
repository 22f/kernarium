// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The words that Android ran out of graphics memory, over the top of the
/// session. It takes touches only where it is drawn, so the guest keeps the
/// rest of the screen, and it goes when the person dismisses it or after
/// `GraphicsNotice.visibleFor`.
struct GraphicsNoticeBanner: View {
    let dismiss: () -> Void

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.title3)
                .foregroundStyle(Color.orange)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text(GraphicsNotice.title)
                    .font(.subheadline.weight(.semibold))
                Text(GraphicsNotice.detail.keepingTogether)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Button(GraphicsNotice.dismissLabel, action: dismiss)
                .buttonStyle(.bordered)
                .controlSize(.small)
                .accessibilityIdentifier("graphics-notice-dismiss")
        }
        .padding(.vertical, 10)
        .padding(.horizontal, 14)
        .frame(maxWidth: 480)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
        .padding(.horizontal, 24)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("graphics-notice")
    }
}

/// The banners over the top of the session when the model has a notice to
/// show: iOS denying Andarium the network, Android's graphics memory, the
/// lowered frame rate, the camera (paused for other apps sharing the
/// screen, or off when the guest opens it) and iOS being short of memory, one
/// below the other in that order, below the statistics capsule when that is
/// up. The stack is one column at the top, each banner as wide as its words,
/// clear of the side keys and the capsule.
struct SessionNoticesOverlay: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let denial = model.networkDenialShowing
        let showing = denial != nil || model.graphicsNotice.isShowing || model.thermalNotice.isShowing
            || model.cameraNoticeShowing || model.memoryNoticeShowing
        VStack(spacing: StatsPlacement.noticeGap) {
            if let denial {
                NetworkNoticeBanner(
                    denial: denial, openSettings: { model.openSettings() },
                    dismiss: { model.dismissNetworkNotice() }
                )
                .transition(.move(edge: .top).combined(with: .opacity))
            }
            if model.graphicsNotice.isShowing {
                GraphicsNoticeBanner { model.dismissGraphicsNotice() }
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
            if model.thermalNotice.isShowing {
                ThermalNoticeBanner { model.dismissThermalNotice() }
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
            if let notice = model.cameraNoticeKind {
                CameraNoticeBanner(notice: notice) { model.dismissCameraNotice() }
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
            if let notice = model.memoryNoticeKind {
                MemoryPressureNoticeBanner(notice: notice) { model.dismissMemoryNotice() }
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
        }
        .padding(.top, showing ? StatsPlacement.noticeTop(statsShowing: model.session.showStats) : 0)
        .animation(.easeInOut(duration: 0.2), value: denial)
        .animation(.easeInOut(duration: 0.2), value: model.graphicsNotice.isShowing)
        .animation(.easeInOut(duration: 0.2), value: model.thermalNotice.isShowing)
        .animation(.easeInOut(duration: 0.2), value: model.cameraNoticeKind)
        .animation(.easeInOut(duration: 0.2), value: model.memoryNoticeKind)
    }
}
