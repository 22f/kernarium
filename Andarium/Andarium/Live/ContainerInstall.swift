// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// What an install has made in the guest that a failure or a cancel must
/// undo, shared between the operation's thread and the cleanup that follows.
/// A session is a thing in the guest until it is committed (which ends it,
/// whatever the answer) or abandoned.
nonisolated final class ApkInstallTrail: @unchecked Sendable {
    private let lock = NSLock()
    private var session: String?

    /// The id of a session that was created and has not been committed.
    var openSession: String? {
        get { lock.withLock { session } }
        set { lock.withLock { session = newValue } }
    }
}

/// Install APK for a file that holds an app in parts (.apks, .xapk, .apkm):
/// the parts that fit this virtual machine go into one install session, each
/// read from the archive as it is sent (the archive is neither unpacked nor
/// copied), and the commit installs them together. A plain APK under one of
/// those names installs as an APK. An APK of 4 GiB or more is refused as it
/// is anywhere, but the archive may be larger than that.
nonisolated enum ContainerInstallOperation {
    static let chunkBytes = ApkInstallOperation.chunkBytes

    /// Installs the archive at `file`, already coordinated (`name` is its
    /// name). Returns how it ended; a failure is thrown with its words.
    static func run(
        session: AdbSession, file: URL, name: String, device: String, bypassLowTargetSdkBlock: Bool,
        hostFree: @Sendable () -> UInt64, trail: ApkInstallTrail, update: @escaping @Sendable (BusyUpdate) -> Void
    ) throws -> ApkInstallOutcome {
        do {
            return try install(
                session: session, file: file, name: name, device: device,
                bypassLowTargetSdkBlock: bypassLowTargetSdkBlock, hostFree: hostFree, trail: trail, update: update)
        } catch let error as ZipError {
            throw AdbFailure(message: ApkContainer.refusal(for: error).message)
        } catch let refusal as InstallRefusal {
            throw AdbFailure(message: refusal.message)
        }
    }

    private static func install(
        session: AdbSession, file: URL, name: String, device: String, bypassLowTargetSdkBlock: Bool,
        hostFree: @Sendable () -> UInt64, trail: ApkInstallTrail, update: @escaping @Sendable (BusyUpdate) -> Void
    ) throws -> ApkInstallOutcome {
        let source = try ZipFileSource(contentsOf: file)
        guard source.size > 0 else { throw AdbFailure(message: PackageInstallMessages.message(forCode: "FAILED_INVALID_APK")) }
        update(BusyUpdate(detail: "\(name) \u{00B7} \(Formats.size(bytes: source.size))"))
        let archive: ZipArchive
        let candidate: ApkContainerPlan
        switch try ApkContainer.inspect(source, fileName: name) {
        case .singleApk:
            // An APK under another name installs as one, from the start.
            try ApkInstallOperation.sendPlain(
                session: session, file: file, name: name, device: device,
                bypassLowTargetSdkBlock: bypassLowTargetSdkBlock, hostFree: hostFree, update: update)
            return .installed
        case let .parts(plan):
            candidate = plan
            archive = try ZipArchive(source: source)
        }
        let watchdog = session.watchdog
        return try session.run { connection in
            watchdog.arm(idle: ApkInstallOperation.sendIdleLimit)
            let plan = try candidate.selecting(guestAbis: guestAbis(on: connection))
            // Nothing is streamed that the device cannot hold or the guest has no room for.
            let apkBytes = plan.apkBytes
            let total = plan.totalBytes
            if let shortage = InstallRoom.deviceShortage(bytes: total, free: hostFree()) {
                throw AdbFailure(message: shortage.message(device: device))
            }
            if let free = try GuestFiles.freeSpace(of: InstallRoom.guestPath, on: connection),
               let shortage = InstallRoom.guestShortage(bytes: total, free: free) {
                throw AdbFailure(message: shortage.message(device: device))
            }
            let counter = SentCounter(total: total)
            let throttle = ProgressThrottle { sent, all in
                update(BusyUpdate(progress: Double(sent) / Double(max(all, 1)) * ApkInstallOperation.sendingShare))
            }

            // The session, and each part written into it.
            let created = try runService(
                PackageInstallCommand.createSession(totalBytes: apkBytes, bypassLowTargetSdkBlock: bypassLowTargetSdkBlock),
                on: connection)
            guard let id = PackageInstallCommand.createdSession(inOutput: created) else {
                throw AdbFailure(message: PackageInstallMessages.failure(inOutput: created)
                    ?? PackageInstallMessages.message(forCode: "FAILED_INVALID_APK"))
            }
            trail.openSession = id
            for (index, part) in plan.candidates.enumerated() {
                if plan.candidates.count > 1 {
                    update(BusyUpdate(detail: String(localized: "Sending part \(index + 1) of \(plan.candidates.count)\u{2026}")))
                }
                try write(
                    part, to: id, archive: archive, on: connection, watchdog: watchdog,
                    progress: { delta in throttle.report(counter.add(delta), total) })
            }

            // The commit. It cannot be stopped, and it ends the session.
            update(BusyUpdate(
                progress: Double(counter.sent) / Double(max(total, 1)) * ApkInstallOperation.sendingShare,
                detail: String(localized: "The virtual machine is installing the app\u{2026}"), cancellable: false))
            watchdog.arm(idle: nil, overall: ApkInstallOperation.commitLimit)
            trail.openSession = nil
            let text = try runService(PackageInstallCommand.commitSession(id), on: connection)
            if !bypassLowTargetSdkBlock, case let .tooOldTarget(target) = PackageInstallAnswer(output: text) {
                throw LegacyApkRefused(target: target)
            }
            if let message = PackageInstallMessages.failure(inOutput: text) {
                throw AdbFailure(message: message)
            }
            return plan.data.isEmpty && plan.unplacedData == 0 ? .installed : .installedWithoutData
        }
    }

    // MARK: Pieces

    /// The processors of the virtual machine, as it lists them; the usual
    /// list when it cannot say.
    private static func guestAbis(on connection: AdbConnection) -> [String] {
        guard let result = try? connection.runShell("getprop ro.product.cpu.abilist"), result.exitCode == 0 else {
            return ApkContainer.defaultGuestAbis
        }
        let listed = result.text.split(whereSeparator: { $0 == "," || $0.isNewline })
            .map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        return listed.isEmpty ? ApkContainer.defaultGuestAbis : listed
    }

    /// Runs a service that prints a short answer and returns what it printed.
    private static func runService(_ service: String, on connection: AdbConnection) throws -> String {
        let stream = try connection.open(service)
        return String(decoding: try stream.readToEnd(limit: 64 * 1024), as: UTF8.self)
    }

    /// Writes one part of the archive into the session: exactly the bytes the
    /// directory declares, then the answer. `progress` is told each piece's size.
    private static func write(
        _ part: ApkContainerPlan.Part, to id: String, archive: ZipArchive, on connection: AdbConnection,
        watchdog: AdbWatchdog, progress: (UInt64) -> Void
    ) throws {
        guard let entry = archive.entry(named: part.entry) else { throw InstallRefusal.damaged }
        let reader = try archive.reader(for: entry, capacity: chunkBytes)
        let stream = try connection.open(PackageInstallCommand.writeSession(id, name: part.name, bytes: part.bytes))
        var sent: UInt64 = 0
        do {
            while sent < part.bytes {
                let want = Int(min(UInt64(chunkBytes), part.bytes - sent))
                guard let data = try reader.read(upTo: want), !data.isEmpty else { throw ZipError.corrupt("\(part.entry) ends early") }
                try stream.write(data)
                sent += UInt64(data.count)
                watchdog.touch()
                progress(UInt64(data.count))
            }
            // The entry's own length and checksum, which only the end shows.
            if try reader.read(upTo: 1) != nil { throw ZipError.corrupt("\(part.entry) is longer than it says") }
        } catch AdbError.streamClosed {
            // The package manager gave up early; what it printed says why.
        }
        let text = String(decoding: try stream.readToEnd(limit: 64 * 1024), as: UTF8.self)
        guard PackageInstallCommand.streamed(inOutput: text, bytes: part.bytes) else {
            throw AdbFailure(message: PackageInstallMessages.failure(inOutput: text)
                ?? PackageInstallMessages.message(forCode: "FAILED_INVALID_APK"))
        }
    }

    /// The bytes sent so far, over every part (and every data file).
    private final class SentCounter: @unchecked Sendable {
        private let lock = NSLock()
        private var total: UInt64 = 0
        let limit: UInt64

        init(total limit: UInt64) { self.limit = limit }

        var sent: UInt64 { lock.withLock { total } }

        func add(_ delta: UInt64) -> UInt64 {
            lock.withLock {
                total = min(limit, total &+ delta)
                return total
            }
        }
    }
}
