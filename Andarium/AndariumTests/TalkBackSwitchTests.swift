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

    /// What VoiceOver is told of the model's own accord.
    private final class Said {
        var lines: [String] = []
        var queued: [Bool] = []
    }

    /// Listens to what the model tells VoiceOver. Call it before anything awaits:
    /// the first read of a start comes as soon as the test suspends.
    private func listen(to model: AppModel) -> Said {
        let said = Said()
        model.announcer = { text, queued in
            said.lines.append(text)
            said.queued.append(queued)
        }
        return said
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

    /// A read that began while a change ran saw Android half way: it never
    /// applies, whatever it answers.
    @Test func aReadThatBeganDuringAChangeNeverApplies() {
        let reader = ScreenReaderSession()
        let before = reading([])
        #expect(reader.apply(read: before, reader.beginRead()))
        #expect(reader.beginChange(turningOn: true))
        let during = reader.beginRead()
        #expect(!reader.apply(read: nil, during))
        #expect(!reader.apply(read: reading([talkBack]), during))
        #expect(reader.state == before)
        #expect(!reader.readFailed)
        reader.finishChange(reading([talkBack]))
        #expect(reader.isOn)
    }

    /// While a change runs the model does not ask Android at all: a read would
    /// wait behind it on the same channel.
    @Test func nothingIsReadWhileAChangeRuns() async throws {
        let (model, driver) = started()
        _ = await read(model)
        driver.screenReaderChangeDelay = .milliseconds(300)
        let reads = driver.screenReaderReads
        model.setScreenReader(true)
        try await settle { driver.screenReaderChanges.count == 1 }
        await model.refreshScreenReader()
        await model.refreshScreenReader()
        #expect(driver.screenReaderReads == reads, "a read began while the change ran")
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
        #expect(ScreenReaderWords.displayHint(model.session.reader, edge: nil)
            == ScreenReaderWords.sentences(ScreenReaderWords.displayHintDirect, ScreenReaderWords.wayBack(nil)))
    }

    /// The display is a direct-touch area exactly while Android's screen reader
    /// is on, so VoiceOver speaks and touches reach Android only then; a read
    /// that gets no answer leaves VoiceOver in charge.
    @Test func theDisplaysDirectTouchFollowsTheState() async throws {
        let (model, driver) = started(voiceOver: true, interval: .milliseconds(20))
        try await settle { model.session.reader.state != nil }
        #expect(GuestLayer.accessibility(of: model)?.directTouch == false)
        driver.screenReaderState = reading([talkBack])
        try await settle { model.session.reader.isOn }
        let on = GuestLayer.accessibility(of: model)
        #expect(on?.directTouch == true)
        #expect(on?.value == ScreenReaderWords.displayValue(model.session.reader))
        driver.screenReaderState = nil
        try await settle { model.session.reader.readFailed }
        #expect(GuestLayer.accessibility(of: model)?.directTouch == false)
        driver.screenReaderState = reading([other])
        try await settle { model.session.reader.state != nil }
        #expect(GuestLayer.accessibility(of: model)?.directTouch == false)
        // The display is described only while the session shows it.
        model.phase = .home
        #expect(GuestLayer.accessibility(of: model) == nil)
    }

    /// The hint VoiceOver reads on the direct-touch display ends with the way
    /// back, naming the edge the side keys are on.
    @Test func theHintOnTheDirectTouchDisplayEndsWithTheWayBack() async throws {
        let (model, _) = started(guestState: reading([talkBack]))
        model.session.keysEdge = .right
        _ = await read(model)
        let right = try #require(GuestLayer.accessibility(of: model)?.hint)
        #expect(right.hasSuffix(ScreenReaderWords.wayBack(.right)))
        #expect(right.hasPrefix(ScreenReaderWords.displayHintDirect))
        model.session.keysEdge = .left
        let left = try #require(GuestLayer.accessibility(of: model)?.hint)
        #expect(left.hasSuffix(ScreenReaderWords.wayBack(.left)))
        #expect(left != right)
        model.session.keysEdge = nil
        #expect(GuestLayer.accessibility(of: model)?.hint?.hasSuffix(ScreenReaderWords.wayBack(nil)) == true)
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

    // MARK: The way back

    @Test func theWayBackNamesTheEdgeTheKeysAreOn() {
        let right = ScreenReaderWords.wayBack(.right)
        let left = ScreenReaderWords.wayBack(.left)
        let none = ScreenReaderWords.wayBack(nil)
        #expect(Set([right, left, none]).count == 3)
        #expect(right.contains("right") && left.contains("left") && !none.contains("edge"))
        // The footer says it too, and follows the edge as the keys move.
        #expect(ScreenReaderWords.footer(.right).hasSuffix(right))
        #expect(ScreenReaderWords.footer(.left).hasSuffix(left))
        #expect(ScreenReaderWords.announcement(on: true, edge: .left).hasSuffix(left))
        #expect(ScreenReaderWords.announcement(on: false, edge: .left) == ScreenReaderWords.announcement(on: false, edge: .right))
    }

    // MARK: What VoiceOver is told

    @Test func turningItOnSaysSoOnceWithTheWayBackAndTurningItOffSaysSoOnce() async throws {
        let (model, _) = started(voiceOver: true)
        let said = listen(to: model)
        model.session.keysEdge = .right
        try await settle { model.session.reader.card == .ask }
        model.turnOnScreenReaderFromCard()
        try await settle { model.session.reader.isOn }
        #expect(said.lines == [ScreenReaderWords.announcement(on: true, edge: .right)])
        #expect(said.lines.first?.hasSuffix(ScreenReaderWords.wayBack(.right)) == true)
        // The loop reads on and finds it on: nothing more is said.
        try await Task.sleep(for: .milliseconds(100))
        #expect(said.lines.count == 1)
        model.setScreenReader(false)
        try await settle { !model.session.reader.isOn && model.session.reader.changing == nil }
        #expect(said.lines == [ScreenReaderWords.announcement(on: true, edge: .right), ScreenReaderWords.announcement(on: false, edge: .right)])
    }

    @Test func aChangeThatFailsSaysSoOnce() async throws {
        let (model, driver) = started(voiceOver: true)
        let said = listen(to: model)
        try await settle { model.session.reader.card == .ask }
        let reason = "The virtual machine couldn\u{2019}t change Android\u{2019}s screen reader."
        driver.screenReaderError = AdbFailure(message: reason)
        model.turnOnScreenReaderFromCard()
        try await settle { model.session.reader.cardFailure != nil }
        try await Task.sleep(for: .milliseconds(50))
        #expect(said.lines == [ScreenReaderWords.changeFailure(turningOn: true, reason: reason)])
    }

    @Test func theCardsTurnOnNeverDoesNothingSilently() async throws {
        let (model, driver) = started(voiceOver: true)
        let said = listen(to: model)
        try await settle { model.session.reader.card == .ask }
        // A transfer holds the channel: the card says so in Settings' words.
        model.busy = BusyState(title: "Working", detail: "Detail", progress: 0)
        model.turnOnScreenReaderFromCard()
        #expect(model.session.reader.cardFailure == ScreenReaderWords.waitsForTransfer)
        #expect(said.lines == [ScreenReaderWords.changeFailure(turningOn: true, reason: ScreenReaderWords.waitsForTransfer)])
        model.busy = nil
        // The last read got no answer: it says so and reads again.
        driver.screenReaderState = nil
        await model.refreshScreenReader()
        #expect(model.session.reader.state == nil)
        let reads = driver.screenReaderReads
        said.lines.removeAll()
        driver.screenReaderState = reading([])
        model.turnOnScreenReaderFromCard()
        #expect(model.session.reader.cardFailure == ScreenReaderWords.couldntCheck)
        #expect(said.lines == [ScreenReaderWords.changeFailure(turningOn: true, reason: ScreenReaderWords.couldntCheck)])
        try await settle { driver.screenReaderReads > reads }
        #expect(driver.screenReaderChanges.isEmpty)
        // The read answered: the note goes, and the next press turns it on.
        try await settle { model.session.reader.state != nil }
        #expect(model.session.reader.cardFailure == nil)
        model.turnOnScreenReaderFromCard()
        try await settle { model.session.reader.isOn }
        #expect(driver.screenReaderChanges == [.init(on: true, service: talkBack)])
    }

    // MARK: A start where the screen reader is on already

    @Test func aStartWhereTheScreenReaderIsOnTellsVoiceOverOnceWithTheWayBack() async throws {
        let (model, _) = started(voiceOver: true, guestState: reading([talkBack]), interval: .milliseconds(20))
        let said = listen(to: model)
        model.session.keysEdge = .right
        try await settle { !said.lines.isEmpty }
        // Reads go on every few milliseconds; Settings opens and closes.
        try await Task.sleep(for: .milliseconds(150))
        let reader = model.settingsOpened()
        try await Task.sleep(for: .milliseconds(60))
        model.settingsClosed(reader)
        try await Task.sleep(for: .milliseconds(60))
        #expect(said.lines == [ScreenReaderWords.announcement(on: true, edge: .right)])
        // After the run sentence, not over it; and no card asks.
        #expect(said.queued == [true])
        #expect(model.session.reader.card == nil)
    }

    @Test func aStartWhereTheScreenReaderIsOffTellsVoiceOverNothing() async throws {
        let (model, _) = started(voiceOver: true, interval: .milliseconds(20))
        let said = listen(to: model)
        try await settle { model.session.reader.card == .ask }
        try await Task.sleep(for: .milliseconds(100))
        #expect(said.lines.isEmpty)
    }

    @Test func withoutVoiceOverNothingIsSaid() async throws {
        let (model, _) = started(voiceOver: false, guestState: reading([talkBack]))
        let said = listen(to: model)
        _ = await read(model)
        #expect(model.session.reader.isOn)
        #expect(said.lines.isEmpty)
    }

    @Test func theSentenceAtStartNamesTheEdgeTheKeysAreOn() async throws {
        let (model, _) = started(voiceOver: true, guestState: reading([talkBack]), interval: .milliseconds(20))
        let said = listen(to: model)
        model.session.keysEdge = .left
        try await settle { !said.lines.isEmpty }
        let line = try #require(said.lines.first, "nothing was said")
        #expect(line == ScreenReaderWords.announcement(on: true, edge: .left))
        #expect(line.hasSuffix(ScreenReaderWords.wayBack(.left)))
    }

    // MARK: Turning it off is an answer

    @Test func turningItOffInSettingsKeepsTheCardAwayForTheSession() async throws {
        let (model, driver) = started(voiceOver: true, guestState: reading([talkBack]), interval: .milliseconds(20))
        try await settle { model.session.reader.isOn }
        let reader = model.settingsOpened()
        model.setScreenReader(false)
        try await settle { model.session.reader.changing == nil && !model.session.reader.isOn }
        model.settingsClosed(reader)
        try await Task.sleep(for: .milliseconds(150))
        #expect(model.session.reader.card == nil)
        #expect(driver.screenReaderChanges.map(\.on) == [false])
    }

    @Test func turningItOffInAndroidKeepsTheCardAwayForTheSession() async throws {
        let (model, driver) = started(voiceOver: true, guestState: reading([talkBack]), interval: .milliseconds(20))
        try await settle { model.session.reader.isOn }
        driver.screenReaderState = reading([])
        try await settle { model.session.reader.state != nil && !model.session.reader.isOn }
        try await Task.sleep(for: .milliseconds(150))
        #expect(model.session.reader.card == nil)
        // A session that never had it on asks, as before; and so does the next.
        model.powerOff()
        #expect(!model.session.reader.declined)
    }

    // MARK: The words

    @Test func theDisplaySaysWhatItHasAndOffersNoHintWhereThereIsNoScreenReader() async throws {
        let (model, driver) = started()
        let reader = model.session.reader
        #expect(ScreenReaderWords.displayValue(reader) == String(localized: "Checking Android\u{2019}s screen reader\u{2026}"))
        #expect(ScreenReaderWords.displayHint(reader, edge: .right) == nil)
        _ = await read(model)
        #expect(ScreenReaderWords.displayHint(reader, edge: .right) == ScreenReaderWords.displayHintOff)
        driver.screenReaderState = ScreenReaderState(installedService: nil, enabledServices: [])
        _ = await read(model)
        #expect(ScreenReaderWords.displayHint(reader, edge: .right) == nil)
        #expect(ScreenReaderWords.displayValue(reader) == String(localized: "No screen reader for Android in this system image"))
    }

    @Test func theWordsKeepTalkBackToTheTitle() {
        let edges: [KeysEdge?] = [.right, .left, nil]
        let explanations = edges.flatMap { edge in
            [
                ScreenReaderWords.footer(edge), ScreenReaderWords.wayBack(edge),
                ScreenReaderWords.announcement(on: true, edge: edge), ScreenReaderWords.announcement(on: false, edge: edge),
            ]
        } + [
            ScreenReaderWords.cantRead, ScreenReaderWords.canRead,
            ScreenReaderWords.noReader, ScreenReaderWords.displayHintDirect, ScreenReaderWords.displayHintOff,
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
