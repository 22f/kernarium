// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// Whether Android can take a text message now.
enum TextMessageAvailability: Equatable {
    case ready
    /// No normal run: the virtual machine is off, starting, or in Recovery,
    /// which runs no adb.
    case notRunning
    /// The virtual machine is paused for lack of space, and the adb channel
    /// waits for it.
    case paused
}

/// The line the bar shows between its fields: what is wrong, how the last
/// send went, or how much room is left.
struct TextMessageStatus: Equatable {
    enum Tone: Equatable {
        /// Something to know: the room left, or that Android is paused.
        case info
        /// Android took the message.
        case good
        /// Something to put right, or a send that was not confirmed.
        case problem
    }

    var words: String
    var tone: Tone
}

/// A text message for the virtual machine, being written in the bar over
/// Android. It belongs to the run: `SessionState` makes one for each run, a
/// send that is under way ends with it, and what a run's send came to is
/// never shown in another.
@Observable
final class TextMessageState {
    /// How the last send ended.
    enum Progress: Equatable {
        case idle
        case sending
        /// Android took the message.
        case sent
        /// Android did not confirm it: it may have arrived.
        case notConfirmed
        /// The system image has no service to take it.
        case unsupported
    }

    /// The bar is up over Android.
    var isOpen = false
    /// What is typed, as typed: it is cleaned and judged where it is used.
    private(set) var sender = GuestSms.defaultSender
    private(set) var text = ""
    var progress = Progress.idle
    /// The identifiers of the drafts whose send was not confirmed.
    var retries = GuestSms.Retries()
    /// The sender and text of the last send, for the words about it to
    /// apply to that draft alone.
    private(set) var attempt: (sender: String, text: String)?
    /// The system image has no service to take a message. The answer cannot
    /// change within a run, so Send stays off for the rest of it and the reason
    /// stays shown.
    private(set) var unsupported = false
    /// The run is over: nothing of this is shown, and a late answer is lost.
    private(set) var ended = false
    @ObservationIgnored var sendTask: Task<Void, Never>?

    /// A field writes its text back as it is when it is laid out or focused:
    /// only a change is typing. While a message is being sent the fields
    /// refuse edits, so the text that goes is the text that stays until
    /// Android has answered; they keep the keyboard's focus (the bar's view).
    func setSender(_ typed: String) {
        guard typed != sender, progress != .sending else { return }
        sender = typed
        edited()
    }

    func setText(_ typed: String) {
        guard typed != text, progress != .sending else { return }
        text = typed
        edited()
    }

    /// Typing after a send began a new message: the words of the earlier
    /// one are not about it.
    private func edited() {
        if progress == .sent { progress = .idle }
    }

    /// Opens the bar with the sender the person last used, and nothing typed.
    func open(sender: String) {
        guard !ended else { return }
        if !isOpen {
            self.sender = sender
            text = ""
            progress = .idle
            attempt = nil
        }
        isOpen = true
    }

    /// Closes the bar and forgets the text. A send under way keeps it up.
    func close() {
        guard progress != .sending else { return }
        isOpen = false
        text = ""
        progress = .idle
        attempt = nil
    }

    /// The run ended: the bar goes, a send under way is dropped, and the
    /// identifiers of the run's unconfirmed drafts are forgotten with the
    /// service's own memory of them.
    func end() {
        ended = true
        sendTask?.cancel()
        sendTask = nil
        isOpen = false
        text = ""
        progress = .idle
        attempt = nil
        retries = GuestSms.Retries()
    }

    /// A send begins with this draft.
    func begin() {
        attempt = (sender, text)
        progress = .sending
    }

    /// The send of `sender` and `sent` with `identifier` ended in `outcome`.
    /// Returns what to say aloud, or nil when the run is over and nothing is
    /// to be said.
    func finish(outcome: GuestSms.Outcome, sender: String, text sent: String, identifier: String) -> String? {
        guard !ended, progress == .sending else { return nil }
        retries.settle(sender: sender, text: sent, identifier: identifier, confirmed: outcome != .notConfirmed)
        switch outcome {
        case .handedToAndroid:
            progress = .sent
            // Only the text that went: what was typed since stays.
            if text == sent { text = "" }
            return SmsMessages.sentToAndroid
        case .unsupported:
            unsupported = true
            progress = .unsupported
            return SmsMessages.unsupported
        case .notConfirmed:
            progress = .notConfirmed
            // The short words are for the eye; what the person is to do is what is heard.
            return SmsMessages.notConfirmedAdvice
        }
    }

    /// The text is the draft the last send was of.
    private var isAttemptedDraft: Bool {
        guard let attempt else { return false }
        return attempt.sender == sender && attempt.text == text
    }

    /// How many UTF-16 units the text may still have.
    var unitsLeft: Int { GuestSms.maximumUnits - text.utf16.count }

    /// The line to show now, or nil for none.
    func status(availability: TextMessageAvailability) -> TextMessageStatus? {
        switch availability {
        case .notRunning: return TextMessageStatus(words: SmsMessages.notRunning, tone: .problem)
        case .paused: return TextMessageStatus(words: SmsMessages.paused, tone: .info)
        case .ready: break
        }
        if unsupported { return TextMessageStatus(words: SmsMessages.unsupported, tone: .problem) }
        switch progress {
        case .sending:
            return nil
        case .sent where text.isEmpty:
            return TextMessageStatus(words: SmsMessages.sentToAndroid, tone: .good)
        case .notConfirmed where isAttemptedDraft:
            return TextMessageStatus(words: SmsMessages.notConfirmedAdvice, tone: .problem)
        default:
            break
        }
        return senderStatus ?? textStatus
    }

    /// What is wrong with the sender. Nothing says it is missing until there
    /// is a message to send.
    var senderStatus: TextMessageStatus? {
        switch GuestSms.senderRefusal(sender) {
        case nil: nil
        case .noSender?: text.isEmpty ? nil : TextMessageStatus(words: SmsMessages.senderEmpty, tone: .problem)
        case let refusal?: TextMessageStatus(words: SmsMessages.refusal(refusal, text: text), tone: .problem)
        }
    }

    /// What is wrong with the text, and once it runs short how much room is
    /// left.
    var textStatus: TextMessageStatus? {
        if unitsLeft < 0 { return TextMessageStatus(words: SmsMessages.charactersOver(-unitsLeft), tone: .problem) }
        if GuestSms.textRefusal(text) == .unsendableCharacter {
            return TextMessageStatus(words: SmsMessages.unsendableCharacter, tone: .problem)
        }
        if unitsLeft <= 100 && !text.isEmpty { return TextMessageStatus(words: SmsMessages.charactersLeft(unitsLeft), tone: .info) }
        return nil
    }
}

extension AppModel {
    /// Settings offers the entrance: the application carries text messages
    /// (`Capabilities.textMessages`) and this is a normal run, which has adb.
    var offersTextMessage: Bool {
        guard capabilities.textMessages, case .running(.normal) = phase else { return false }
        return true
    }

    var textMessageAvailability: TextMessageAvailability {
        guard case .running(.normal) = phase else { return .notRunning }
        return spacePause == nil ? .ready : .paused
    }

    /// Send may be pressed: Android can take it, no send is under way, the
    /// message passes the rules, and the guest's one adb channel is not
    /// busy with a transfer.
    var canSendTextMessage: Bool {
        let state = session.textMessage
        return textMessageAvailability == .ready && state.progress != .sending && !state.unsupported
            && GuestSms.refusal(sender: state.sender, text: state.text) == nil && fileBrowser?.busy != true
    }

    /// Settings' row: Settings closes, and the bar comes up over Android
    /// once the sheet has gone.
    func presentTextMessage() {
        let state = session.textMessage
        Task { @MainActor [weak self, weak state] in
            await self?.textMessageSleep(.milliseconds(350))
            // The same run: a restart in between made a session of its own.
            guard let self, let state, state === self.session.textMessage else { return }
            self.openTextMessage()
        }
    }

    func openTextMessage() {
        guard offersTextMessage else { return }
        let remembered = smsSender.sender.flatMap { GuestSms.senderRefusal($0) == nil ? $0 : nil }
        session.textMessage.open(sender: remembered ?? GuestSms.defaultSender)
    }

    func closeTextMessage() {
        session.textMessage.close()
    }

    func setTextMessageSender(_ typed: String) { session.textMessage.setSender(typed) }
    func setTextMessageText(_ typed: String) { session.textMessage.setText(typed) }

    /// Hands the typed message to Android. The send and its answer belong to
    /// the run: stopping the virtual machine ends it, and an answer that
    /// comes after is dropped.
    func sendTextMessage() {
        guard canSendTextMessage else { return }
        let state = session.textMessage
        let sender = state.sender
        let text = state.text
        smsSender.keep(GuestSms.sender(sender) ?? sender)
        let identifier = state.retries.identifier(sender: sender, text: text)
        state.begin()
        state.sendTask = Task { @MainActor [weak self, weak state] in
            let outcome: GuestSms.Outcome
            if let driver = self?.driver {
                outcome = await driver.sendSms(sender: sender, text: text, identifier: identifier)
            } else {
                // The preview has no guest: Android takes it after a moment.
                try? await Task.sleep(for: .milliseconds(600))
                outcome = .handedToAndroid(parts: 1)
            }
            guard !Task.isCancelled, let state else { return }
            if let words = state.finish(outcome: outcome, sender: sender, text: text, identifier: identifier) {
                self?.textMessageAnnouncer(words)
            }
        }
    }
}
