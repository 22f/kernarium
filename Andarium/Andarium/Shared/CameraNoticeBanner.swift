// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The words that the camera is paused while other apps share the screen, over
/// the top of the session, in the shape of the thermal notice: it takes touches
/// only where it is drawn, and it goes when the person dismisses it or the
/// interruption ends.
struct CameraNoticeBanner: View {
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
        .accessibilityIdentifier("camera-notice")
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
            .accessibilityIdentifier("camera-notice-dismiss")
    }

    private func banner(oneLine: Bool) -> some View {
        HStack(alignment: .center, spacing: 12) {
            icon
            Text(CameraWords.noticeTitle)
                .font(.subheadline.weight(.semibold))
                .fixedSize(horizontal: oneLine, vertical: true)
            dismissButton
        }
        .padding(.vertical, 10)
        .padding(.horizontal, 14)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
    }

    private var stacked: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .top, spacing: 12) {
                icon
                Text(CameraWords.noticeTitle)
                    .font(.subheadline.weight(.semibold))
                    .fixedSize(horizontal: false, vertical: true)
            }
            dismissButton
        }
        .padding(.vertical, 12)
        .padding(.horizontal, 14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
    }
}
