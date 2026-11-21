// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The root switch's model: what a release carries, what the guest keeps of
/// the switch, the three rules the owner may change, the Magisk app check and
/// the boot screen's install step. The release files here are made up: no
/// Magisk file is in the repository.
struct RootReleaseTests {
    private func manifest(
        version: Any = 31_100, name: String = "v31.1", apk: String = "root/Magisk-v31.1.apk",
        image: String = "root/init_boot-magisk.img", withRoot: Bool = true
    ) -> Data {
        var object: [String: Any] = ["schema": 8, "release_id": "lineage24-test"]
        if withRoot {
            object["root"] = [
                "magisk": ["version_name": name, "version_code": version],
                "apk": ["path": apk, "sha256": String(repeating: "a", count: 64), "size": 10],
                "boot_image": ["path": image, "sha256": String(repeating: "b", count: 64), "size": 12],
            ]
        }
        return try! JSONSerialization.data(withJSONObject: object)
    }

    @Test func readsTheRootObjectOfTheManifest() throws {
        let release = try #require(RootRelease(manifest: manifest()))
        #expect(release == RootRelease(versionName: "v31.1", versionCode: 31_100, apkPath: "root/Magisk-v31.1.apk"))
        #expect(RootRelease.bootImage == "root/init_boot-magisk.img")
    }

    @Test func aReleaseWithoutRootOrWithAnUnreadableOneIsNoRoot() {
        #expect(RootRelease(manifest: manifest(withRoot: false)) == nil)
        #expect(RootRelease(manifest: Data("not json".utf8)) == nil)
        for version: Any in [0, -1, "31100", true, 3.5 as Double] where !(version is Double) {
            #expect(RootRelease(manifest: manifest(version: version)) == nil, "version \(version)")
        }
        #expect(RootRelease(manifest: manifest(name: "")) == nil)
        // The patched image is the one name the runtime reads.
        #expect(RootRelease(manifest: manifest(image: "root/other.img")) == nil)
        for path in ["Magisk.apk", "/root/Magisk.apk", "root/../Magisk.apk", "root//Magisk.apk", "root/./a.apk", "root/", "other/Magisk.apk"] {
            #expect(RootRelease(manifest: manifest(apk: path)) == nil, "path \(path)")
        }
    }

    @Test func aPartialRootDirectoryIsNoRoot() throws {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("root-release-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: folder) }
        try FileManager.default.createDirectory(at: folder.appendingPathComponent("root"), withIntermediateDirectories: true)
        let data = manifest()
        #expect(RootRelease.carried(in: folder, manifest: data) == nil)
        try Data("ramdisk".utf8).write(to: folder.appendingPathComponent(RootRelease.bootImage))
        #expect(RootRelease.carried(in: folder, manifest: data) == nil)
        try Data("app".utf8).write(to: folder.appendingPathComponent("root/Magisk-v31.1.apk"))
        let release = try #require(RootRelease.carried(in: folder, manifest: data))
        #expect(release.apkURL(in: folder).lastPathComponent == "Magisk-v31.1.apk")
        // A link in the place of the image is not the release's own file.
        let image = folder.appendingPathComponent(RootRelease.bootImage)
        let elsewhere = folder.appendingPathComponent("elsewhere.img")
        try Data("ramdisk".utf8).write(to: elsewhere)
        try FileManager.default.removeItem(at: image)
        try FileManager.default.createSymbolicLink(at: image, withDestinationURL: elsewhere)
        #expect(RootRelease.carried(in: folder, manifest: data) == nil)
    }
}

struct RootStateTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    @Test func aGuestStoredBeforeTheSwitchHasRootOff() throws {
        let old = """
            {"cores":6,"memoryMiB":4096,"capacityGiB":8,"microphone":false,"includeInBackups":false}
            """
        let guest = try JSONDecoder().decode(Guest.self, from: Data(old.utf8))
        #expect(guest.root == RootState())
        #expect(!guest.root.on && !guest.root.rooted && guest.root.attention == nil)
    }

    @Test func theStateIsKeptWithTheGuest() throws {
        var guest = Guest.fresh(on: host)
        guest.root.setOn(true)
        guest.root.rooted = true
        guest.root.appVersionCode = 31_100
        guest.root.appEverHandled = true
        guest.root.attention = .stoppedStart
        let back = try JSONDecoder().decode(Guest.self, from: JSONEncoder().encode(guest))
        #expect(back.root == guest.root)
        // A stored value this reader does not know is no attention.
        let odd = """
            {"cores":6,"memoryMiB":4096,"capacityGiB":8,"microphone":false,"includeInBackups":false,
             "root":{"on":true,"attention":"something-new"}}
            """
        let read = try JSONDecoder().decode(Guest.self, from: Data(odd.utf8))
        #expect(read.root.on && read.root.attention == nil)
    }

    @Test func turningRootOffEndsThePeriodAndWhatWaitedForADecision() {
        var root = RootState()
        root.setOn(true)
        root.appVersionCode = 31_100
        root.appEverHandled = true
        root.attention = .failedStart
        root.setOn(false)
        #expect(!root.on && root.attention == nil && root.appVersionCode == nil)
        // The guest still has had the app.
        #expect(root.appEverHandled)
        root.attention = .failedStart
        root.tryAgain()
        #expect(root.on && root.attention == nil && !root.paused)
    }

    @Test func aStartBootsRootedOnlyWhenEverythingAllowsIt() {
        var guest = Guest.fresh(on: host)
        #expect(!guest.bootsRooted(mode: .normal, releaseCarriesRoot: true))
        guest.root.setOn(true)
        #expect(guest.bootsRooted(mode: .normal, releaseCarriesRoot: true))
        #expect(!guest.bootsRooted(mode: .normal, releaseCarriesRoot: false), "a release with no root")
        #expect(!guest.bootsRooted(mode: .recovery, releaseCarriesRoot: true), "recovery has its own ramdisk")
        #expect(!guest.bootsRooted(mode: .normal, releaseCarriesRoot: true, withoutRootOnce: true))
        guest.root.attention = .failedStart
        #expect(!guest.bootsRooted(mode: .normal, releaseCarriesRoot: true), "paused after a failed start")
        guest.root.attention = .stoppedStart
        #expect(guest.bootsRooted(mode: .normal, releaseCarriesRoot: true), "a stop is no failure")
        guest.root.attention = .turnedOffAfterFailure
        guest.root.on = false
        #expect(!guest.bootsRooted(mode: .normal, releaseCarriesRoot: true))
    }

    @Test func aFactoryResetKeepsTheChoiceAndForgetsTheApp() {
        var guest = Guest.fresh(on: host)
        guest.root.setOn(true)
        guest.root.rooted = true
        guest.root.appVersionCode = 31_100
        guest.root.appEverHandled = true
        guest.root.attention = .failedStart
        let erased = guest.erased
        #expect(erased.root.on && erased.root.rooted)
        #expect(erased.root.attention == nil && erased.root.appVersionCode == nil && !erased.root.appEverHandled)
    }
}

struct MagiskAppTests {
    private func output(_ text: String, exit: Int? = 0) -> GuestShellOutput { GuestShellOutput(text: text, exitCode: exit) }

    @Test func readsTheVersionCodeOfTheExactPackage() {
        #expect(MagiskApp.presence(of: output("")) == .absent)
        #expect(MagiskApp.presence(of: output("package:com.topjohnwu.magisk versionCode:31100\n")) == .installed(versionCode: 31_100))
        #expect(MagiskApp.presence(of: output("package:com.topjohnwu.magisk.stub versionCode:1\n")) == .absent)
        #expect(
            MagiskApp.presence(
                of: output("package:com.topjohnwu.magisk.stub versionCode:1\npackage:com.topjohnwu.magisk versionCode:27000\r\n"))
                == .installed(versionCode: 27_000))
    }

    @Test func anAnswerThatIsNotTheLineIsNoAnswer() {
        #expect(MagiskApp.presence(of: output("", exit: 1)) == .unknown)
        #expect(MagiskApp.presence(of: output("", exit: nil)) == .unknown)
        #expect(MagiskApp.presence(of: output("Error: unknown option --show-versioncode")) == .unknown)
        #expect(MagiskApp.presence(of: output("package:com.topjohnwu.magisk\n")) == .unknown)
        #expect(MagiskApp.presence(of: output("package:com.topjohnwu.magisk versionCode:abc\n")) == .unknown)
        #expect(MagiskApp.presence(of: output("package:com.topjohnwu.magisk versionCode:-4\n")) == .unknown)
    }

    @Test func theCommandsNameOnlyThePackage() {
        #expect(MagiskApp.probeCommand == "pm list packages --show-versioncode com.topjohnwu.magisk")
        #expect(MagiskApp.openCommand.hasSuffix("-p com.topjohnwu.magisk"))
        #expect(MagiskApp.uninstallCommand.hasSuffix(" com.topjohnwu.magisk"))
    }
}

struct RootRulesTests {
    private func state(period: Int? = nil, ever: Bool = false) -> RootState {
        var state = RootState()
        state.on = true
        state.appVersionCode = period
        state.appEverHandled = ever
        return state
    }

    @Test func anAbsentAppIsInstalledOncePerPeriodByDefault() {
        #expect(RootInstallRule.policy == .oncePerOnPeriod)
        #expect(RootInstallRule.opensAppAfterInstall)
        #expect(RootInstallRule.decide(presence: .absent, carriedVersionCode: 31_100, state: state()) == .install)
        #expect(RootInstallRule.decide(presence: .unknown, carriedVersionCode: 31_100, state: state()) == .install)
        // The person removed it after the application put it in: it stays removed.
        #expect(RootInstallRule.decide(presence: .absent, carriedVersionCode: 31_100, state: state(period: 31_100)) == .none)
        // An older app is updated whatever the period says.
        #expect(RootInstallRule.decide(presence: .installed(versionCode: 27_000), carriedVersionCode: 31_100, state: state(period: 27_000)) == .install)
        #expect(RootInstallRule.decide(presence: .installed(versionCode: 31_100), carriedVersionCode: 31_100, state: state()) == .none)
        #expect(RootInstallRule.decide(presence: .installed(versionCode: 32_000), carriedVersionCode: 31_100, state: state()) == .none)
    }

    @Test func theOtherChoicesOfTheDecisionAreOneConstantAway() {
        for (policy, period, ever, expected) in [
            (RootInstallRule.Policy.everyRootedBoot, 31_100 as Int?, true, RootInstallRule.Action.install),
            (.oncePerGuest, nil, false, .install),
            (.oncePerGuest, nil, true, .none),
            (.oncePerGuest, 31_100, true, .none),
            (.oncePerOnPeriod, nil, true, .install),
        ] {
            #expect(
                RootInstallRule.decide(
                    presence: .absent, carriedVersionCode: 31_100, state: state(period: period, ever: ever), policy: policy)
                    == expected, "\(policy) \(String(describing: period)) \(ever)")
        }
    }

    @Test func theRuleRemembersWhatItPutInOrSaw() {
        let first = RootInstallRule.recorded(state(), versionCode: 31_100)
        #expect(first.appVersionCode == 31_100 && first.appEverHandled)
        // A lower code seen later does not lower the memory.
        #expect(RootInstallRule.recorded(first, versionCode: 27_000).appVersionCode == 31_100)
    }

    @Test func rootOffKeepsTheAppByDefault() {
        #expect(RootOffRule.appPolicy == .keep)
        var off = state(period: nil, ever: true)
        off.on = false
        #expect(!RootOffRule.removesApp(state: off))
        #expect(RootOffRule.removesApp(state: off, policy: .uninstall))
        // Nothing to remove when the application never put the app in, and never while root is on.
        #expect(!RootOffRule.removesApp(state: RootState(), policy: .uninstall))
        #expect(!RootOffRule.removesApp(state: state(ever: true), policy: .uninstall))
    }

    @Test func aFailedStartPausesRootByDefault() {
        #expect(RootFallbackRule.behaviour == .bootStockOnce)
        var root = state()
        root.rooted = true
        #expect(RootFallbackRule.failed(&root))
        #expect(root.on && !root.rooted && root.paused && root.attention == .failedStart)
        var other = state()
        #expect(RootFallbackRule.failed(&other, behaviour: .turnOff))
        #expect(!other.on && other.attention == .turnedOffAfterFailure)
        var none = state()
        #expect(!RootFallbackRule.failed(&none, behaviour: .none))
        #expect(none.on && none.attention == nil)
    }

    @Test func aStopBeforeBootOffersAStartWithoutRoot() {
        var root = state()
        RootFallbackRule.stopped(&root)
        #expect(root.attention == .stoppedStart && root.on)
        // It never replaces a decision that waits.
        var paused = state()
        paused.attention = .failedStart
        RootFallbackRule.stopped(&paused)
        #expect(paused.attention == .failedStart)
    }

    @Test func aStartWithoutRootRemovesTheAppOnlyWhenTheRuleSaysSo() async {
        let present = RootScriptedShell { command in
            command == MagiskApp.probeCommand
                ? GuestShellOutput(text: "package:com.topjohnwu.magisk versionCode:31100\n", exitCode: 0)
                : GuestShellOutput(text: "Success\n", exitCode: 0)
        }
        let removedPresent = await RootOffRule.removeAppIfAny(shell: present, limit: .seconds(1))
        #expect(removedPresent)
        #expect(present.commands == [MagiskApp.probeCommand, MagiskApp.uninstallCommand])
        let absent = RootScriptedShell { _ in GuestShellOutput(text: "", exitCode: 0) }
        let removedAbsent = await RootOffRule.removeAppIfAny(shell: absent, limit: .seconds(1))
        #expect(removedAbsent)
        #expect(absent.commands == [MagiskApp.probeCommand])
        let unreadable = await RootOffRule.removeAppIfAny(shell: RootScriptedShell { _ in GuestShellOutput(text: "", exitCode: 1) }, limit: .seconds(1))
        #expect(!unreadable)
    }
}

struct RootFallbackRecordTests {
    private func record(end: String, status: Int = 0, boot: String? = "unfinished", running: Bool = false) throws -> SessionRecord {
        var text = "uml-session 8\nstate \(running ? "running" : "ended")\nend \(end)\nstatus \(status)\npid 1\n"
        if let boot { text += "boot \(boot)\n" }
        return try #require(SessionRecord(text: text))
    }

    @Test func aKernelThatEndedTheStartBeforeAndroidBootedIsAFailedStart() throws {
        #expect(RootFallbackRule.endedBeforeBoot(try record(end: "guest-panic")))
        #expect(RootFallbackRule.endedBeforeBoot(try record(end: "kernel-abort")))
        #expect(RootFallbackRule.endedBeforeBoot(try record(end: "kernel-exit", status: 1)))
    }

    @Test func everyOtherEndTakesTheOrdinaryPaths() throws {
        // Android had booted, or the record does not say.
        #expect(!RootFallbackRule.endedBeforeBoot(try record(end: "guest-panic", boot: "completed")))
        #expect(!RootFallbackRule.endedBeforeBoot(try record(end: "guest-panic", boot: nil)))
        // A kernel's clean exit, a stop, a power-off.
        #expect(!RootFallbackRule.endedBeforeBoot(try record(end: "kernel-exit", status: 0)))
        for end in ["powered-off", "restarted", "stopped", "host-fault", "hibernation-restore-failed", "process-exit", "failed"] {
            let ended = try record(end: end, status: 1)
            #expect(!RootFallbackRule.endedBeforeBoot(ended), "\(end)")
        }
        // The system or the person ended the process: the record is still running.
        #expect(!RootFallbackRule.endedBeforeBoot(try record(end: "running", running: true)))
        #expect(!RootFallbackRule.endedBeforeBoot(try record(end: "guest-panic", running: true)))
    }

    @Test func aRestartLoopBeforeBootIsAFailedStartToo() {
        #expect(RootFallbackRule.endedBeforeBoot(RunEnd(result: -100, restartedInALoop: true), bootCompleted: false))
        #expect(!RootFallbackRule.endedBeforeBoot(RunEnd(result: -100, restartedInALoop: true), bootCompleted: true))
        #expect(!RootFallbackRule.endedBeforeBoot(RunEnd(result: -101, memoryRefusedMiB: 4096), bootCompleted: false))
        #expect(!RootFallbackRule.endedBeforeBoot(RunEnd(result: 0, stoppedByUser: true), bootCompleted: false))
    }
}

/// A guest shell that answers by command and remembers what it was asked.
final class RootScriptedShell: GuestShell, @unchecked Sendable {
    private let lock = NSLock()
    private var asked: [String] = []
    private let answer: @Sendable (String) async throws -> GuestShellOutput

    init(_ answer: @escaping @Sendable (String) async throws -> GuestShellOutput) { self.answer = answer }

    var commands: [String] { lock.withLock { asked } }

    func run(_ command: String, limit: Duration) async throws -> GuestShellOutput {
        lock.withLock { asked.append(command) }
        return try await answer(command)
    }
}

actor RootCounter {
    private(set) var value = 0
    func bump() { value += 1 }
}
