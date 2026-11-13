// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The words about the camera over the top of the session, in the shape of the
/// thermal notice: that it is paused while other apps share the screen, or that
/// a guest app opened it while Camera is off and how to turn it on. It takes
/// touches only where it is drawn, and it goes when the person dismisses it or
/// its cause ends.
struct CameraNoticeBanner: View {
    let notice: CameraNotice
    let dismiss: () -> Void

    @Environment(\.dynamicTypeSize) private var typeSize

    var body: some View {
        // The capsule is as wide as its words when they fit on one line, so a
        // short text in any language is not lost in a wide one; a longer text
        // wraps inside the cap. At the accessibility sizes the button goes
        // under the words, as the network notice's does, so they keep the
        // width.
        Group {
            if typeSize.isAccessibilitySize {
                stacked
            } else {
                ViewThatFits(in: .horizontal) {
                    banner(oneLine: true)
                    banner(oneLine: false)
                }
            }
        }
        .frame(maxWidth: 640)
        .padding(.horizontal, 24)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier(identifier)
    }

    private var identifier: String {
        switch notice {
        case .pausedForOtherApps: "camera-notice"
        case .cameraOff: "camera-off-notice"
        }
    }

    private var icon: some View {
        Image(systemName: "camera.fill")
            .font(.title3)
            .foregroundStyle(Color.orange)
            .accessibilityHidden(true)
    }

    private var dismissButton: some View {
        Button(NetworkWords.dismissLabel, action: dismiss)
            .buttonStyle(.bordered)
            .controlSize(.small)
            .accessibilityIdentifier(identifier + "-dismiss")
    }

    private func titleText(oneLine: Bool) -> some View {
        Text(CameraWords.noticeTitle(for: notice))
            .font(.subheadline.weight(.semibold))
            .fixedSize(horizontal: oneLine, vertical: true)
    }

    @ViewBuilder
    private func detailText(oneLine: Bool) -> some View {
        if let detail = CameraWords.noticeDetail(for: notice) {
            Text(detail)
                .font(.footnote)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: oneLine, vertical: true)
        }
    }

    private func banner(oneLine: Bool) -> some View {
        HStack(alignment: .center, spacing: 12) {
            icon
            VStack(alignment: .leading, spacing: 2) {
                titleText(oneLine: oneLine)
                detailText(oneLine: oneLine)
            }
            dismissButton
        }
        .padding(.vertical, 10)
        .padding(.horizontal, 14)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
    }

    /// The icon stays beside the title only: the line under it takes the
    /// whole width, so the larger text sizes do not wrap it into a narrow
    /// column and the notice stays about as tall as the one without a line.
    private var stacked: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .top, spacing: 12) {
                icon
                titleText(oneLine: false)
            }
            detailText(oneLine: false)
            dismissButton
        }
        .padding(.vertical, 12)
        .padding(.horizontal, 14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
    }
}
