// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The text message bar, up over the running session while the model says so
/// (`TextMessageState.isOpen`). Android stays in sight above it, so the app
/// that waits for a code can be read while the code is typed.
struct TextMessageBarOverlay: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        ZStack {
            if model.session.textMessage.isOpen {
                TextMessageBar()
                    .frame(maxHeight: .infinity, alignment: .bottom)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
            }
        }
        .animation(.snappy, value: model.session.textMessage.isOpen)
    }
}

private enum TextMessageField: Hashable {
    case sender, text
}

/// The bar's top edge: what the bar is (the Settings row's symbol, "Text
/// message" and a line of who receives it) with Close beside it, and under
/// that who the message is from. It is one view so that the edge can change
/// without the fields under it changing.
private struct TextMessageBarHeader: View {
    @Environment(AppModel.self) private var model
    var focus: FocusState<TextMessageField?>.Binding
    /// VoiceOver starts on the title when the bar comes up.
    var titleFocused: AccessibilityFocusState<Bool>.Binding

    var body: some View {
        let state = model.session.textMessage
        let sending = state.progress == .sending
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 10) {
                IconTile(symbol: "message.fill")
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 1) {
                    Text(SmsMessages.name)
                        .font(.subheadline.weight(.semibold))
                        .accessibilityAddTraits(.isHeader)
                        .accessibilityFocused(titleFocused)
                    Text(SmsMessages.caption)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .minimumScaleFactor(0.85)
                }
                Spacer(minLength: 0)
                Button(SmsMessages.close, systemImage: "xmark") { model.closeTextMessage() }
                    .disabled(sending)
                    .labelStyle(.iconOnly)
                    .font(.footnote.weight(.semibold))
                    .frame(width: 32, height: 32)
                    .background(Color(.tertiarySystemFill), in: Circle())
                    .foregroundStyle(.secondary)
                    .accessibilityIdentifier("text-message-close")
            }
            HStack(spacing: 6) {
                Text(SmsMessages.fromLabel)
                    .foregroundStyle(.secondary)
                    .accessibilityHidden(true)
                // No content type: iOS would offer the person's own number.
                TextField(SmsMessages.senderPlaceholder, text: Binding(
                    get: { state.shownSender }, set: { model.setTextMessageSender($0) }))
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                    .submitLabel(.next)
                    .onSubmit { focus.wrappedValue = .text }
                    .focused(focus, equals: .sender)
                    .opacity(sending ? 0.5 : 1)
                    .accessibilityLabel(SmsMessages.from)
                    .accessibilityHint(SmsMessages.senderHint)
                    .accessibilityIdentifier("text-message-sender")
                    .frame(maxWidth: 200)
            }
            .font(.subheadline)
            .padding(.horizontal, 14)
            .frame(height: 36)
            .background(Color(.tertiarySystemFill), in: Capsule())
        }
    }
}

/// The sender, the message and Send, with what is wrong or how the last send
/// went between them. What is typed and the send are the run's
/// (`TextMessageState`), so the bar holds nothing of its own but the focus.
struct TextMessageBar: View {
    @Environment(AppModel.self) private var model
    @FocusState private var focus: TextMessageField?
    @AccessibilityFocusState private var titleFocused: Bool

    var body: some View {
        let state = model.session.textMessage
        let sending = state.progress == .sending
        VStack(alignment: .leading, spacing: 10) {
            TextMessageBarHeader(focus: $focus, titleFocused: $titleFocused)
            if let status = state.status(availability: model.textMessageAvailability) {
                statusLine(status)
            }
            HStack(alignment: .bottom, spacing: 8) {
                // The one-time code type lets iOS offer a code that has just
                // come to the person's phone.
                TextField(SmsMessages.name, text: Binding(
                    get: { state.shownText }, set: { model.setTextMessageText($0) }),
                    axis: .vertical)
                    .lineLimit(1...4)
                    .textContentType(.oneTimeCode)
                    .focused($focus, equals: .text)
                    .opacity(sending ? 0.5 : 1)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 10)
                    .background(Color(.tertiarySystemFill), in: .rect(cornerRadius: 20))
                    .accessibilityLabel(SmsMessages.message)
                    .accessibilityIdentifier("text-message-text")
                // Send stays where it is while the message goes, disabled and
                // labelled "Sending…", so VoiceOver focus is kept on it and the
                // result follows on the same element.
                Button {
                    model.sendTextMessage()
                } label: {
                    Group {
                        if sending {
                            ProgressView()
                        } else {
                            Image(systemName: "arrow.up")
                                .font(.body.weight(.semibold))
                        }
                    }
                    .frame(width: 30, height: 30)
                }
                .buttonStyle(.glassProminent)
                .buttonBorderShape(.circle)
                .disabled(sending || !model.canSendTextMessage)
                .keyboardShortcut(.return, modifiers: .command)
                .accessibilityLabel(sending ? SmsMessages.sending : SmsMessages.send)
                .accessibilityIdentifier("text-message-send")
            }
        }
        .padding(12)
        .glassEffect(.regular, in: .rect(cornerRadius: 28))
        .frame(maxWidth: 560)
        .padding(.horizontal, 10)
        .padding(.bottom, 6)
        .accessibilityElement(children: .contain)
        // The fields kept the keyboard through the send; once Android has
        // answered, the message box is where the typing goes on.
        .onChange(of: sending) { was, now in
            if was && !now { focus = .text }
        }
        .task {
            // The message box takes the keys once the bar has come up.
            guard state.progress != .sending else { return }
            try? await Task.sleep(for: .milliseconds(300))
            focus = .text
            // After the keyboard's focus, which VoiceOver would follow: it
            // starts on the title, then reads the line under it.
            try? await Task.sleep(for: .milliseconds(400))
            titleFocused = true
        }
    }

    private func statusLine(_ status: TextMessageStatus) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 6) {
            if status.tone == .good {
                Image(systemName: "checkmark.circle.fill")
                    .foregroundStyle(.green)
                    .accessibilityHidden(true)
            }
            Text(status.words)
                .foregroundStyle(status.tone == .problem ? Color.red : Color.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .font(.footnote)
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityIdentifier("text-message-status")
    }
}
