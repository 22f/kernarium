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
}
