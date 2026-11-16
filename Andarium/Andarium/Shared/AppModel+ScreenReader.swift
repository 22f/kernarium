// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import UIKit

/// Android's screen reader in the session (`ScreenReaderSession`). Every
/// read and change binds to the session it began in and does nothing to a
/// later one; a read that a change overtook is dropped; and nothing turns the
/// screen reader on but the person's own action: the switch in Settings or
/// Turn On on the card.
extension AppModel {
    /// Whether the session offers the screen reader: a normal run, whose
    /// Android the runtime's adb channel reaches. Recovery has none.
    var offersScreenReader: Bool {
        guard capabilities.screenReader, case .running(.normal) = phase else { return false }
        return true
    }

    /// A file transfer or another task holds the adb channel: a read or a
    /// change would only wait for it.
    var screenReaderChannelBusy: Bool {
        busy != nil || fileBrowser?.running == true || previewScreenReaderFault == .transfer
    }

    /// What Settings shows under the switch.
    var screenReaderRow: ScreenReaderRow { session.reader.row(channelBusy: screenReaderChannelBusy) }

    var screenReaderCanChange: Bool {
        offersScreenReader && spacePause == nil && session.reader.canChange(channelBusy: screenReaderChannelBusy)
    }

    // MARK: Reading

    /// Reads Android's state, so the switch and the display show what Android's
    /// own settings last did. Settings asks while it is open and a loop asks
    /// while VoiceOver runs.
    func refreshScreenReader() async {
        // A guest paused for space is not asked: a connection waits for it.
        guard offersScreenReader, spacePause == nil, !screenReaderChannelBusy else { return }
        let current = session
        let reader = current.reader
        guard reader.changing == nil else { return }
        let ticket = reader.beginRead()
        let answer: ScreenReaderState?
        if let driver {
            answer = await driver.readScreenReader()
        } else {
            answer = previewScreenReaderFault == .unreadable ? nil : previewScreenReader
        }
        guard session === current, offersScreenReader else { return }
        if reader.apply(read: answer, ticket) { evaluateScreenReaderCard() }
    }

    /// Reads again while VoiceOver runs, so the display follows a change made
    /// in Android within a few seconds. One loop per session; it ends with
    /// VoiceOver, with the session or with the run.
    func startScreenReaderWatch() {
        let current = session
        let reader = current.reader
        reader.stop()
        guard voiceOverRunning(), offersScreenReader else { return }
        reader.watcher = Task { [weak self] in
            while !Task.isCancelled {
                guard let self, session === current, voiceOverRunning(), offersScreenReader else { return }
                await refreshScreenReader()
                try? await Task.sleep(for: screenReaderWatchInterval)
            }
        }
    }

    /// VoiceOver turned on or off.
    func voiceOverStatusChanged() {
        if voiceOverRunning() {
            startScreenReaderWatch()
        } else {
            session.reader.stop()
            session.reader.card = nil
        }
    }

    /// The first thing a run does about the screen reader: nothing without
    /// VoiceOver (Settings reads it when it opens), a loop of reads with it.
    func screenReaderBootFinished() {
        if voiceOverObserver == nil {
            voiceOverObserver = NotificationCenter.default.addObserver(
                forName: UIAccessibility.voiceOverStatusDidChangeNotification, object: nil, queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated { self?.voiceOverStatusChanged() }
            }
        }
        startScreenReaderWatch()
    }

    // MARK: Changing

    /// Turns the screen reader on or off, and VoiceOver says how it ended. The
    /// service to add comes from the last read; the list it joins is read in
    /// the same command that writes it.
    func setScreenReader(_ on: Bool) {
        guard offersScreenReader, spacePause == nil, !screenReaderChannelBusy else { return }
        let current = session
        let reader = current.reader
        guard let state = reader.state, state.isInstalled else {
            Task { await refreshScreenReader() }
            return
        }
        guard state.isOn != on, reader.beginChange(turningOn: on) else { return }
        let service = state.installedService ?? ScreenReader.services[0]
        Task { [weak self] in
            guard let self else { return }
            do {
                let after: ScreenReaderState
                if let driver {
                    after = try await driver.changeScreenReader(on, service: service)
                } else {
                    after = try await previewChange(on, from: state)
                }
                guard session === current else { return }
                guard after.isOn == on else {
                    // The command ended well and Android shows another state.
                    reader.finishChange(after)
                    failScreenReader(reader, turningOn: on, reason: ScreenReaderWords.notReady)
                    return
                }
                reader.finishChange(after)
                VoiceOver.announce(ScreenReaderWords.announcement(on: on))
                evaluateScreenReaderCard()
            } catch {
                guard session === current else { return }
                failScreenReader(reader, turningOn: on, reason: error.localizedDescription)
                await refreshScreenReader()
            }
        }
    }

    private func failScreenReader(_ reader: ScreenReaderSession, turningOn on: Bool, reason: String) {
        reader.failChange(turningOn: on, reason: reason)
        VoiceOver.announce(ScreenReaderWords.changeFailure(turningOn: on, reason: reason))
    }

    /// The preview's mock guest changes the way a guest does, or as
    /// `previewScreenReaderFault` says.
    private func previewChange(_ on: Bool, from state: ScreenReaderState) async throws -> ScreenReaderState {
        switch previewScreenReaderFault {
        case .slow: try await Task.sleep(for: .seconds(600))
        case .refusesChange: throw AdbFailure(.io(EIO), activity: .screenReader)
        case .unreadable, .transfer, nil: break
        }
        let after = ScreenReader.state(afterTurningOn: on, state)
        previewScreenReader = after
        return after
    }

    /// Opens Android's Accessibility page; Settings closes for it.
    func openAccessibilitySettings() {
        guard offersScreenReader, spacePause == nil, let driver else { return }
        Task { [weak self] in
            do {
                try await driver.openAccessibilitySettings()
            } catch {
                self?.alert = AppAlert(title: ScreenReaderWords.androidSettings, message: error.localizedDescription)
            }
        }
    }

    // MARK: The card

    /// Puts the card over Android's screen, or takes it away, as what Android
    /// last said and the person's earlier answers decide. Only while VoiceOver
    /// runs, and never over Settings or another card.
    func evaluateScreenReaderCard() {
        let reader = session.reader
        guard offersScreenReader, voiceOverRunning() else {
            reader.card = nil
            return
        }
        guard let state = reader.state else { return }
        let canShow = spacePause == nil && busy == nil && sheet == nil && reader.settingsOpen == 0 && reader.card == nil
        if !state.isInstalled {
            if reader.card == .ask { reader.card = nil }
            if canShow, !reader.toldMissing { present(.unavailable, in: reader) }
        } else if state.isOn {
            if reader.card == .ask { reader.card = nil }
        } else if canShow, !reader.declined, reader.changing == nil {
            present(.ask, in: reader)
        }
    }

    private func present(_ card: ScreenReaderSession.Card, in reader: ScreenReaderSession) {
        reader.card = card
        reader.cardFailure = nil
        if card == .unavailable { reader.toldMissing = true }
        // VoiceOver lands on the card's title and reads on from it.
        VoiceOver.screenChanged()
    }

    /// Turn On on the card; the card goes when the screen reader is on, and
    /// stays, with the reason, when it did not turn on.
    func turnOnScreenReaderFromCard() {
        setScreenReader(true)
    }

    /// Not Now on the card: no card again in this session.
    func declineScreenReaderCard() {
        session.reader.declined = true
        session.reader.card = nil
        VoiceOver.screenChanged()
    }

    /// OK on the card that says the image has none.
    func dismissScreenReaderCard() {
        session.reader.card = nil
        VoiceOver.screenChanged()
    }

    /// While Settings is open the card does not rise over it. The reader it
    /// returns is the session's that Settings opened in, which closing gives back.
    func settingsOpened() -> ScreenReaderSession {
        let reader = session.reader
        reader.settingsOpen += 1
        return reader
    }

    func settingsClosed(_ reader: ScreenReaderSession) {
        reader.settingsOpen = max(0, reader.settingsOpen - 1)
        if reader === session.reader, reader.settingsOpen == 0 { evaluateScreenReaderCard() }
    }
}
