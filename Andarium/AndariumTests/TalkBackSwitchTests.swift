// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import Foundation
import Testing
@testable import Andarium

/// Android's screen reader in the session: the switch in Settings shows what
/// Android reports and turns it on and off through the driver; a read that a
/// change overtook never overwrites it; every read and change belongs to the
/// session it began in; and nothing turns the screen reader on but the person,
/// on the switch or on the card that asks while VoiceOver runs.
@MainActor
struct TalkBackSwitchTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )
    private let system = SystemInfo(
        version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1")
    private let talkBack = ScreenReader.services[0]
    private let other = "com.example.reader/com.example.reader.Service"

    /// A guest that ran `plan`'s start to its end, with VoiceOver as given.
    private func started(
        plan: StartPlan = .normal, voiceOver: Bool = false, mode: StartMode = .normal,
        guestState: ScreenReaderState? = nil, interval: Duration = .seconds(3)
    ) -> (AppModel, StubDriver) {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: plan)
        if let guestState { driver.screenReaderState = guestState }
        model.driver = driver
        model.voiceOverRunning = { voiceOver }
        model.screenReaderWatchInterval = interval
        model.refreshPlan()
        model.start(mode)
        model.finishBoot(model.bootKind(mode))
        return (model, driver)
    }

    private func reading(_ services: [String], installed: String? = nil) -> ScreenReaderState {
        ScreenReaderState(installedService: installed ?? talkBack, enabledServices: services)
    }

    /// Waits, within a bound, until `done` holds.
    private func settle(_ done: () -> Bool) async throws {
        let deadline = ContinuousClock.now + .seconds(5)
        while !done(), ContinuousClock.now < deadline {
            try await Task.sleep(for: .milliseconds(10))
        }
    }

    /// Reads once, as Settings does, and returns what the switch shows.
    private func read(_ model: AppModel) async -> ScreenReaderRow {
        await model.refreshScreenReader()
        return model.screenReaderRow
    }

    // MARK: The switch

    @Test func theSwitchShowsWhatAndroidReports() async throws {
        let (model, driver) = started()
        #expect(model.screenReaderRow == .checking)
        #expect(await read(model) == .off)
        // A change made in Android's own settings reaches the switch.
        driver.screenReaderState = reading([talkBack])
        #expect(await read(model) == .on)
        #expect(model.session.reader.isOn)
    }

    @Test func aReadThatGetsNoAnswerIsUnknownNotTheLastState() async throws {
        let (model, driver) = started()
        #expect(await read(model) == .off)
        driver.screenReaderState = reading([talkBack])
        #expect(await read(model) == .on)
        driver.screenReaderState = nil
        #expect(await read(model) == .couldntCheck)
        #expect(!model.session.reader.isOn)
        #expect(!model.screenReaderCanChange)
        // Try Again reads again.
        driver.screenReaderState = reading([])
        #expect(await read(model) == .off)
        #expect(model.screenReaderCanChange)
    }

    @Test func theSwitchTurnsTheScreenReaderOnAndOffNamingTheService() async throws {
        let (model, driver) = started()
        _ = await read(model)
        model.setScreenReader(true)
        try await settle { model.session.reader.changing == nil && model.screenReaderRow == .on }
        #expect(driver.screenReaderChanges == [.init(on: true, service: talkBack)])
        // Asking for the state it is in already changes nothing.
        model.setScreenReader(true)
        #expect(driver.screenReaderChanges.count == 1)
        model.setScreenReader(false)
        try await settle { model.session.reader.changing == nil && model.screenReaderRow == .off }
        #expect(driver.screenReaderChanges.map(\.on) == [true, false])
        // No alert closes Settings: a failure is told where the person is.
        #expect(model.alert == nil)
    }

    @Test func aSystemWithoutAScreenReaderOffersNoSwitchToTurn() async throws {
        let (model, driver) = started(guestState: ScreenReaderState(installedService: nil, enabledServices: []))
        #expect(await read(model) == .unavailable)
        #expect(!model.screenReaderCanChange)
        model.setScreenReader(true)
        try await Task.sleep(for: .milliseconds(50))
        #expect(driver.screenReaderChanges.isEmpty)
    }

    @Test func recoveryHasNoScreenReader() async throws {
        let (model, driver) = started(voiceOver: true, mode: .recovery)
        #expect(!model.offersScreenReader)
        await model.refreshScreenReader()
        model.setScreenReader(true)
        try await Task.sleep(for: .milliseconds(50))
        #expect(driver.screenReaderReads == 0 && driver.screenReaderChanges.isEmpty)
        #expect(model.session.reader.card == nil)
    }

    @Test func aFileTransferHoldsTheSwitchAndSaysUntilWhen() async throws {
        let (model, driver) = started()
        #expect(await read(model) == .off)
        model.busy = BusyState(title: "Working", detail: "Detail", progress: 0)
        #expect(model.screenReaderRow == .waitsForTransfer)
        #expect(!model.screenReaderCanChange)
        model.setScreenReader(true)
        await model.refreshScreenReader()
        #expect(driver.screenReaderChanges.isEmpty)
        #expect(driver.screenReaderReads == 1)
        model.busy = nil
        #expect(model.screenReaderRow == .off)
    }

    // MARK: Failures

    @Test func aChangeThatFailsSaysSoOnTheRowAndReadsAgain() async throws {
        let (model, driver) = started()
        _ = await read(model)
        let reason = "The virtual machine couldn\u{2019}t change Android\u{2019}s screen reader."
        driver.screenReaderError = AdbFailure(message: reason)
        let reads = driver.screenReaderReads
        model.setScreenReader(true)
        try await settle { model.session.reader.failure != nil && driver.screenReaderReads > reads }
        #expect(model.screenReaderRow == .failed(turningOn: true, reason: reason))
        #expect(model.screenReaderRow.line == ScreenReaderWords.didntChange(turningOn: true))
        #expect(model.alert == nil)
        #expect(!model.session.reader.isOn)
        // Try Again after the cause is gone.
        driver.screenReaderError = nil
        model.setScreenReader(true)
        try await settle { model.screenReaderRow == .on }
        #expect(model.session.reader.failure == nil)
    }

    // MARK: Reads and changes that overlap

    @Test func aReadThatAnsweredAfterAChangeNeverOverwritesIt() async throws {
        let (model, driver) = started()
        _ = await read(model)
        // The next read leaves before the change and answers after it ended.
        driver.screenReaderReadDelays = [.zero, .milliseconds(400)]
        let late = Task { await model.refreshScreenReader() }
        try await settle { driver.screenReaderReads == 2 }
        model.setScreenReader(true)
        try await settle { model.session.reader.changing == nil && model.session.reader.isOn }
        await late.value
        #expect(model.screenReaderRow == .on)
        #expect(model.session.reader.isOn)
    }

    @Test func aReadThatAnsweredWhileAChangeWasUnderWayNeverOverwritesIt() async throws {
        let (model, driver) = started()
        _ = await read(model)
        driver.screenReaderReadDelays = [.zero, .milliseconds(100)]
        driver.screenReaderChangeDelay = .milliseconds(400)
        let early = Task { await model.refreshScreenReader() }
        try await settle { driver.screenReaderReads == 2 }
        model.setScreenReader(true)
        await early.value
        // Answered and dropped while the change ran: nothing shown is stale.
        #expect(model.screenReaderRow == .switching(turningOn: true))
        try await settle { model.session.reader.changing == nil }
        #expect(model.screenReaderRow == .on)
    }

    @Test func aNewerReadIsNotOverwrittenByAnOlderOne() async throws {
        let (model, driver) = started()
        _ = await read(model)
        // The older read answers last, with what Android held before.
        driver.screenReaderReadDelays = [.zero, .milliseconds(300)]
        let older = Task { await model.refreshScreenReader() }
        try await settle { driver.screenReaderReads == 2 }
        driver.screenReaderState = reading([talkBack])
        await model.refreshScreenReader()
        #expect(model.screenReaderRow == .on)
        await older.value
        #expect(model.screenReaderRow == .on)
    }

    @Test func aChangeKeepsAServiceTurnedOnInAndroidSinceTheLastRead() async throws {
        let (model, driver) = started()
        _ = await read(model)
        // Android's own settings turned another service on since the read.
        driver.screenReaderState = reading([other])
        model.setScreenReader(true)
        try await settle { model.session.reader.changing == nil && model.session.reader.isOn }
        #expect(driver.screenReaderState?.enabledServices == [other, talkBack])
        model.setScreenReader(false)
        try await settle { model.session.reader.changing == nil && !model.session.reader.isOn }
        #expect(driver.screenReaderState?.enabledServices == [other])
    }

    // MARK: A session's own

    @Test func aChangeThatEndsAfterTheRunLeavesTheNextSessionAlone() async throws {
        let (model, driver) = started()
        _ = await read(model)
        driver.screenReaderChangeDelay = .milliseconds(300)
        let old = model.session
        model.setScreenReader(true)
        try await settle { driver.screenReaderChanges.count == 1 }
        model.powerOff()
        #expect(model.session !== old)
        try await Task.sleep(for: .milliseconds(500))
        #expect(model.session.reader.state == nil)
        #expect(model.session.reader.changing == nil)
        #expect(!old.reader.isOn)
        #expect(model.alert == nil)
    }

    @Test func aFailedChangeAfterTheRunSaysNothingToTheNextSession() async throws {
        let (model, driver) = started()
        _ = await read(model)
        driver.screenReaderChangeDelay = .milliseconds(200)
        driver.screenReaderError = AdbFailure(.io(EIO), activity: .screenReader)
        model.setScreenReader(true)
        try await settle { driver.screenReaderChanges.count == 1 }
        model.powerOff()
        try await Task.sleep(for: .milliseconds(400))
        #expect(model.session.reader.failure == nil)
        #expect(model.alert == nil)
    }

    @Test func theLoopThatReadsEndsWithTheSession() async throws {
        let (model, driver) = started(voiceOver: true, interval: .milliseconds(20))
        try await settle { driver.screenReaderReads >= 3 }
        let old = model.session
        model.powerOff()
        #expect(old.reader.watcher == nil)
        let reads = driver.screenReaderReads
        try await Task.sleep(for: .milliseconds(200))
        #expect(driver.screenReaderReads <= reads + 1)
    }

    // MARK: Reading only when it is needed

    @Test func withoutVoiceOverNothingReadsUntilSettingsOpens() async throws {
        let (model, driver) = started(voiceOver: false, interval: .milliseconds(20))
        try await Task.sleep(for: .milliseconds(150))
        #expect(driver.screenReaderReads == 0)
        let watcher = Task { await model.watchSettings() }
        defer { watcher.cancel() }
        try await settle { driver.screenReaderReads > 0 }
        #expect(driver.screenReaderReads > 0)
        #expect(model.session.reader.settingsOpen == 1)
        watcher.cancel()
        await watcher.value
        #expect(model.session.reader.settingsOpen == 0)
    }

    @Test func underVoiceOverTheDisplayFollowsAChangeMadeInAndroid() async throws {
        let (model, driver) = started(voiceOver: true, interval: .milliseconds(20))
        try await settle { model.session.reader.state != nil }
        driver.screenReaderState = reading([talkBack])
        try await settle { model.session.reader.isOn }
        #expect(model.session.reader.isOn)
        #expect(ScreenReaderWords.displayHint(model.session.reader) == ScreenReaderWords.displayHintDirect)
    }

    // MARK: Nothing turns on by itself

    @Test func aFirstStartUnderVoiceOverAsksAndTurnsNothingOn() async throws {
        let (model, driver) = started(plan: .firstStart, voiceOver: true)
        try await settle { model.session.reader.card == .ask }
        try await Task.sleep(for: .milliseconds(100))
        #expect(model.session.reader.card == .ask)
        #expect(driver.screenReaderChanges.isEmpty)
        #expect(!model.session.reader.isOn)
    }

    @Test func aFirstStartWithoutVoiceOverShowsNoCardAndLeavesItOff() async throws {
        let (model, driver) = started(plan: .firstStart, voiceOver: false)
        #expect(await read(model) == .off)
        #expect(model.session.reader.card == nil)
        #expect(driver.screenReaderChanges.isEmpty)
    }

    @Test func theCardsTurnOnTurnsItOnAndGoes() async throws {
        let (model, driver) = started(voiceOver: true)
        try await settle { model.session.reader.card == .ask }
        model.turnOnScreenReaderFromCard()
        try await settle { model.session.reader.isOn }
        #expect(driver.screenReaderChanges == [.init(on: true, service: talkBack)])
        #expect(model.session.reader.card == nil)
    }

    @Test func aTurnOnThatFailsKeepsTheCardWithTheReason() async throws {
        let (model, driver) = started(voiceOver: true)
        try await settle { model.session.reader.card == .ask }
        let reason = "The virtual machine couldn\u{2019}t change Android\u{2019}s screen reader."
        driver.screenReaderError = AdbFailure(message: reason)
        model.turnOnScreenReaderFromCard()
        try await settle { model.session.reader.cardFailure != nil }
        #expect(model.session.reader.card == .ask)
        #expect(model.session.reader.cardFailure == reason)
        // Try Again, once the cause is gone.
        driver.screenReaderError = nil
        model.turnOnScreenReaderFromCard()
        try await settle { model.session.reader.isOn }
        #expect(model.session.reader.card == nil)
        #expect(model.session.reader.cardFailure == nil)
    }

    @Test func notNowKeepsTheCardAwayForTheRestOfTheSession() async throws {
        let (model, driver) = started(voiceOver: true, interval: .milliseconds(20))
        try await settle { model.session.reader.card == .ask }
        model.declineScreenReaderCard()
        #expect(model.session.reader.card == nil)
        let reads = driver.screenReaderReads
        try await settle { driver.screenReaderReads > reads + 2 }
        #expect(model.session.reader.card == nil)
        #expect(driver.screenReaderChanges.isEmpty)
        // A new session asks again.
        model.powerOff()
        #expect(!model.session.reader.declined)
    }

    @Test func noCardWhereTheScreenReaderIsAlreadyOn() async throws {
        let (model, _) = started(voiceOver: true, guestState: reading([talkBack]))
        try await settle { model.session.reader.isOn }
        try await Task.sleep(for: .milliseconds(50))
        #expect(model.session.reader.card == nil)
    }

    @Test func aSystemWithoutOneIsToldOnceAndNeverAskedToTurnItOn() async throws {
        let (model, driver) = started(
            voiceOver: true, guestState: ScreenReaderState(installedService: nil, enabledServices: []),
            interval: .milliseconds(20))
        try await settle { model.session.reader.card == .unavailable }
        model.dismissScreenReaderCard()
        let reads = driver.screenReaderReads
        try await settle { driver.screenReaderReads > reads + 2 }
        #expect(model.session.reader.card == nil)
        #expect(driver.screenReaderChanges.isEmpty)
    }

    @Test func theCardDoesNotRiseOverSettings() async throws {
        let (model, _) = started(voiceOver: true, interval: .milliseconds(20))
        let reader = model.settingsOpened()
        try await Task.sleep(for: .milliseconds(150))
        #expect(model.session.reader.state != nil)
        #expect(model.session.reader.card == nil)
        model.settingsClosed(reader)
        #expect(model.session.reader.card == .ask)
    }

    @Test func theCardGoesWhenVoiceOverTurnsOff() async throws {
        let (model, driver) = started(voiceOver: true)
        try await settle { model.session.reader.card == .ask }
        var running = false
        model.voiceOverRunning = { running }
        model.voiceOverStatusChanged()
        #expect(model.session.reader.card == nil)
        #expect(model.session.reader.watcher == nil)
        #expect(driver.screenReaderChanges.isEmpty)
        // And back on: the loop reads again and the card follows.
        running = true
        model.voiceOverStatusChanged()
        try await settle { model.session.reader.card == .ask }
    }

    // MARK: The words

    @Test func theDisplaySaysWhatItHasAndOffersNoHintWhereThereIsNoScreenReader() async throws {
        let (model, driver) = started()
        let reader = model.session.reader
        #expect(ScreenReaderWords.displayValue(reader) == String(localized: "Checking Android\u{2019}s screen reader\u{2026}"))
        #expect(ScreenReaderWords.displayHint(reader) == nil)
        _ = await read(model)
        #expect(ScreenReaderWords.displayHint(reader) == ScreenReaderWords.displayHintOff)
        driver.screenReaderState = ScreenReaderState(installedService: nil, enabledServices: [])
        _ = await read(model)
        #expect(ScreenReaderWords.displayHint(reader) == nil)
        #expect(ScreenReaderWords.displayValue(reader) == String(localized: "No screen reader for Android in this system image"))
    }

    @Test func theWordsKeepTalkBackToTheTitle() {
        let explanations = [
            ScreenReaderWords.footer, ScreenReaderWords.cantRead, ScreenReaderWords.canRead, ScreenReaderWords.wayBack,
            ScreenReaderWords.noReader, ScreenReaderWords.displayHintDirect, ScreenReaderWords.displayHintOff,
            ScreenReaderWords.announcement(on: true), ScreenReaderWords.announcement(on: false),
            ScreenReaderWords.changeFailure(turningOn: true, reason: ""), ScreenReaderWords.notReady,
        ]
        for text in explanations {
            #expect(!text.contains("TalkBack"), "\(text)")
            #expect(!text.localizedCaseInsensitiveContains("guest"), "\(text)")
        }
        #expect(ScreenReaderWords.name == "TalkBack")
        #expect(ScreenReaderWords.askTitle.contains("TalkBack"))
    }
}
