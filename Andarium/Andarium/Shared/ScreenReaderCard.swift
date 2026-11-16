// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// Rises over Android's screen while VoiceOver runs and Android's screen reader
/// is off: it says that VoiceOver cannot read Android's screen, what the
/// screen reader does instead and how to come back to VoiceOver, and asks.
/// Where the system image has no screen reader it says so, once.
struct ScreenReaderCard: View {
    @Environment(AppModel.self) private var model
    @Environment(\.horizontalSizeClass) private var sizeClass
    let card: ScreenReaderSession.Card

    var body: some View {
        let reader = model.session.reader
        ZStack {
            Color.black.opacity(0.32).ignoresSafeArea()
            VStack(alignment: .leading, spacing: 14) {
                switch card {
                case .ask: ask(reader)
                case .unavailable: unavailable
                }
            }
            .padding(20)
            .frame(maxWidth: sizeClass == .regular ? 520 : 380)
            .glassEffect(.regular, in: .rect(cornerRadius: 26))
            .padding(24)
            .onCardHalf()
        }
        .transition(.opacity)
        // The card is what the person is to deal with: VoiceOver reaches
        // nothing behind it, and lands on its title.
        .accessibilityElement(children: .contain)
        .accessibilityAddTraits(.isModal)
        .accessibilityIdentifier("screen-reader-card")
    }

    private func title(_ text: String) -> some View {
        Text(text.keepingTogether)
            .font(.title3.weight(.bold))
            .accessibilityAddTraits(.isHeader)
    }

    private func body(_ text: String) -> some View {
        Text(text.keepingTogether)
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
    }

    @ViewBuilder
    private func ask(_ reader: ScreenReaderSession) -> some View {
        title(ScreenReaderWords.askTitle)
        body(ScreenReaderWords.cantRead)
        body(ScreenReaderWords.canRead)
        body(ScreenReaderWords.wayBack)
        if let reason = reader.cardFailure {
            Text(reason.keepingTogether)
                .font(.subheadline)
                .foregroundStyle(.red)
                .fixedSize(horizontal: false, vertical: true)
                .accessibilityIdentifier("screen-reader-card-failure")
        }
        HStack(spacing: 10) {
            Spacer()
            Button(ScreenReaderWords.notNow) { model.declineScreenReaderCard() }
                .buttonStyle(.bordered)
                .disabled(reader.changing != nil)
                .accessibilityIdentifier("screen-reader-card-not-now")
            Button {
                model.turnOnScreenReaderFromCard()
            } label: {
                Text(reader.changing == true ? ScreenReaderWords.turningOn
                    : reader.cardFailure != nil ? ScreenReaderWords.tryAgain : ScreenReaderWords.turnOn)
            }
            .buttonStyle(.borderedProminent)
            .disabled(reader.changing != nil)
            .accessibilityIdentifier("screen-reader-card-turn-on")
        }
    }

    @ViewBuilder
    private var unavailable: some View {
        title(ScreenReaderWords.cantRead)
        body(ScreenReaderWords.noReader)
        HStack {
            Spacer()
            Button(ScreenReaderWords.ok) { model.dismissScreenReaderCard() }
                .buttonStyle(.borderedProminent)
                .accessibilityIdentifier("screen-reader-card-ok")
        }
    }
}
