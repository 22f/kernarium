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

/// The bar's top edge: who the message is from, and Close. It is one view so
/// that the edge can change without the fields under it changing.
private struct TextMessageBarHeader: View {
    @Environment(AppModel.self) private var model
    var focus: FocusState<TextMessageField?>.Binding

    var body: some View {
        let state = model.session.textMessage
        let sending = state.progress == .sending
        HStack(spacing: 8) {
            HStack(spacing: 6) {
                Text(SmsMessages.fromLabel)
                    .foregroundStyle(.secondary)
                    .accessibilityHidden(true)
                // No content type: iOS would offer the person's own number.
                TextField(SmsMessages.senderPlaceholder, text: Binding(
                    get: { state.sender }, set: { model.setTextMessageSender($0) }))
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                    .submitLabel(.next)
                    .onSubmit { focus.wrappedValue = .text }
                    .focused(focus, equals: .sender)
                    .accessibilityLabel(SmsMessages.from)
                    .accessibilityHint(SmsMessages.senderHint)
                    .accessibilityIdentifier("text-message-sender")
                    .frame(maxWidth: 200)
            }
            .font(.subheadline)
            .disabled(sending)
            .padding(.horizontal, 14)
            .frame(height: 36)
            .background(Color(.tertiarySystemFill), in: Capsule())
            Spacer(minLength: 0)
            Button(SmsMessages.close, systemImage: "xmark") { model.closeTextMessage() }
                .disabled(sending)
                .labelStyle(.iconOnly)
                .font(.subheadline.weight(.semibold))
                .frame(width: 36, height: 36)
                .background(Color(.tertiarySystemFill), in: Circle())
                .foregroundStyle(.secondary)
                .accessibilityIdentifier("text-message-close")
        }
    }
}

/// The sender, the message and Send, with what is wrong or how the last send
/// went between them. What is typed and the send are the run's
/// (`TextMessageState`), so the bar holds nothing of its own but the focus.
struct TextMessageBar: View {
    @Environment(AppModel.self) private var model
    @FocusState private var focus: TextMessageField?

    var body: some View {
        let state = model.session.textMessage
        let sending = state.progress == .sending
        VStack(alignment: .leading, spacing: 10) {
            TextMessageBarHeader(focus: $focus)
            if let status = state.status(availability: model.textMessageAvailability) {
                statusLine(status)
            }
            HStack(alignment: .bottom, spacing: 8) {
                // The one-time code type lets iOS offer a code that has just
                // come to the person's phone.
                TextField(SmsMessages.name, text: Binding(
                    get: { state.text }, set: { model.setTextMessageText($0) }), axis: .vertical)
                    .lineLimit(1...4)
                    .textContentType(.oneTimeCode)
                    .focused($focus, equals: .text)
                    .disabled(sending)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 10)
                    .background(Color(.tertiarySystemFill), in: .rect(cornerRadius: 20))
                    .accessibilityLabel(SmsMessages.message)
                    .accessibilityIdentifier("text-message-text")
                Group {
                    if sending {
                        ProgressView()
                            .frame(width: 44, height: 44)
                            .accessibilityLabel(SmsMessages.sending)
                    } else {
                        Button {
                            model.sendTextMessage()
                        } label: {
                            Image(systemName: "arrow.up")
                                .font(.body.weight(.semibold))
                                .frame(width: 30, height: 30)
                        }
                        .buttonStyle(.glassProminent)
                        .buttonBorderShape(.circle)
                        .disabled(!model.canSendTextMessage)
                        .keyboardShortcut(.return, modifiers: .command)
                        .accessibilityLabel(SmsMessages.send)
                        .accessibilityIdentifier("text-message-send")
                    }
                }
            }
        }
        .padding(12)
        .glassEffect(.regular, in: .rect(cornerRadius: 28))
        .frame(maxWidth: 560)
        .padding(.horizontal, 10)
        .padding(.bottom, 6)
        .accessibilityElement(children: .contain)
        .accessibilityLabel(SmsMessages.name)
        .task {
            // The message box takes the keys once the bar has come up.
            guard state.progress != .sending else { return }
            try? await Task.sleep(for: .milliseconds(300))
            focus = .text
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
