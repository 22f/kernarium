// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

extension AppModel {
    /// The Simulator preview plays the text message bar in one of its states:
    /// it has no virtual machine to answer. The bar is open on the screen of
    /// the running session, with what the state needs typed.
    func previewTextMessage(_ screen: String) {
        let state = session.textMessage
        state.open(sender: GuestSms.defaultSender)
        let code = "Your sign-in code is 482913."
        func send(_ outcome: GuestSms.Outcome) {
            state.setText(code)
            state.begin()
            let identifier = GuestSms.newIdentifier()
            _ = state.finish(outcome: outcome, sender: state.sender, text: code, identifier: identifier)
        }
        switch screen {
        case "sms-bar-typing":
            state.setText(code)
        case "sms-bar-near-limit":
            state.setText(String(repeating: "a", count: GuestSms.maximumUnits - 40))
        case "sms-bar-sending":
            // The bar's message box takes the keyboard 300 ms after the bar rises,
            // and the keyboard stays up through a send: the send begins after that.
            state.setText(code)
            Task { @MainActor [weak state] in
                try? await Task.sleep(for: .milliseconds(1500))
                state?.begin()
            }
        case "sms-bar-sent":
            send(.handedToAndroid(parts: 1))
        case "sms-bar-not-confirmed":
            send(.notConfirmed)
        case "sms-bar-unsupported":
            send(.unsupported)
        case "sms-bar-bad-sender":
            state.setSender("Shop & Go")
            state.setText(code)
        case "sms-bar-long-sender":
            state.setSender("Bank of the North")
            state.setText(code)
        case "sms-bar-no-sender":
            state.setSender("")
            state.setText(code)
        case "sms-bar-too-long":
            state.setText(String(repeating: "a", count: GuestSms.maximumUnits + 12))
        case "sms-bar-unsendable":
            state.setText("Code \u{FFFE} 482913")
        case "sms-bar-paused":
            // The low-space screen covers the session; under it the bar says
            // Android is paused and Send is off.
            state.setText(code)
            spacePause = SpacePause(freeBytes: 820 * Limits.bytesPerMiB)
        default:
            break
        }
    }
}
