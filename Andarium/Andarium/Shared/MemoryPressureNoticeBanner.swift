// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The words that iOS is short of memory over the top of the session, in the
/// shape of the camera notice: a warning says iOS asked Andarium to free memory
/// and the virtual machine may slow down for a moment, and critical says iOS may
/// close Andarium and what to do about it. It takes touches only where it is
/// drawn, and it goes when the person dismisses it or the pressure has passed.
struct MemoryPressureNoticeBanner: View {
    let notice: MemoryPressureNotice
    let dismiss: () -> Void

    @Environment(\.dynamicTypeSize) private var typeSize

    var body: some View {
        // The capsule is as wide as its words when they fit on one line, so a
        // short text in any language is not lost in a wide one; a longer text
        // wraps inside the cap. At the accessibility sizes the button goes
        // under the words, as the camera notice's does, so they keep the width.
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
        .accessibilityIdentifier("memory-notice")
    }

    /// Critical is red, as the memory ruler's line about iOS closing Andarium
    /// is; a warning is orange like the other notices.
    private var tint: Color {
        switch notice {
        case .low: .orange
        case .critical: .red
        }
    }

    private var icon: some View {
        Image(systemName: "memorychip.fill")
            .font(.title3)
            .foregroundStyle(tint)
            .accessibilityHidden(true)
    }

    private var dismissButton: some View {
        Button(NetworkWords.dismissLabel, action: dismiss)
            .buttonStyle(.bordered)
            .controlSize(.small)
            .accessibilityIdentifier("memory-notice-dismiss")
    }

    private func titleText(oneLine: Bool) -> some View {
        Text(MemoryPressureWords.title(for: notice))
            .font(.subheadline.weight(.semibold))
            .fixedSize(horizontal: oneLine, vertical: true)
    }

    private func detailText(oneLine: Bool) -> some View {
        Text(MemoryPressureWords.detail(for: notice).keepingTogether)
            .font(.footnote)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: oneLine, vertical: true)
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

    /// The icon stays beside the title only: the line under it takes the whole
    /// width, so the larger text sizes do not wrap it into a narrow column.
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
