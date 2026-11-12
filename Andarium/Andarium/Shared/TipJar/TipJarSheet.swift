// SPDX-License-Identifier: GPL-2.0-or-later

#if TIP_JAR

import SwiftUI

// Buy me tokens. The sheet is a short conversation with Andarium's developer:
// the joke in three bubbles, the plain statement as the centred note a chat
// uses for what is not a message, and the four tiers as reply chips under the
// thumb. A tip is the person's reply and the thank-you the developer's answer;
// after it the tiers are not offered again and Done is the one control.
// Problems are centred notes, never the developer's voice. Nothing here is
// kept when the sheet closes.

#if !APP_STORE
/// The Simulator preview scrolls the conversation to its top, where the sender line is at the largest
/// text sizes (the conversation otherwise rests at its bottom, by the tiers).
private struct TipJarScrollsToTopKey: EnvironmentKey {
    static let defaultValue = false
}

extension EnvironmentValues {
    var tipJarScrollsToTop: Bool {
        get { self[TipJarScrollsToTopKey.self] }
        set { self[TipJarScrollsToTopKey.self] = newValue }
    }
}
#endif

struct TipJarSheet: View {
    let jar: TipJarModel
    /// The developer the header shows.
    let sender: TipJarSenderContent
    /// "iPhone" or "iPad", for the note when purchases are not allowed.
    let device: String
    let close: () -> Void
    @Environment(\.dynamicTypeSize) private var typeSize
    #if !APP_STORE
    @Environment(\.tipJarScrollsToTop) private var scrollsToTop
    private var anchor: UnitPoint { scrollsToTop ? .top : .bottom }
    #else
    private var anchor: UnitPoint { .bottom }
    #endif
    /// The conversation's and the bar's heights: the sheet is as tall as they
    /// are, and grows as the conversation does.
    @State private var conversationHeight: CGFloat = 400
    @State private var barHeight: CGFloat = 130

    /// The conversation, the bar, the navigation bar and the grabber.
    private var fittedHeight: CGFloat { conversationHeight + barHeight + 78 }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 6) {
                    TipJarSender(content: sender)
                        .padding(.bottom, 2)
                    TipJarBubble(text: TipJarWords.coffee, first: true)
                    TipJarBubble(text: TipJarWords.tokens)
                    TipJarBubble(text: TipJarWords.invitation)
                    TipJarNote(text: Text(TipJarWords.statement))
                        .padding(.vertical, 12)
                    replies
                    if typeSize.isAccessibilitySize {
                        TipJarBar(jar: jar, close: close, pinned: false)
                    }
                }
                .padding(.horizontal, 16)
                .padding(.top, 8)
                .padding(.bottom, 12)
                .frame(maxWidth: 600)
                .frame(maxWidth: .infinity)
                .animation(.snappy, value: jar.outcome)
                .animation(.snappy, value: jar.purchasing)
                .onGeometryChange(for: CGFloat.self) { $0.size.height } action: { height in
                    conversationHeight = height
                }
            }
            .defaultScrollAnchor(anchor)
            .background(Color(.systemBackground))
            .navigationTitle(Text(TipJarWords.name))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .principal) { TipJarTitle() }
                // The close symbol, as Settings and the file browser have their Done: an icon-only
                // toolbar button, which the folding iPhone's cover moves into the column beside its
                // camera, with the word for VoiceOver.
                ToolbarItem(placement: .confirmationAction) {
                    Button(action: close) {
                        Label { Text(TipJarWords.done) } icon: { Image(systemName: "xmark") }
                    }
                    .accessibilityIdentifier("tipjar-done-bar")
                }
            }
            .safeAreaBar(edge: .bottom) {
                // At accessibility sizes four full-width chips would take half the screen from the
                // conversation, so the bar is the last thing in the conversation instead.
                if !typeSize.isAccessibilitySize {
                    TipJarBar(jar: jar, close: close)
                        .onGeometryChange(for: CGFloat.self) { $0.size.height } action: { height in
                            barHeight = height
                        }
                }
            }
        }
        // Larger text needs the whole height.
        .presentationDetents(typeSize.isAccessibilitySize ? [.large] : [.height(fittedHeight)])
        // An iPad sheet at an accessibility size takes the whole height.
        .presentationSizing(FixedSheetSizing(width: 600, height: typeSize.isAccessibilitySize ? 2000 : fittedHeight))
        .accessibilityIdentifier("tipjar-sheet")
        .task { jar.start() }
    }

    /// What follows the person's choice, and the problems, which are notes.
    @ViewBuilder
    private var replies: some View {
        if let tier = jar.purchasing ?? jar.outcome?.tier, let offer = jar.offer(for: tier) {
            TipJarReply(offer: offer, sending: jar.purchasing == tier)
                .padding(.bottom, 6)
                .transition(.move(edge: .bottom).combined(with: .opacity))
        }
        switch jar.outcome {
        case .thanks?:
            TipJarSender(content: sender, links: false).padding(.top, 4)
            TipJarBubble(text: TipJarWords.thanksTitle, first: true)
                .transition(.opacity)
            TipJarBubble(text: TipJarWords.thanksText)
                .transition(.opacity)
        case .pending?:
            TipJarNote(
                text: Text(TipJarWords.pendingText), title: Text(TipJarWords.pendingTitle), symbol: "clock.fill",
                color: .orange)
        case .failed(_, let message)?:
            TipJarNote(
                text: Text(verbatim: message), title: Text(TipJarWords.failedTitle),
                symbol: "exclamationmark.triangle.fill", color: .red)
        case nil:
            if !jar.canMakePayments {
                TipJarNote(
                    text: Text(TipJarWords.restrictedText(device: device)),
                    title: Text(TipJarWords.restrictedTitle(device: device)), symbol: "hand.raised.fill",
                    color: .secondary)
            } else if jar.phase == .unavailable {
                TipJarNote(
                    text: Text(TipJarWords.unavailableText), title: Text(TipJarWords.unavailableTitle),
                    symbol: "icloud.slash", color: .secondary)
            }
        }
    }
}

/// The developer's bubble.
private struct TipJarBubble: View {
    let text: LocalizedStringResource
    var first = false

    var body: some View {
        Text(text)
            .font(.body)
            .fixedSize(horizontal: false, vertical: true)
            .padding(.horizontal, 14)
            .padding(.vertical, 9)
            .background(Color(.secondarySystemBackground), in: bubbleShape(leadingTail: first))
            .frame(maxWidth: 520, alignment: .leading)
            .padding(.trailing, 48)
    }

    /// A sharper top leading corner on the first of a run, as a chat draws the
    /// bubble nearest its sender.
    private func bubbleShape(leadingTail: Bool) -> UnevenRoundedRectangle {
        UnevenRoundedRectangle(
            topLeadingRadius: leadingTail ? 6 : 18, bottomLeadingRadius: 18, bottomTrailingRadius: 18,
            topTrailingRadius: 18, style: .continuous)
    }
}

/// The person's reply: the tier they chose and its price, with a spinner while
/// the store works.
private struct TipJarReply: View {
    let offer: TipJarOffer
    let sending: Bool

    var body: some View {
        HStack(spacing: 8) {
            Spacer(minLength: 48)
            if sending {
                ProgressView()
                    .controlSize(.small)
                    .accessibilityLabel(Text(TipJarWords.waiting))
            }
            Text(verbatim: "\(offer.tier.name) \u{00B7} \(offer.price)")
                .font(.body.weight(.semibold))
                .foregroundStyle(.white)
                .padding(.horizontal, 14)
                .padding(.vertical, 9)
                .background(Color.accentColor, in: .rect(cornerRadius: 18))
                .opacity(sending ? 0.7 : 1)
                .accessibilityLabel(Text(verbatim: "\(offer.tier.name), \(offer.price)"))
        }
        .frame(maxWidth: .infinity, alignment: .trailing)
        .accessibilityIdentifier("tipjar-reply")
    }
}

/// A centred note: the plain statement, and every problem or wait.
private struct TipJarNote: View {
    let text: Text
    var title: Text?
    var symbol: String?
    var color: Color = .secondary

    var body: some View {
        VStack(spacing: 3) {
            if let title {
                HStack(spacing: 5) {
                    if let symbol {
                        Image(systemName: symbol)
                            .foregroundStyle(color)
                            .accessibilityHidden(true)
                    }
                    title
                }
                .font(.footnote.weight(.semibold))
            }
            text
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
        .multilineTextAlignment(.center)
        .fixedSize(horizontal: false, vertical: true)
        .padding(.horizontal, 24)
        .frame(maxWidth: .infinity)
        .accessibilityElement(children: .combine)
    }
}

/// What stands under the conversation: the four tiers as reply chips, two by
/// two (one column at the end of the conversation at accessibility sizes); Try again when the store gave
/// nothing; Done alone after the thank-you; nothing when purchases are not
/// allowed and no prices came.
private struct TipJarBar: View {
    @Environment(\.dynamicTypeSize) private var typeSize
    let jar: TipJarModel
    let close: () -> Void
    /// Pinned under the conversation on a fade, or (accessibility sizes) the last thing in it.
    var pinned = true

    var body: some View {
        Group {
            if jar.thanked {
                Button(action: close) {
                    Text(TipJarWords.done).frame(maxWidth: .infinity)
                }
                .buttonStyle(.glassProminent)
                .controlSize(.large)
                .accessibilityIdentifier("tipjar-done")
            } else if jar.canRetry {
                Button { jar.retry() } label: {
                    Text(TipJarWords.retry).frame(maxWidth: .infinity)
                }
                .buttonStyle(.glassProminent)
                .controlSize(.large)
                .accessibilityIdentifier("tipjar-retry")
            } else {
                let columns = Array(repeating: GridItem(.flexible(), spacing: 10), count: typeSize.isAccessibilitySize ? 1 : 2)
                LazyVGrid(columns: columns, spacing: 10) {
                    ForEach(jar.shownTiers) { tier in
                        chip(tier)
                    }
                }
            }
        }
        .frame(maxWidth: 600)
        .padding(.horizontal, pinned ? 16 : 0)
        .padding(.top, pinned ? 14 : 12)
        .padding(.bottom, pinned ? 6 : 0)
        .frame(maxWidth: .infinity)
        .background {
            if pinned { TipJarBarFade(color: Color(.systemBackground)) }
        }
    }

    private func chip(_ tier: TipJarTier) -> some View {
        let offer = jar.offer(for: tier)
        let enabled = jar.canBuy(tier) || jar.purchasing == tier
        let name = Text(verbatim: tier.name).font(.body.weight(.semibold))
        let price = Group {
            if let offer {
                Text(verbatim: offer.price)
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
            } else {
                ProgressView()
                    .controlSize(.small)
                    .accessibilityLabel(Text(TipJarWords.loadingPrice))
            }
        }
        return Button { jar.purchase(tier) } label: {
            // A long price goes under the name rather than being cut.
            ViewThatFits(in: .horizontal) {
                HStack(spacing: 8) {
                    name
                    Spacer(minLength: 4)
                    price
                }
                .lineLimit(1)
                VStack(alignment: .leading, spacing: 0) {
                    name
                    price.font(.subheadline)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 4)
            .frame(minHeight: 48)
            .contentShape(.capsule)
        }
        .buttonStyle(.plain)
        .glassEffect(enabled ? .regular.interactive() : .regular, in: .capsule)
        .opacity(enabled ? 1 : 0.45)
        .disabled(!enabled)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(verbatim: offer.map { "\(tier.name), \($0.price)" } ?? tier.name))
        .accessibilityHint(Text(TipJarWords.chipHint))
        .accessibilityAddTraits(.isButton)
        .accessibilityIdentifier("tipjar-tier-\(tier.rawValue)")
    }
}

/// What a bar at the foot of a sheet stands on, as Start does on Home: the page
/// fades out under it instead of showing through.
private struct TipJarBarFade: View {
    let color: Color

    var body: some View {
        LinearGradient(
            stops: [
                .init(color: color.opacity(0), location: 0),
                .init(color: color.opacity(0.92), location: 0.3),
                .init(color: color, location: 1),
            ],
            startPoint: .top, endPoint: .bottom
        )
        .ignoresSafeArea(edges: .bottom)
        .allowsHitTesting(false)
    }
}

/// The sheet's title with the menu item's icon before it.
private struct TipJarTitle: View {
    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: TipJarSymbols.menuItem)
                .foregroundStyle(.tint)
                .accessibilityHidden(true)
            Text(TipJarWords.name)
                .font(.headline)
        }
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isHeader)
    }
}

#endif
