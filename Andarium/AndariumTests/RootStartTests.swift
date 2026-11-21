// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import Foundation
import Testing
@testable import Andarium

/// What a start does about root, as the model plays it against a driver that
/// records: the ramdisk the guest is written with before the runtime is
/// handed the start, and the boot screen's Magisk app step.
@MainActor
final class RootStartTests {
    // The model of a test adopts the process's App Intent hub, which the suites
    // of Add to Home Screen replace: these take turns with them (`AndroidAppsHubLock`).
    init() async { await AndroidAppsHubLock.shared.acquire() }
    deinit { Task { await AndroidAppsHubLock.shared.release() } }

    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )
    private let release = RootRelease(versionName: "v31.1", versionCode: 31100, apkPath: "root/Magisk-v31.1.apk")
    private let apk = URL(fileURLWithPath: "/release/root/Magisk-v31.1.apk")

    /// A guest shell that answers the package list with `listing` and
    /// remembers what it was asked.
    private final class Shell: GuestShell, @unchecked Sendable {
        private let lock = NSLock()
        private var asked: [String] = []
        let listing: String
        init(listing: String) { self.listing = listing }
        var commands: [String] { lock.withLock { asked } }
        func run(_ command: String, limit: Duration) async throws -> GuestShellOutput {
            lock.withLock { asked.append(command) }
            return GuestShellOutput(text: command.hasPrefix("pm list") ? listing : "", exitCode: 0)
        }
    }

    private func model(
        rootOn: Bool, available: Bool = true, shell: Shell? = nil, carries: Bool = true
    ) -> (AppModel, StubDriver) {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1"
        )
        var guest = Guest.fresh(on: host)
        guest.root.on = rootOn
        let model = AppModel(
            guest: guest, host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        if carries {
            driver.rootRelease = release
            driver.rootApkURL = apk
        }
        driver.guestShell = shell
        model.driver = driver
        model.rootAvailable = available
        model.refreshPlan()
        return (model, driver)
    }

    @Test func aStartWithRootOnIsWrittenAsRootedBeforeTheRuntimeIsHandedIt() {
        let (model, driver) = model(rootOn: true)
        model.start(.normal)
        #expect(driver.begunGuests.first?.root.rooted == true)
        #expect(driver.savedWhenBegun.first??.root.rooted == true)
        #expect(model.guest?.root.rooted == true)
    }

    @Test func noStartBootsRootedWithoutTheSwitchOrTheReleaseOrInRecovery() {
        let (off, offDriver) = model(rootOn: false)
        off.start(.normal)
        #expect(offDriver.begunGuests.first?.root.rooted == false)

        let (without, withoutDriver) = model(rootOn: true, available: false)
        without.start(.normal)
        #expect(withoutDriver.begunGuests.first?.root.rooted == false)

        let (recovery, recoveryDriver) = model(rootOn: true)
        recovery.start(.recovery)
        #expect(recoveryDriver.begunGuests.first?.root.rooted == false)

        let (paused, pausedDriver) = model(rootOn: true)
        paused.guest?.root.attention = .failedStart
        paused.start(.normal)
        #expect(pausedDriver.begunGuests.first?.root.rooted == false)
    }

    @Test func theStepPutsTheAppInWhenTheGuestHasNoneAndRemembersIt() async {
        let shell = Shell(listing: "")
        let (model, driver) = model(rootOn: true, shell: shell)
        model.start(.normal)
        var installing = 0
        await model.runRootAppStep(kind: .normal) { installing += 1 }
        #expect(installing == 1)
        #expect(driver.installURLs == [apk])
        #expect(model.rootAppInstalling)
        #expect(model.guest?.root.appVersionCode == 31100)
        #expect(model.alert == nil)
        // The next start's boot, with the app there at the carried version, installs nothing more.
        var again = 0
        driver.guestShell = Shell(listing: "package:com.topjohnwu.magisk versionCode:31100\n")
        model.rootAppInstalling = false
        await model.runRootAppStep(kind: .normal) { again += 1 }
        #expect(again == 0)
        #expect(driver.installURLs.count == 1)
        #expect(!model.rootAppInstalling)
    }

    @Test func aFailedInstallIsAnAlertAndTheBootGoesOnWithTheStateUnchanged() async {
        let shell = Shell(listing: "")
        let (model, driver) = model(rootOn: true, shell: shell)
        driver.install = { _ in throw InstanceError.refused("No.") }
        model.start(.normal)
        await model.runRootAppStep(kind: .normal) {}
        #expect(model.alert?.title == RootWords.appFailedTitle)
        #expect(model.guest?.root.appVersionCode == nil)
        #expect(model.guest?.root.on == true)
    }

    @Test func aStartWithoutRootAsksTheGuestNothing() async {
        let shell = Shell(listing: "")
        let (model, driver) = model(rootOn: false, shell: shell)
        model.start(.normal)
        await model.runRootAppStep(kind: .normal) {}
        #expect(shell.commands.isEmpty)
        #expect(driver.installURLs.isEmpty)
        #expect(model.alert == nil)
    }

    @Test func recoveryHasNoMagiskAppStep() async {
        let shell = Shell(listing: "")
        let (model, driver) = model(rootOn: true, shell: shell)
        model.start(.normal)
        await model.runRootAppStep(kind: .recovery) {}
        #expect(shell.commands.isEmpty)
        #expect(driver.installURLs.isEmpty)
    }

    // MARK: A start with root that did not finish

    private func diagnostics(record: String) throws -> URL {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("root-start-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try Data(record.utf8).write(to: directory.appendingPathComponent(BugReportAttachments.sessionRecord))
        return directory
    }

    private func record(end: String, status: Int = 0, boot: String = "unfinished") -> String {
        "uml-session 8\nstate ended\nend \(end)\nstatus \(status)\npid 1\nboot \(boot)\n"
    }

    @Test func aPanicBeforeAndroidBootedPausesRootAtLaunchAndSavesIt() throws {
        let (model, driver) = model(rootOn: true)
        model.guest?.root.rooted = true
        let directory = try diagnostics(record: record(end: "guest-panic"))
        defer { try? FileManager.default.removeItem(at: directory) }
        model.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: false)
        #expect(model.guest?.root.attention == .failedStart)
        #expect(model.guest?.root.rooted == false)
        #expect(model.guest?.root.on == true)
        #expect(driver.savedGuest?.root.attention == .failedStart)
        // The next start boots without root until the person chooses.
        model.start(.normal)
        #expect(driver.begunGuests.last?.root.rooted == false)
    }

    @Test func aLaunchBeforeTheDriverExistsSavesTheFallbackOnceItDoes() throws {
        let (model, driver) = model(rootOn: true)
        model.driver = nil
        model.guest?.root.rooted = true
        let directory = try diagnostics(record: record(end: "kernel-abort"))
        defer { try? FileManager.default.removeItem(at: directory) }
        model.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: false)
        #expect(model.guest?.root.attention == .failedStart)
        #expect(driver.savedGuest == nil)
        model.driver = driver
        model.saveRootStateIfPending()
        #expect(driver.savedGuest?.root.attention == .failedStart)
    }

    @Test func aPanicAfterAndroidBootedOrWithoutRootChangesNothing() throws {
        for (rooted, boot) in [(true, "completed"), (false, "unfinished")] {
            let (model, driver) = model(rootOn: true)
            model.guest?.root.rooted = rooted
            let directory = try diagnostics(record: record(end: "guest-panic", boot: boot))
            defer { try? FileManager.default.removeItem(at: directory) }
            model.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: false)
            #expect(model.guest?.root.attention == nil, "rooted \(rooted), boot \(boot)")
            #expect(driver.savedGuest == nil)
        }
    }

    @Test func aRestartLoopBeforeBootPausesRootAtTheEndOfTheRun() {
        let (model, driver) = model(rootOn: true)
        model.start(.normal)
        model.runEnded(RunEnd(result: -100, restartedInALoop: true), uptimeSeconds: 4)
        #expect(model.guest?.root.attention == .failedStart)
        #expect(driver.savedGuest?.root.attention == .failedStart)
    }

    @Test func aStopBeforeAndroidBootedOffersAStartWithoutRoot() {
        let (model, driver) = model(rootOn: true)
        model.start(.normal)
        model.runEnded(RunEnd(result: -5, stoppedByUser: true), uptimeSeconds: 200)
        #expect(model.guest?.root.attention == .stoppedStart)
        #expect(model.guest?.root.on == true)
        #expect(driver.savedGuest?.root.attention == .stoppedStart)
    }

    @Test func aRunThatBootedAndThenEndedChangesNothing() {
        let (model, _) = model(rootOn: true)
        model.start(.normal)
        model.bootTracker.reach(.ready, atSeconds: 30)
        model.runEnded(RunEnd(result: -100, restartedInALoop: true), uptimeSeconds: 600)
        #expect(model.guest?.root.attention == nil)
    }

    @Test func tryAgainEndsThePauseAndStartWithoutRootBootsStockOnce() {
        let (model, driver) = model(rootOn: true)
        model.guest?.root.attention = .failedStart
        model.tryRootAgain()
        #expect(model.guest?.root.attention == nil)
        #expect(model.guest?.root.on == true)
        model.start(.normal)
        #expect(driver.begunGuests.last?.root.rooted == true)

        let (stopped, stoppedDriver) = self.model(rootOn: true)
        stopped.guest?.root.attention = .stoppedStart
        stopped.start(.normal, withoutRoot: true)
        #expect(stoppedDriver.begunGuests.last?.root.rooted == false)
        #expect(stopped.guest?.root.attention == nil)
        #expect(stopped.guest?.root.on == true)
    }

    @Test func turnOffRootFromTheCardEndsThePauseAndTheSwitch() {
        let (model, driver) = model(rootOn: true)
        model.guest?.root.attention = .failedStart
        model.setRoot(false)
        #expect(model.guest?.root.on == false)
        #expect(model.guest?.root.attention == nil)
        #expect(driver.savedGuest?.root.on == false)
    }

    @Test func theBugReportCarriesTheRootLineOnlyWhereRootIsOffered() {
        let (offered, _) = model(rootOn: true)
        #expect(offered.rootReportEntry()?.name == "andarium/root.txt")
        let (hidden, _) = model(rootOn: false, available: false)
        #expect(hidden.rootReportEntry() == nil)
    }
}
