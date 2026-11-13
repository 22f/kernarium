// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CryptoKit
import Foundation
import Testing
@testable import Andarium

private let refusalLine =
    "Failure [INSTALL_FAILED_DEPRECATED_SDK_VERSION: App package must target at least SDK version 24, but found 7]\n"

/// Bytes that tell one file from another and do not repeat within it.
private func apkData(_ seed: Int, count: Int = 300_001) -> Data {
    var data = Data(count: count)
    for index in 0..<count { data[index] = UInt8(truncatingIfNeeded: index &* (seed &* 2 &+ 31) &+ index >> 8 &+ seed) }
    return data
}

private func digest(_ data: Data) -> String {
    SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
}

private nonisolated final class Flag: @unchecked Sendable {
    private let lock = NSLock()
    private var raised = false
    func raise() { lock.withLock { raised = true } }
    var isRaised: Bool { lock.withLock { raised } }
}

/// What Install APK does with a file that holds an app in parts: the parts
/// that fit the virtual machine go into one session of a fake package
/// manager, byte for byte, and the session ends in a commit or is abandoned.
/// The container of every kind is a real ZIP file; the guest is the fake
/// adbd that keeps sessions (`DiscardingAdbd`).
@Suite(.serialized)
struct ContainerInstallTests {
    private func guest(
        on adbd: DiscardingAdbd, hostFree: @escaping @Sendable () -> UInt64 = { .max }
    ) throws -> AdbGuestOperations {
        AdbGuestOperations(
            identity: try throwawayIdentity(), clock: GuestRunClock(), device: "iPad", connector: adbd,
            hostFree: hostFree, run: GuestRunMark.random())
    }

    private func outcome<T>(_ work: () async throws -> T) async -> Result<T, Error> {
        do { return .success(try await work()) } catch { return .failure(error) }
    }

    /// The parts the fake package manager committed, as (name, digest).
    private func committedParts(_ adbd: DiscardingAdbd) -> [[String]] {
        adbd.committedSessions.map { $0.parts.map { "\($0.name) \($0.digest)" } }
    }

    // MARK: The containers

    private struct Case {
        var name: String
        var entries: [ContainerEntry]
        /// The entries that must reach the session, in order of the session's names.
        var selected: [(session: String, content: Data)]
    }

    private func cases() -> [Case] {
        let base = apkData(1), density = apkData(2), arm64 = apkData(3), x86 = apkData(4), arm32 = apkData(5), language = apkData(6)
        return [
            Case(
                name: "bundletool.apks",
                entries: [
                    ContainerEntry("toc.pb", Data([1, 2, 3])),
                    ContainerEntry("splits/base-master.apk", base),
                    ContainerEntry("splits/base-xxhdpi.apk", density),
                    ContainerEntry("splits/base-arm64_v8a.apk", arm64),
                    ContainerEntry("splits/base-x86_64.apk", x86),
                    ContainerEntry("splits/base-armeabi_v7a.apk", arm32),
                ],
                selected: [("base.apk", base), ("1_base-xxhdpi.apk", density), ("2_base-arm64_v8a.apk", arm64)]),
            Case(
                name: "installer.apks",
                entries: [
                    ContainerEntry("meta.sai_v2.json", Data("{}".utf8)),
                    ContainerEntry("split_config.x86_64.apk", x86),
                    ContainerEntry("base.apk", base),
                    ContainerEntry("split_config.arm64_v8a.apk", arm64),
                    ContainerEntry("split_config.en.apk", language),
                ],
                selected: [("base.apk", base), ("2_split_config_arm64_v8a.apk", arm64), ("3_split_config_en.apk", language)]),
            Case(
                name: "game.xapk",
                entries: [
                    ContainerEntry("icon.png", Data([9, 9])),
                    ContainerEntry("manifest.json", Data("""
                    {"xapk_version":2,"package_name":"com.example.game","split_apks":[
                    {"file":"com.example.game.apk","id":"base"},
                    {"file":"config.arm64_v8a.apk","id":"config.arm64_v8a"},
                    {"file":"config.xxhdpi.apk","id":"config.xxhdpi"}]}
                    """.utf8)),
                    ContainerEntry("com.example.game.apk", base),
                    ContainerEntry("config.arm64_v8a.apk", arm64),
                    ContainerEntry("config.xxhdpi.apk", density),
                ],
                selected: [("base.apk", base), ("1_config_arm64_v8a.apk", arm64), ("2_config_xxhdpi.apk", density)]),
            Case(
                name: "viewer.apkm",
                entries: [
                    ContainerEntry("info.json", Data("{}".utf8)),
                    ContainerEntry("icon.png", Data([9])),
                    ContainerEntry("base.apk", base),
                    ContainerEntry("split_config.armeabi_v7a.apk", arm32),
                    ContainerEntry("split_config.arm64_v8a.apk", arm64),
                    ContainerEntry("split_config.xxhdpi.apk", density),
                ],
                selected: [("base.apk", base), ("2_split_config_arm64_v8a.apk", arm64), ("3_split_config_xxhdpi.apk", density)]),
        ]
    }

    @Test func everyKindOfContainerInstallsItsPartsThroughOneSessionByteForByte() async throws {
        for item in cases() {
            let adbd = DiscardingAdbd()
            defer { adbd.shutdown() }
            let url = try ContainerFixture.write(item.entries, name: item.name)
            defer { try? FileManager.default.removeItem(at: url) }
            // The processors of this virtual machine, as the guest lists them.
            adbd.abiList = "arm64-v8a,armeabi-v7a,armeabi"
            let log = UpdateLog()
            let result = try await guest(on: adbd).installApk(at: url, update: { log.add($0) })
            #expect(result == .installed, "\(item.name)")
            let expected = item.selected.map { "\($0.session) \(digest($0.content))" }
            // The splits of other processors stay in the file.
            #expect(committedParts(adbd) == [expected], "\(item.name)")
            #expect(adbd.openSessions.isEmpty && adbd.abandonedSessions.isEmpty, "\(item.name)")
            // Only the session's commands ran: no single-file install.
            let services = adbd.servicesOpened.filter { $0.hasPrefix("abb_exec:") }
            #expect(services.first?.contains("install-create") == true, "\(item.name)")
            #expect(services.last?.contains("install-commit") == true, "\(item.name)")
            #expect(!services.contains { $0.hasPrefix("abb_exec:package\0install\0") }, "\(item.name)")
            let written = Set(item.selected.map(\.session))
            #expect(services.filter { $0.contains("install-write") }.count == written.count, "\(item.name)")
            // The session was created for exactly the bytes it was given.
            let total = item.selected.reduce(UInt64(0)) { $0 + UInt64($1.content.count) }
            let created = try #require(adbd.committedSessions.first?.createService)
            #expect(created == PackageInstallCommand.createSession(totalBytes: total), "\(item.name)")
            #expect(adbd.installBytes == total, "\(item.name)")
            // The overlay was told which part is going, and then that Android is installing.
            if item.selected.count > 1 {
                #expect(log.details.contains("Sending part 1 of \(item.selected.count)\u{2026}"), "\(item.name)")
                #expect(log.details.contains("Sending part \(item.selected.count) of \(item.selected.count)\u{2026}"), "\(item.name)")
            }
            #expect(log.details.last == "The virtual machine is installing the app\u{2026}", "\(item.name)")
            let progress = log.progress
            #expect(progress == progress.sorted() && (progress.last ?? 0) <= 0.8 + 1e-9, "\(item.name)")
        }
    }

    @Test func theProcessorsOfTheGuestDecideWhichSplitsGo() async throws {
        let arm64 = apkData(3), arm32 = apkData(5), base = apkData(1)
        let entries = [
            ContainerEntry("manifest.json", Data(#"{"package_name":"com.example.a","split_apks":[{"file":"b.apk","id":"base"},{"file":"c64.apk","id":"config.arm64_v8a"},{"file":"c32.apk","id":"config.armeabi_v7a"}]}"#.utf8)),
            ContainerEntry("b.apk", base), ContainerEntry("c64.apk", arm64), ContainerEntry("c32.apk", arm32),
        ]
        let url = try ContainerFixture.write(entries, name: "abi.xapk")
        defer { try? FileManager.default.removeItem(at: url) }
        for (abis, wanted) in [
            ("arm64-v8a,armeabi-v7a", arm64), ("armeabi-v7a,armeabi", arm32), ("armeabi-v7a,arm64-v8a", arm32),
        ] {
            let adbd = DiscardingAdbd()
            defer { adbd.shutdown() }
            adbd.abiList = abis
            _ = try await guest(on: adbd).installApk(at: url, update: { _ in })
            let parts = try #require(adbd.committedSessions.first?.parts)
            #expect(parts.count == 2 && parts[0].digest == digest(base) && parts[1].digest == digest(wanted), "\(abis)")
        }
        // An app with no code for any of the guest's processors is turned away before a session exists.
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.abiList = "x86_64,x86"
        let result = await outcome { try await guest(on: adbd).installApk(at: url, update: { _ in }) }
        guard case let .failure(error) = result else {
            Issue.record("installed")
            return
        }
        #expect((error as? AdbFailure)?.message == InstallRefusal.noMatchingProcessor.message)
        #expect(adbd.servicesOpened.filter { $0.hasPrefix("abb_exec:") }.isEmpty)
    }

    @Test func aPlainApkUnderAContainerNameInstallsAsOneApk() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let apk = apkData(7, count: 123_457)
        let url = try ContainerFixture.write([
            ContainerEntry("AndroidManifest.xml", Data([1])), ContainerEntry("classes.dex", apk),
        ], name: "Plain.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        let size = try #require(try FileManager.default.attributesOfItem(atPath: url.path)[.size] as? Int)
        _ = try await guest(on: adbd).installApk(at: url, update: { _ in })
        #expect(adbd.servicesOpened.filter { $0.hasPrefix("abb_exec:") } == [PackageInstallCommand.service(size: size)])
        #expect(adbd.committedSessions.isEmpty && adbd.openSessions.isEmpty)
    }

    // MARK: Turned away before anything is sent

    @Test func aFileThatIsNoAppIsTurnedAwayWithTheWordsOfItsKindAndNothingIsSent() async throws {
        let base = apkData(1)
        let hostile: [(String, [ContainerEntry], InstallRefusal)] = [
            ("empty.apks", [ContainerEntry("readme.txt", Data("hi".utf8))], .noApp),
            ("bundle.apks", [ContainerEntry("BundleConfig.pb", Data([1])), ContainerEntry("base/manifest/AndroidManifest.xml", Data([1]))], .appBundle),
            ("nobase.apks", [ContainerEntry("toc.pb", Data([1])), ContainerEntry("splits/base-xxhdpi.apk", base)], .damaged),
            ("twobases.apkm", [ContainerEntry("info.json", Data("{}".utf8)), ContainerEntry("a.apk", base), ContainerEntry("b.apk", base)], .damaged),
            ("missing.xapk", [
                ContainerEntry("manifest.json", Data(#"{"package_name":"com.example.a","split_apks":[{"file":"gone.apk","id":"base"}]}"#.utf8)),
                ContainerEntry("other.apk", base),
            ], .damaged),
            ("x86only.xapk", [
                ContainerEntry("manifest.json", Data(#"{"package_name":"com.example.a","split_apks":[{"file":"b.apk","id":"base"},{"file":"c.apk","id":"config.x86_64"}]}"#.utf8)),
                ContainerEntry("b.apk", base), ContainerEntry("c.apk", base),
            ], .noMatchingProcessor),
        ]
        for (name, entries, refusal) in hostile {
            let adbd = DiscardingAdbd()
            defer { adbd.shutdown() }
            adbd.abiList = "arm64-v8a"
            let url = try ContainerFixture.write(entries, name: name)
            defer { try? FileManager.default.removeItem(at: url) }
            let result = await outcome { try await guest(on: adbd).installApk(at: url, update: { _ in }) }
            guard case let .failure(error) = result else {
                Issue.record("\(name) installed")
                continue
            }
            #expect((error as? AdbFailure)?.message == refusal.message, "\(name)")
            #expect(adbd.servicesOpened.filter { $0.hasPrefix("abb_exec:") }.isEmpty, "\(name)")
            #expect(adbd.openSessions.isEmpty, "\(name)")
        }
    }

    @Test func aFileThatIsNotAZipIsTurnedAwayInTheWordsOfItsName() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        for (name, refusal) in [("junk.apks", InstallRefusal.notApk), ("junk.xapk", .notApk), ("junk.apkm", .encrypted)] {
            let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(UUID().uuidString)-\(name)")
            try apkData(1, count: 5000).write(to: url)
            defer { try? FileManager.default.removeItem(at: url) }
            let result = await outcome { try await guest(on: adbd).installApk(at: url, update: { _ in }) }
            guard case let .failure(error) = result else {
                Issue.record("\(name) installed")
                continue
            }
            #expect((error as? AdbFailure)?.message == refusal.message, "\(name)")
        }
        #expect(adbd.servicesOpened.filter { $0.hasPrefix("abb_exec:") }.isEmpty)
    }

    @Test func anApkOfFourGibOrMoreInsideAContainerIsRefusedBeforeASessionExists() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        var big = ContainerEntry("splits/base-xxhdpi.apk", zeros: InstallRefusal.singleApkLimit)
        big.crc = 0
        let url = try ContainerFixture.write([
            ContainerEntry("toc.pb", Data([1])), ContainerEntry("splits/base-master.apk", apkData(1)), big,
        ], name: "Big.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        let result = await outcome { try await guest(on: adbd).installApk(at: url, update: { _ in }) }
        guard case let .failure(error) = result else {
            Issue.record("installed")
            return
        }
        #expect((error as? AdbFailure)?.message == InstallRefusal.tooLarge.message)
        #expect(adbd.servicesOpened.filter { $0.hasPrefix("abb_exec:") }.isEmpty)
    }

    @Test func theRoomChecksCountTheWholeInstallAndRunBeforeASession() async throws {
        let item = cases()[0]
        let url = try ContainerFixture.write(item.entries, name: item.name)
        defer { try? FileManager.default.removeItem(at: url) }
        // The device is short.
        let device = DiscardingAdbd()
        defer { device.shutdown() }
        let shortDevice = await outcome { try await guest(on: device, hostFree: { 1_000_000 }).installApk(at: url, update: { _ in }) }
        guard case let .failure(error) = shortDevice, error is AdbFailure else {
            Issue.record("expected the device's shortage")
            return
        }
        #expect(device.servicesOpened.filter { $0.hasPrefix("abb_exec:package\0install-create") }.isEmpty)
        // The virtual machine is short.
        let machine = DiscardingAdbd()
        defer { machine.shutdown() }
        machine.guestFreeKiB = 100
        let shortMachine = await outcome { try await guest(on: machine).installApk(at: url, update: { _ in }) }
        guard case let .failure(second) = shortMachine, second is AdbFailure else {
            Issue.record("expected the virtual machine's shortage")
            return
        }
        #expect(machine.servicesOpened.filter { $0.hasPrefix("abb_exec:") }.isEmpty)
    }

    // MARK: Stopped and failed

    @Test func aCancelPartWayThroughAWriteLeavesNoSessionBehind() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        let item = cases()[0]
        let url = try ContainerFixture.write(item.entries, name: item.name)
        defer { try? FileManager.default.removeItem(at: url) }
        let reached = Flag()
        let gate = DispatchSemaphore(value: 0)
        defer { gate.signal() }
        // The fake package manager stops reading in the second part and leaves the sender waiting.
        adbd.onWritten = { written in
            if written >= 400_000 {
                reached.raise()
                gate.wait()
            }
        }
        let operations = try guest(on: adbd)
        let task = Task { try await operations.installApk(at: url, update: { _ in }) }
        let deadline = ContinuousClock.now + .seconds(20)
        while !reached.isRaised, ContinuousClock.now < deadline { try await Task.sleep(for: .milliseconds(5)) }
        #expect(reached.isRaised)
        task.cancel()
        let result = await task.result
        guard case let .failure(error) = result, error is CancellationError else {
            Issue.record("expected the cancel, got \(result)")
            return
        }
        // The cancel waited for the cleanup: the session is gone and was never committed.
        #expect(adbd.openSessions.isEmpty)
        #expect(adbd.committedSessions.isEmpty)
        #expect(adbd.abandonedSessions.count == 1)
        let services = adbd.servicesOpened.filter { $0.hasPrefix("abb_exec:") }
        #expect(services.contains { $0.contains("install-abandon") })
        #expect(!services.contains { $0.contains("install-commit") })
    }

    @Test func aDamagedPartAbandonsTheSessionAndSaysSo() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        var entries = cases()[0].entries
        // The second part's checksum is not its data's.
        entries[2].crc = 0x1234_5678
        let url = try ContainerFixture.write(entries, name: "Damaged.apks")
        defer { try? FileManager.default.removeItem(at: url) }
        let result = await outcome { try await guest(on: adbd).installApk(at: url, update: { _ in }) }
        guard case let .failure(error) = result else {
            Issue.record("installed")
            return
        }
        #expect((error as? AdbFailure)?.message == InstallRefusal.damaged.message)
        #expect(adbd.openSessions.isEmpty && adbd.committedSessions.isEmpty)
        #expect(adbd.abandonedSessions.count == 1)
    }

    @Test func aRefusedCommitIsTheGuestsReasonAndTheEndedSessionIsNotAbandoned() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.commitOutputs = ["Failure [INSTALL_FAILED_VERSION_DOWNGRADE: Downgrade detected]\n"]
        let item = cases()[0]
        let url = try ContainerFixture.write(item.entries, name: item.name)
        defer { try? FileManager.default.removeItem(at: url) }
        let result = await outcome { try await guest(on: adbd).installApk(at: url, update: { _ in }) }
        guard case let .failure(error) = result else {
            Issue.record("installed")
            return
        }
        #expect((error as? AdbFailure)?.message == PackageInstallMessages.message(forCode: "FAILED_VERSION_DOWNGRADE"))
        #expect(adbd.committedSessions.count == 1 && adbd.openSessions.isEmpty)
        // The commit ended the session; a second command on it would be noise.
        #expect(adbd.abandonedSessions.isEmpty)
        #expect(!adbd.servicesOpened.contains { $0.contains("install-abandon") })
    }

    @Test func aSessionTheGuestCannotCreateIsTheGuestsReason() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.createOutputs = ["Failure [INSTALL_FAILED_INSUFFICIENT_STORAGE: Not enough space]\n"]
        let item = cases()[0]
        let url = try ContainerFixture.write(item.entries, name: item.name)
        defer { try? FileManager.default.removeItem(at: url) }
        let result = await outcome { try await guest(on: adbd).installApk(at: url, update: { _ in }) }
        guard case let .failure(error) = result else {
            Issue.record("installed")
            return
        }
        #expect((error as? AdbFailure)?.message == PackageInstallMessages.message(forCode: "FAILED_INSUFFICIENT_STORAGE"))
        #expect(adbd.openSessions.isEmpty)
        #expect(!adbd.servicesOpened.contains { $0.contains("install-write") || $0.contains("install-abandon") })
    }

    // MARK: Apps built for an older Android

    @Test func anOlderAppIsRefusedAtTheCommitAndInstalledThroughANewSessionWithTheFlag() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.commitOutputs = [refusalLine, "Success\n"]
        let item = cases()[2]
        let url = try ContainerFixture.write(item.entries, name: item.name)
        defer { try? FileManager.default.removeItem(at: url) }
        let operations = try guest(on: adbd)
        let first = await outcome { try await operations.installApk(at: url, update: { _ in }) }
        guard case let .failure(error) = first, let refused = error as? LegacyApkRefused else {
            Issue.record("expected the too-old refusal, got \(first)")
            return
        }
        #expect(refused.target == LegacyTarget(appLevel: 7, requiredLevel: 24))
        // The refusal ended the first session; nothing was abandoned.
        #expect(adbd.openSessions.isEmpty && adbd.abandonedSessions.isEmpty)
        let second = try await operations.installApk(at: url, bypassLowTargetSdkBlock: true, update: { _ in })
        #expect(second == .installed)
        let sessions = adbd.committedSessions
        #expect(sessions.count == 2)
        #expect(sessions[0].parts == sessions[1].parts)
        #expect(sessions[0].id != sessions[1].id)
        let total = item.selected.reduce(UInt64(0)) { $0 + UInt64($1.content.count) }
        // The flag is in the create of the second session and in nothing else.
        #expect(sessions[0].createService == PackageInstallCommand.createSession(totalBytes: total))
        #expect(sessions[1].createService == PackageInstallCommand.createSession(totalBytes: total, bypassLowTargetSdkBlock: true))
        let withFlag = adbd.servicesOpened.filter { $0.contains(PackageInstallCommand.bypassLowTargetSdkBlock) }
        #expect(withFlag == [sessions[1].createService])
        #expect(adbd.openSessions.isEmpty && adbd.abandonedSessions.isEmpty)
    }

    @Test func aRefusalWithTheFlagIsAnOrdinaryFailure() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.commitOutputs = [refusalLine]
        let item = cases()[2]
        let url = try ContainerFixture.write(item.entries, name: item.name)
        defer { try? FileManager.default.removeItem(at: url) }
        let result = await outcome { try await guest(on: adbd).installApk(at: url, bypassLowTargetSdkBlock: true, update: { _ in }) }
        guard case let .failure(error) = result else {
            Issue.record("installed")
            return
        }
        #expect(!(error is LegacyApkRefused))
        #expect((error as? AdbFailure)?.message == PackageInstallMessages.message(forCode: "FAILED_DEPRECATED_SDK_VERSION"))
    }
}
