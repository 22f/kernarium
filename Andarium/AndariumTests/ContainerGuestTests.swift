// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CryptoKit
import Darwin
import Foundation
import Testing
@testable import Andarium

/// Install APK of a file that holds an app in parts, against a real Android
/// guest. The suite is opt-in. It reaches the guest's adbd over a loopback TCP
/// port (a launcher on the Mac bridges the port to the guest's adbd), and the
/// APKs it installs are synthesized ones that a harness outside the repository
/// writes into a folder:
///
///     TEST_RUNNER_ANDARIUM_CONTAINER_GUEST_PORT=<port of the guest's adbd>
///     TEST_RUNNER_ANDARIUM_CONTAINER_GUEST_FIXTURES=<folder with the APKs>
///
/// The folder holds `base.apk`, `config.xhdpi.apk`, `config.arm64_v8a.apk`,
/// `config.armeabi_v7a.apk`, `config.x86_64.apk` (one app, one signature),
/// `old-base.apk` and `old-config.xhdpi.apk` (the same package built for API
/// level 21) and `foreign-config.fr.apk` (a split of that package signed by
/// another key). Every test leaves the guest as it found it.
@Suite(.serialized)
struct ContainerGuestTests {
    private static let environment = ProcessInfo.processInfo.environment
    private static let port = environment["ANDARIUM_CONTAINER_GUEST_PORT"].flatMap { UInt16($0) }
    private static let fixtures = environment["ANDARIUM_CONTAINER_GUEST_FIXTURES"]
    private static let enabled = port != nil && fixtures != nil
    private static let package = "org.example.andarium.splitprobe"
    private static let obbFolder = "/storage/emulated/0/Android/obb/" + package

    private func apk(_ name: String) throws -> Data {
        try Data(contentsOf: URL(fileURLWithPath: Self.fixtures!).appendingPathComponent(name))
    }

    private func operations() throws -> AdbGuestOperations {
        AdbGuestOperations(
            identity: try throwawayIdentity(), clock: GuestRunClock(), device: "Mac",
            connector: LoopbackAdbConnector(port: Self.port!), hostFree: { .max }, run: GuestRunMark.random())
    }

    private func shell(_ operations: AdbGuestOperations, _ command: String) async throws -> String {
        try await operations.shell(command, limit: .seconds(60)).text.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// The installed files of the app, by name, in `pm path` order; empty when it is not installed.
    private func installedFiles(_ operations: AdbGuestOperations) async throws -> [String] {
        try await shell(operations, "pm path \(Self.package)").split(whereSeparator: \.isNewline).map {
            String($0.split(separator: "/").last ?? "")
        }
    }

    /// The free space of the data partition, in KiB.
    private func freeKiB(_ operations: AdbGuestOperations) async throws -> Int {
        Int(try await shell(operations, "df -Pk /data | tail -n 1 | tr -s ' ' | cut -d' ' -f4")) ?? -1
    }

    private func activeSessions(_ operations: AdbGuestOperations) async throws -> Int {
        Int(try await shell(operations, "dumpsys package | grep -c 'Active Session'")) ?? -1
    }

    private func clean(_ operations: AdbGuestOperations) async {
        _ = try? await shell(operations, "pm uninstall \(Self.package) >/dev/null 2>&1; rm -rf '\(Self.obbFolder)'; true")
    }

    private func write(_ entries: [ContainerEntry], _ name: String) throws -> URL {
        try ContainerFixture.write(entries, name: name)
    }

    private func digest(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    // MARK: A plain APK and every kind of container

    @Test(.timeLimit(.minutes(10))) func aPlainApkInstallsAsItAlwaysDid() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(UUID().uuidString)-Probe.apk")
        try apk("base.apk").write(to: url)
        defer { try? FileManager.default.removeItem(at: url) }
        #expect(try await operations.installApk(at: url, update: { _ in }) == .installed)
        #expect(try await installedFiles(operations) == ["base.apk"])
        #expect(try await activeSessions(operations) == 0)
        await clean(operations)
    }

    @Test(.timeLimit(.minutes(20))) func everyKindOfContainerInstallsTheSplitsThatFitTheGuest() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        let abis = try await shell(operations, "getprop ro.product.cpu.abilist")
        let first = abis.split(separator: ",").first.map(String.init) ?? ""
        let known = ["arm64-v8a": "arm64_v8a", "armeabi-v7a": "armeabi_v7a", "x86_64": "x86_64"]
        let config = try #require(known[first], "unexpected processor list \(abis)")
        let wrong = known.values.filter { $0 != config }
        let base = try apk("base.apk"), density = try apk("config.xhdpi.apk")
        let arm64 = try apk("config.arm64_v8a.apk"), arm32 = try apk("config.armeabi_v7a.apk"), x86 = try apk("config.x86_64.apk")
        let kinds: [(String, [ContainerEntry])] = [
            ("bundletool.apks", [
                ContainerEntry("toc.pb", Data([1])), ContainerEntry("splits/base-master.apk", base),
                ContainerEntry("splits/base-xhdpi.apk", density), ContainerEntry("splits/base-arm64_v8a.apk", arm64),
                ContainerEntry("splits/base-armeabi_v7a.apk", arm32), ContainerEntry("splits/base-x86_64.apk", x86),
            ]),
            ("installer.apks", [
                ContainerEntry("meta.sai_v2.json", Data("{}".utf8)), ContainerEntry("base.apk", base),
                ContainerEntry("split_config.xhdpi.apk", density), ContainerEntry("split_config.arm64_v8a.apk", arm64),
                ContainerEntry("split_config.armeabi_v7a.apk", arm32), ContainerEntry("split_config.x86_64.apk", x86),
            ]),
            ("game.xapk", [
                ContainerEntry("icon.png", Data([9])),
                ContainerEntry("manifest.json", Data(#"{"xapk_version":2,"package_name":"\#(Self.package)","split_apks":[{"file":"base.apk","id":"base"},{"file":"config.xhdpi.apk","id":"config.xhdpi"},{"file":"config.arm64_v8a.apk","id":"config.arm64_v8a"},{"file":"config.armeabi_v7a.apk","id":"config.armeabi_v7a"},{"file":"config.x86_64.apk","id":"config.x86_64"}]}"#.utf8)),
                ContainerEntry("base.apk", base), ContainerEntry("config.xhdpi.apk", density),
                ContainerEntry("config.arm64_v8a.apk", arm64), ContainerEntry("config.armeabi_v7a.apk", arm32),
                ContainerEntry("config.x86_64.apk", x86),
            ]),
            ("viewer.apkm", [
                ContainerEntry("info.json", Data("{}".utf8)), ContainerEntry("base.apk", base),
                ContainerEntry("split_config.xhdpi.apk", density), ContainerEntry("split_config.arm64_v8a.apk", arm64),
                ContainerEntry("split_config.armeabi_v7a.apk", arm32), ContainerEntry("split_config.x86_64.apk", x86),
            ]),
        ]
        for (name, entries) in kinds {
            await clean(operations)
            let url = try write(entries, name)
            defer { try? FileManager.default.removeItem(at: url) }
            let log = UpdateLog()
            let outcome = try await operations.installApk(at: url, update: { log.add($0) })
            #expect(outcome == .installed, "\(name)")
            // The base, the density split and the split of the guest's own processor went in together.
            let files = try await installedFiles(operations)
            #expect(files.count == 3 && files.contains("base.apk"), "\(name): \(files)")
            #expect(files.contains { $0.contains("xhdpi") } && files.contains { $0.contains(config) }, "\(name): \(files)")
            for other in wrong { #expect(!files.contains { $0.contains(other) }, "\(name): \(files)") }
            #expect(try await activeSessions(operations) == 0, "\(name)")
            #expect(log.details.last == "The virtual machine is installing the app\u{2026}", "\(name)")
        }
        await clean(operations)
    }

    // MARK: Failures

    @Test(.timeLimit(.minutes(10))) func aSplitSignedByAnotherKeyIsTheGuestsReasonAndLeavesNoSession() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        let url = try write([
            ContainerEntry("meta.sai_v2.json", Data("{}".utf8)), ContainerEntry("base.apk", try apk("base.apk")),
            ContainerEntry("split_config.fr.apk", try apk("foreign-config.fr.apk")),
        ], "foreign.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        do {
            _ = try await operations.installApk(at: url, update: { _ in })
            Issue.record("installed")
        } catch let failure as AdbFailure {
            #expect(!failure.message.isEmpty)
            print("CONTAINER_GUEST foreign-signature message=\(failure.message)")
        }
        #expect(try await installedFiles(operations).isEmpty)
        #expect(try await activeSessions(operations) == 0)
        await clean(operations)
    }

    @Test(.timeLimit(.minutes(10))) func aDamagedPartAbandonsTheSession() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        var density = ContainerEntry("split_config.xhdpi.apk", try apk("config.xhdpi.apk"))
        density.crc = 0x1357_9BDF
        let url = try write([
            ContainerEntry("meta.sai_v2.json", Data("{}".utf8)), ContainerEntry("base.apk", try apk("base.apk")), density,
        ], "damaged.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        do {
            _ = try await operations.installApk(at: url, update: { _ in })
            Issue.record("installed")
        } catch let failure as AdbFailure {
            #expect(failure.message == InstallRefusal.damaged.message)
        }
        #expect(try await installedFiles(operations).isEmpty)
        #expect(try await activeSessions(operations) == 0)
        await clean(operations)
    }

    @Test(.timeLimit(.minutes(10))) func aCancelPartWayThroughAWriteLeavesNoSessionAndNoStagedData() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        // A split of 400 MiB (a hole in the file) after the base: the cancel lands inside its write.
        let url = try write([
            ContainerEntry("meta.sai_v2.json", Data("{}".utf8)), ContainerEntry("base.apk", try apk("base.apk")),
            ContainerEntry("split_config.xhdpi.apk", zeros: 400 << 20),
        ], "big.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        let freeBefore = try await freeKiB(operations)
        let box = TaskBox()
        let task = Task {
            try await operations.installApk(at: url, update: { update in
                if let progress = update.progress, progress > 0.25 { box.cancel() }
            })
        }
        box.task = task
        let result = await task.result
        guard case let .failure(error) = result, error is CancellationError else {
            Issue.record("expected the cancel, got \(result)")
            return
        }
        #expect(try await installedFiles(operations).isEmpty)
        #expect(try await activeSessions(operations) == 0)
        // Nothing of the session's staging is left on the data partition: the
        // 400 MiB the cancelled write had staged is free again (within the
        // noise of the system's own writes).
        var freeAfter = try await freeKiB(operations)
        for _ in 0..<30 where freeAfter + 8192 < freeBefore {
            try await Task.sleep(for: .milliseconds(500))
            freeAfter = try await freeKiB(operations)
        }
        print("CONTAINER_GUEST cancel free-before-kib=\(freeBefore) free-after-kib=\(freeAfter)")
        #expect(freeAfter + 8192 >= freeBefore)
        await clean(operations)
    }

    // MARK: Apps built for an older Android

    @Test(.timeLimit(.minutes(10))) func anOlderAppIsInstalledThroughANewSessionAfterTheQuestion() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        let url = try write([
            ContainerEntry("meta.sai_v2.json", Data("{}".utf8)), ContainerEntry("base.apk", try apk("old-base.apk")),
            ContainerEntry("split_config.xhdpi.apk", try apk("old-config.xhdpi.apk")),
        ], "old.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        do {
            _ = try await operations.installApk(at: url, update: { _ in })
            Issue.record("installed without the question")
        } catch let refused as LegacyApkRefused {
            #expect(refused.target == LegacyTarget(appLevel: 21, requiredLevel: 24))
        }
        #expect(try await installedFiles(operations).isEmpty)
        #expect(try await activeSessions(operations) == 0)
        #expect(try await operations.installApk(at: url, bypassLowTargetSdkBlock: true, update: { _ in }) == .installed)
        #expect(try await installedFiles(operations).count == 2)
        #expect(try await shell(operations, "dumpsys package \(Self.package) | grep -m1 -o 'targetSdk=[0-9]*'") == "targetSdk=21")
        #expect(try await activeSessions(operations) == 0)
        await clean(operations)
    }

    // MARK: Data

    @Test(.timeLimit(.minutes(15))) func anXapkPlacesItsObbFileAndTheAppReadsNoPartialOne() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        let obb = incompressible(5 << 20)
        let name = "main.1.\(Self.package).obb"
        let path = "Android/obb/\(Self.package)/\(name)"
        let url = try write([
            ContainerEntry("manifest.json", Data(#"{"xapk_version":2,"package_name":"\#(Self.package)","split_apks":[{"file":"base.apk","id":"base"},{"file":"config.xhdpi.apk","id":"config.xhdpi"}],"expansions":[{"file":"\#(path)","install_location":"EXTERNAL_STORAGE","install_path":"\#(path)"}]}"#.utf8)),
            ContainerEntry("base.apk", try apk("base.apk")), ContainerEntry("config.xhdpi.apk", try apk("config.xhdpi.apk")),
            ContainerEntry(path, obb),
        ], "game.xapk")
        defer { try? FileManager.default.removeItem(at: url) }
        let partials = try Self.settledLedger()
        #expect(try await operations.installApk(at: url, partials: partials.ledger, update: { _ in }) == .installed)
        defer { try? FileManager.default.removeItem(at: partials.directory) }
        #expect(try await installedFiles(operations).count == 2)
        let listing = try await shell(operations, "ls -A '\(Self.obbFolder)'")
        #expect(listing == name, "\(listing)")
        #expect(try await shell(operations, "sha256sum '\(Self.obbFolder)/\(name)'").hasPrefix(digest(obb)))
        #expect(try await shell(operations, "stat -c %s '\(Self.obbFolder)/\(name)'") == String(obb.count))
        #expect(!partials.ledger.isOwed)
        await clean(operations)
    }

    @Test(.timeLimit(.minutes(15))) func aCancelWhileTheDataIsCopiedLeavesTheAppInstalledAndNoPartialFile() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        let name = "main.1.\(Self.package).obb"
        let path = "Android/obb/\(Self.package)/\(name)"
        let url = try write([
            ContainerEntry("manifest.json", Data(#"{"xapk_version":2,"package_name":"\#(Self.package)","split_apks":[{"file":"base.apk","id":"base"}],"expansions":[{"file":"\#(path)","install_location":"EXTERNAL_STORAGE","install_path":"\#(path)"}]}"#.utf8)),
            ContainerEntry("base.apk", try apk("base.apk")), ContainerEntry(path, zeros: 600 << 20),
        ], "bigdata.xapk")
        defer { try? FileManager.default.removeItem(at: url) }
        let partials = try Self.settledLedger()
        defer { try? FileManager.default.removeItem(at: partials.directory) }
        let box = TaskBox()
        let copying = Flag()
        let task = Task {
            try await operations.installApk(at: url, partials: partials.ledger, update: { update in
                if update.cancellable == true { copying.raise() }
                if copying.isRaised, let progress = update.progress, progress > 0.3 { box.cancel() }
            })
        }
        box.task = task
        let result = await task.result
        guard case let .success(outcome) = result else {
            Issue.record("the cancel of the copy ends in the installed app, got \(result)")
            return
        }
        #expect(outcome == .installedWithoutData)
        #expect(try await installedFiles(operations) == ["base.apk"])
        let listing = try await shell(operations, "ls -A '\(Self.obbFolder)' 2>/dev/null")
        print("CONTAINER_GUEST cancel-data folder-listing=[\(listing)]")
        #expect(!listing.contains(".andarium-") && !listing.contains(name))
        #expect(!partials.ledger.isOwed)
        await clean(operations)
    }

    // MARK: A container of more than 4 GiB

    @Test(.timeLimit(.minutes(30))) func aContainerOfMoreThanFourGibInstallsItsApksFromPastTheFourGibMark() async throws {
        guard Self.enabled else { return }
        let operations = try operations()
        await clean(operations)
        // Two unused entries of 2.2 GiB (holes) come first, so the APKs and the
        // directory are past what 32-bit offsets hold.
        let pad: UInt64 = 22 << 27
        let url = try write([
            ContainerEntry("docs/pad1.bin", zeros: pad), ContainerEntry("docs/pad2.bin", zeros: pad),
            ContainerEntry("meta.sai_v2.json", Data("{}".utf8)), ContainerEntry("base.apk", try apk("base.apk")),
            ContainerEntry("split_config.xhdpi.apk", try apk("config.xhdpi.apk")),
        ], "huge.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        let size = try #require(try FileManager.default.attributesOfItem(atPath: url.path)[.size] as? UInt64)
        #expect(size > 4 << 30)
        let footprint = FootprintPeak()
        let before = FootprintPeak.current(relieving: true)
        #expect(try await operations.installApk(at: url, update: { _ in footprint.sample() }) == .installed)
        #expect(try await installedFiles(operations).count == 2)
        let growth = footprint.growth(since: before)
        print("CONTAINER_GUEST zip64 file=\(size) footprint-growth=\(growth.footprint) heap-growth=\(growth.heap)")
        #expect(growth.footprint < LargeTransferFootprintTests.allowedGrowth)
        await clean(operations)
    }

    // MARK: Helpers

    private static func settledLedger() throws -> (ledger: PartialSweepLedger, directory: URL) {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("ledger-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try PartialSweepLedger.markSettled(in: directory)
        return (PartialSweepLedger(directory: directory), directory)
    }
}

/// Lets an update closure cancel the task that is running the install.
private nonisolated final class TaskBox: @unchecked Sendable {
    private let lock = NSLock()
    private var current: Task<ApkInstallOutcome, Error>?
    var task: Task<ApkInstallOutcome, Error>? {
        get { lock.withLock { current } }
        set { lock.withLock { current = newValue } }
    }
    func cancel() { task?.cancel() }
}

private nonisolated final class Flag: @unchecked Sendable {
    private let lock = NSLock()
    private var raised = false
    func raise() { lock.withLock { raised = true } }
    var isRaised: Bool { lock.withLock { raised } }
}

/// Reaches a guest's adbd over a loopback TCP port.
nonisolated struct LoopbackAdbConnector: AdbConnector {
    let port: UInt16

    func connect() throws -> any AdbTransport {
        let descriptor = socket(AF_INET, SOCK_STREAM, 0)
        guard descriptor >= 0 else { throw AdbError.io(errno) }
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = port.bigEndian
        address.sin_addr.s_addr = inet_addr("127.0.0.1")
        let result = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(descriptor, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard result == 0 else {
            let code = errno
            close(descriptor)
            throw code == ECONNREFUSED ? AdbError.refused : AdbError.io(code)
        }
        return DescriptorTransport(descriptor: descriptor)
    }
}

/// Bytes that do not compress: a fixed pseudo-random sequence.
private func incompressible(_ count: Int) -> Data {
    var state: UInt64 = 0x9E37_79B9_7F4A_7C15
    var bytes = [UInt8](repeating: 0, count: count)
    for index in 0..<count {
        state = state &* 6364136223846793005 &+ 1442695040888963407
        bytes[index] = UInt8(truncatingIfNeeded: state >> 33)
    }
    return Data(bytes)
}
