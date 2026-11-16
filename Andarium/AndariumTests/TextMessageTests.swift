// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The text message bar as the model plays it: a send and its answer belong
/// to the run, typing goes on while an answer is awaited, a paused guest
/// takes nothing, and the entrance is held where the application does not
/// carry text messages.
@MainActor
@Suite(.serialized)
struct TextMessageTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    /// Holds sends and the wait before the bar rises until a test lets them
    /// go, so a test decides what happens between a send and its answer.
    @MainActor
    private final class Held<Value: Sendable> {
        private var waiting: [CheckedContinuation<Value, Never>] = []
        /// What was let go before anything waited: the next wait has it at once.
        private var early: [Value] = []
        var pending: Int { waiting.count }
        func wait() async -> Value {
            if !early.isEmpty { return early.removeFirst() }
            return await withCheckedContinuation { waiting.append($0) }
        }
        func release(_ value: Value) {
            if waiting.isEmpty { early.append(value) } else { waiting.removeFirst().resume(returning: value) }
        }
    }

    @MainActor
    private final class Said {
        var lines: [String] = []
    }

    private struct Rig {
        let model: AppModel
        let driver: StubDriver
        let answers: Held<GuestSms.Outcome>
        let said: Said
        let memory: GuestSms.SenderMemory
        let cleanUp: () -> Void

        var state: TextMessageState { model.session.textMessage }
    }

    /// A running session of an application that carries text messages, whose
    /// sends wait for the test and whose kept sender is in a database of its own.
    private func rig(capabilities: Capabilities = .preview, mode: StartMode = .normal) throws -> Rig {
        let suite = "TextMessageTests-\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suite))
        let memory = GuestSms.SenderMemory(defaults: defaults)
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: capabilities, createRequirementBytes: 1
        )
        model.smsSender = memory
        let said = Said()
        model.textMessageAnnouncer = { said.lines.append($0) }
        model.textMessageSleep = { _ in }
        let answers = Held<GuestSms.Outcome>()
        let driver = StubDriver(plan: .normal)
        driver.smsAnswer = { _, _, _ in await answers.wait() }
        model.driver = driver
        model.phase = .running(mode)
        return Rig(model: model, driver: driver, answers: answers, said: said, memory: memory, cleanUp: {
            defaults.removePersistentDomain(forName: suite)
        })
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while ContinuousClock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(10))
        }
        return condition()
    }

    /// Long enough for a task that was let go to do whatever it will.
    private func settle() async {
        try? await Task.sleep(for: .milliseconds(80))
    }

    // MARK: A send and what is typed meanwhile

    @Test func aLateAnswerForAnEarlierMessageLeavesWhatWasTypedSince() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        #expect(state.progress == .sending && !model.canSendTextMessage)
        // Typing goes on while Android has not answered.
        model.setTextMessageText("two")
        rig.answers.release(.handedToAndroid(parts: 1))
        #expect(await eventually { state.progress == .sent })
        #expect(state.text == "two")
        #expect(rig.said.lines == [SmsMessages.sentToAndroid])
        // Nothing says the new text was sent, and it can go.
        #expect(state.status(availability: .ready) == nil)
        #expect(model.canSendTextMessage)
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        rig.answers.release(.handedToAndroid(parts: 1))
        #expect(await eventually { state.progress == .sent && state.text.isEmpty })
        #expect(rig.driver.smsSent.map(\.text) == ["one", "two"])
        #expect(state.status(availability: .ready) == TextMessageStatus(words: SmsMessages.sentToAndroid, tone: .good))
        #expect(rig.said.lines == [SmsMessages.sentToAndroid, SmsMessages.sentToAndroid])
    }

    @Test func aFieldWritingItsTextBackIsNotTyping() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        rig.answers.release(.handedToAndroid(parts: 1))
        #expect(await eventually { state.progress == .sent })
        // Laying the emptied field out writes the same text and sender back.
        model.setTextMessageText("")
        model.setTextMessageSender(state.sender)
        #expect(state.progress == .sent)
        #expect(state.status(availability: .ready)?.words == SmsMessages.sentToAndroid)
        model.setTextMessageText("t")
        #expect(state.progress == .idle)
    }

    @Test func aSecondSendWaitsForTheFirstAnswer() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        await settle()
        #expect(rig.driver.smsSent.count == 1)
        rig.answers.release(.handedToAndroid(parts: 1))
        #expect(await eventually { rig.state.progress == .sent })
    }

    @Test func closingWhileAMessageIsBeingSentKeepsTheBarUp() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        model.closeTextMessage()
        #expect(rig.state.isOpen && rig.state.text == "one")
        rig.answers.release(.handedToAndroid(parts: 1))
        #expect(await eventually { rig.state.progress == .sent })
        model.closeTextMessage()
        #expect(!rig.state.isOpen && rig.state.text.isEmpty && rig.state.progress == .idle)
    }

    @Test func aMessageThatWasNotConfirmedStaysAndItsWordsAreAboutThatDraftAlone() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        rig.answers.release(.notConfirmed)
        #expect(await eventually { state.progress == .notConfirmed })
        #expect(state.text == "one")
        #expect(state.status(availability: .ready) == TextMessageStatus(words: SmsMessages.notConfirmedAdvice, tone: .problem))
        #expect(rig.said.lines == [SmsMessages.notConfirmed])
        // Another text is not what the words are about; the same text is.
        model.setTextMessageText("one!")
        #expect(state.status(availability: .ready) == nil)
        model.setTextMessageText("one")
        #expect(state.status(availability: .ready)?.words == SmsMessages.notConfirmedAdvice)
        // The same draft sent again carries the same identifier, so Android's
        // service answers it once if the first did arrive.
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        rig.answers.release(.handedToAndroid(parts: 1))
        #expect(await eventually { state.progress == .sent })
        let identifiers = rig.driver.smsSent.map(\.identifier)
        #expect(identifiers.count == 2 && identifiers[0] == identifiers[1] && GuestSms.isIdentifier(identifiers[0]))
        // Once confirmed, the same words sent again are a new message.
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        rig.answers.release(.handedToAndroid(parts: 1))
        #expect(await eventually { rig.driver.smsSent.count == 3 && state.progress == .sent })
        #expect(rig.driver.smsSent[2].identifier != identifiers[0])
    }

    @Test func anImageWithoutTheServiceSaysSoAndKeepsTheText() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        rig.answers.release(.unsupported)
        #expect(await eventually { state.progress == .unsupported })
        #expect(state.text == "one")
        #expect(state.status(availability: .ready) == TextMessageStatus(words: SmsMessages.unsupported, tone: .problem))
        #expect(rig.said.lines == [SmsMessages.unsupported])
    }

    // MARK: The run

    @Test func theEndOfTheRunDropsTheSendAndItsLateAnswer() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        let earlier = rig.state
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 1)
        #expect(model.session.textMessage !== earlier)
        #expect(earlier.ended && !earlier.isOpen && earlier.text.isEmpty)
        rig.answers.release(.handedToAndroid(parts: 1))
        await settle()
        #expect(rig.said.lines.isEmpty)
        #expect(earlier.progress == .idle)
        #expect(!model.session.textMessage.isOpen && model.session.textMessage.progress == .idle)
    }

    @Test func aRestartDropsItAndTheNewRunShowsNothingOfIt() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        let earlier = rig.state
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        model.restart()
        #expect(earlier.ended && !earlier.isOpen)
        rig.answers.release(.handedToAndroid(parts: 1))
        await settle()
        #expect(rig.said.lines.isEmpty)
        // The run that follows has its own bar: nothing typed, nothing said.
        model.phase = .running(.normal)
        #expect(!model.session.textMessage.isOpen)
        model.openTextMessage()
        #expect(model.session.textMessage.isOpen && model.session.textMessage.text.isEmpty)
        #expect(model.session.textMessage.progress == .idle)
        #expect(model.session.textMessage.status(availability: .ready) == nil)
    }

    @Test func aRunThatFailedLeavesNoBarEither() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.openTextMessage()
        model.setTextMessageText("one")
        model.runEnded(RunEnd(result: 1), uptimeSeconds: 1)
        #expect(!rig.state.isOpen && rig.state.ended)
        // A bar that came up late for that run does not open.
        model.openTextMessage()
        #expect(!rig.state.isOpen)
    }

    @Test func aRestartBeforeTheBarComesUpLeavesNoBarInTheNewRun() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        let wait = Held<Void>()
        model.textMessageSleep = { _ in await wait.wait() }
        model.presentTextMessage()
        #expect(await eventually { wait.pending == 1 })
        model.restart()
        model.phase = .running(.normal)
        wait.release(())
        await settle()
        #expect(!model.session.textMessage.isOpen)
    }

    @Test func settingsClosedAfterTheBarWentUpLeavesWhatWasTyped() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.presentTextMessage()
        #expect(await eventually { rig.state.isOpen })
        model.setTextMessageText("code 1234")
        // Settings opened and closed again over the bar, and its row pressed again.
        model.sessionSettingsOpen = true
        model.sessionSettingsOpen = false
        model.presentTextMessage()
        await settle()
        #expect(rig.state.isOpen && rig.state.text == "code 1234")
        // While the bar is up, the keys are the bar's.
        #expect(!model.guestHasKeys)
        model.closeTextMessage()
        #expect(model.guestHasKeys)
    }

    // MARK: Low space

    @Test func aGuestPausedForSpaceTakesNothing() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        model.setTextMessageText("one")
        #expect(model.textMessageAvailability == .ready && model.canSendTextMessage)
        model.guestPausingForSpace(freeBytes: 100_000_000)
        #expect(model.textMessageAvailability == .paused && !model.canSendTextMessage)
        model.sendTextMessage()
        await settle()
        #expect(rig.driver.smsSent.isEmpty && state.progress == .idle)
        #expect(state.status(availability: model.textMessageAvailability)
            == TextMessageStatus(words: SmsMessages.paused, tone: .info))
        model.spacePause = nil
        #expect(model.textMessageAvailability == .ready && model.canSendTextMessage)
        model.sendTextMessage()
        #expect(await eventually { rig.answers.pending == 1 })
        rig.answers.release(.handedToAndroid(parts: 1))
        #expect(await eventually { state.progress == .sent })
    }

    @Test func aTransferOnTheAdbChannelHoldsTheSendToo() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.fileBrowser = FileBrowserModel(backend: FileBrowserModelTests.ScriptedBackend(), center: NotificationCenter())
        model.openTextMessage()
        model.setTextMessageText("one")
        #expect(model.canSendTextMessage)
        // An import that asks what to do with a name already there holds the channel.
        model.fileBrowser?.conflict = ImportConflict(picks: [], clashing: [], folder: "/sdcard")
        #expect(!model.canSendTextMessage)
    }

    // MARK: Where it is offered

    @Test func theApplicationThatDoesNotCarryItOffersNoEntranceAndOpensNoBar() throws {
        let rig = try rig(capabilities: .device)
        defer { rig.cleanUp() }
        #expect(!rig.model.offersTextMessage)
        rig.model.openTextMessage()
        rig.model.presentTextMessage()
        #expect(!rig.state.isOpen)
    }

    @Test func aDevelopmentLaunchOffersItOnlyToANormalRun() throws {
        var capabilities = Capabilities.device
        capabilities.textMessages = true
        let normal = try rig(capabilities: capabilities)
        defer { normal.cleanUp() }
        #expect(normal.model.offersTextMessage)
        let recovery = try rig(capabilities: capabilities, mode: .recovery)
        defer { recovery.cleanUp() }
        #expect(!recovery.model.offersTextMessage)
        recovery.model.openTextMessage()
        #expect(!recovery.state.isOpen)
        #expect(recovery.model.textMessageAvailability == .notRunning)
        normal.model.phase = .home
        #expect(normal.model.textMessageAvailability == .notRunning && !normal.model.canSendTextMessage)
    }

    // MARK: The sender

    @Test func theSenderIsAndariumUntilAnotherIsUsedAndThenThatOne() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.openTextMessage()
        #expect(rig.state.sender == "Andarium" && rig.state.text.isEmpty)
        model.setTextMessageSender("My Bank")
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(rig.memory.sender == "My Bank")
        #expect(await eventually { rig.answers.pending == 1 })
        rig.answers.release(.handedToAndroid(parts: 1))
        #expect(await eventually { rig.state.progress == .sent })
        // A new run opens with the sender of the last.
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 1)
        model.phase = .running(.normal)
        model.openTextMessage()
        #expect(model.session.textMessage.sender == "My Bank")
        // A kept sender the rules would refuse is not offered again.
        rig.memory.keep("Bad_Name!")
        model.session.textMessage.close()
        model.openTextMessage()
        #expect(model.session.textMessage.sender == "Andarium")
    }

    @Test func theSenderIsCleanedBeforeItIsJudged() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.openTextMessage()
        // A direction mark and a no-break space, as a paste may bring.
        model.setTextMessageSender("\u{202E}My\u{00A0}Bank\u{200E}")
        model.setTextMessageText("one")
        #expect(model.canSendTextMessage)
        model.sendTextMessage()
        #expect(rig.memory.sender == "My Bank")
        #expect(await eventually { rig.answers.pending == 1 })
        rig.answers.release(.handedToAndroid(parts: 1))
        #expect(await eventually { rig.state.progress == .sent })
    }

    // MARK: What is refused, and how it is said

    @Test func eachRefusalHasItsOwnWordsAndSendsNothing() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let (model, state) = (rig.model, rig.state)
        model.openTextMessage()
        let units = String(repeating: "a", count: GuestSms.maximumUnits)
        let cases: [(sender: String, text: String, words: String?)] = [
            ("Andarium", "", nil),
            ("", "hello", SmsMessages.senderEmpty),
            ("A_B", "hello", SmsMessages.senderCharacters),
            ("Bank of the North", "hello", SmsMessages.senderLongName),
            (String(repeating: "1", count: 21), "hello", SmsMessages.senderLongNumber),
            ("Andarium", units + "a", SmsMessages.charactersOver(1)),
            ("Andarium", "a\u{FFFE}b", SmsMessages.unsendableCharacter),
        ]
        for (sender, text, words) in cases {
            model.setTextMessageSender(sender)
            model.setTextMessageText(text)
            #expect(!model.canSendTextMessage, "\(sender.debugDescription) \(text.count)")
            let status = state.status(availability: .ready)
            #expect(status?.words == words, "\(sender.debugDescription): \(status?.words ?? "none")")
            if words != nil { #expect(status?.tone == .problem) }
            model.sendTextMessage()
        }
        await settle()
        #expect(rig.driver.smsSent.isEmpty)
        // The room left shows once it runs short, and the full limit passes.
        model.setTextMessageSender("Andarium")
        model.setTextMessageText(units)
        #expect(model.canSendTextMessage)
        #expect(state.status(availability: .ready) == TextMessageStatus(words: SmsMessages.charactersLeft(0), tone: .info))
        model.setTextMessageText("short")
        #expect(state.status(availability: .ready) == nil)
    }

    // MARK: Without a runtime (the preview)

    @Test func aPreviewWithNoGuestPlaysTheSend() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        let model = rig.model
        model.driver = nil
        model.openTextMessage()
        model.setTextMessageText("one")
        model.sendTextMessage()
        #expect(rig.state.progress == .sending)
        #expect(await eventually { rig.state.progress == .sent })
        #expect(rig.said.lines == [SmsMessages.sentToAndroid])
    }

    // MARK: The views

    /// Hosts `view` in a window tall enough for it and gives the accessibility
    /// labels and UIKit text inputs it draws once it has settled.
    private func draw<Content: View>(
        _ view: Content, height: CGFloat = 900
    ) async throws -> (labels: [String], inputs: [(placeholder: String?, type: UITextContentType?)]) {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let container = UIViewController()
        lease.window.rootViewController = container
        let hosting = UIHostingController(rootView: view)
        container.addChild(hosting)
        hosting.view.frame = CGRect(x: 0, y: 0, width: 390, height: height)
        container.view.addSubview(hosting.view)
        hosting.didMove(toParent: container)
        defer {
            hosting.willMove(toParent: nil)
            hosting.view.removeFromSuperview()
            hosting.removeFromParent()
        }
        try await Task.sleep(for: .milliseconds(800))
        hosting.view.setNeedsLayout()
        hosting.view.layoutIfNeeded()
        var inputs: [(String?, UITextContentType?)] = []
        func walk(_ node: UIView) {
            if let field = node as? UITextField { inputs.append((field.placeholder, field.textContentType)) }
            if let text = node as? UITextView { inputs.append((text.accessibilityLabel, text.textContentType)) }
            node.subviews.forEach(walk)
        }
        walk(hosting.view)
        let labels = SideKeysTouchTests.elements(in: hosting.view).map(\.label)
        return (labels, inputs)
    }

    @Test func onlyTheMessageBoxHasTheOneTimeCodeContentType() async throws {
        let rig = try rig()
        defer { rig.cleanUp() }
        rig.model.openTextMessage()
        let drawn = try await draw(TextMessageBar().environment(rig.model))
        #expect(!drawn.inputs.isEmpty)
        let typed = drawn.inputs.filter { $0.type != nil }
        #expect(typed.count == 1 && typed.first?.type == .oneTimeCode, "\(drawn.inputs)")
        #expect(drawn.labels.contains(SmsMessages.from) && drawn.labels.contains(SmsMessages.message))
        #expect(drawn.labels.contains(SmsMessages.send) && drawn.labels.contains(SmsMessages.close))
    }

    @Test func settingsHasTheRowOnlyWhereTheApplicationCarriesTextMessages() async throws {
        let held = try rig(capabilities: .device)
        defer { held.cleanUp() }
        let heldLabels = try await draw(SessionSettingsSheet(large: true).environment(held.model), height: 2600).labels
        #expect(heldLabels.contains { $0.contains("ADB over network") })
        #expect(!heldLabels.contains { $0.contains(SmsMessages.row) }, "\(heldLabels)")

        var capabilities = Capabilities.device
        capabilities.textMessages = true
        let offered = try rig(capabilities: capabilities)
        defer { offered.cleanUp() }
        let offeredLabels = try await draw(SessionSettingsSheet(large: true).environment(offered.model), height: 2600).labels
        #expect(offeredLabels.contains { $0.contains(SmsMessages.row) }, "\(offeredLabels)")
    }
}
