// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// What an install has made in the guest that a failure or a cancel must
/// undo, shared between the operation's thread and the cleanup that follows.
/// A session is a thing in the guest until it is committed (which ends it,
/// whatever the answer) or abandoned. After the commit the app is installed,
/// and what is left to undo is the partial file of the data being copied.
nonisolated final class ApkInstallTrail: @unchecked Sendable {
    private let lock = NSLock()
    private var session: String?
    private var installed = false
    private var partial: String?
    private var importing = false

    /// The id of a session that was created and has not been committed.
    var openSession: String? {
        get { lock.withLock { session } }
        set { lock.withLock { session = newValue } }
    }

    /// The commit answered `Success`: the app is installed.
    var committed: Bool {
        get { lock.withLock { installed } }
        set { lock.withLock { installed = newValue } }
    }

    /// The hidden file a data file is being written to, until it is moved
    /// into place or removed.
    var partialFile: String? {
        get { lock.withLock { partial } }
        set { lock.withLock { partial = newValue } }
    }

    /// The sweep ledger was told an import began and has not been told it ended.
    var importOpen: Bool {
        get { lock.withLock { importing } }
        set { lock.withLock { importing = newValue } }
    }
}

/// What the copying of an .xapk's data needs besides the connection: the
/// guest run whose mark the partial files carry, and the ledger that says
/// the guest may hold one (`PartialSweepLedger`; nil where a test has none).
nonisolated struct ApkDataStep: Sendable {
    var run = GuestRunMark.random()
    var partials: PartialSweepLedger?
}

/// A zip entry as the source of an upload: read in pieces, never whole, and
/// the entry's own length and checksum are checked when the last piece is
/// asked for, so a damaged file never reaches its final name.
nonisolated final class ZipEntrySource: AdbSource, @unchecked Sendable {
    private let reader: ZipEntryReader

    init(_ reader: ZipEntryReader) { self.reader = reader }

    func read(limit: Int) throws -> Data? { try reader.read(upTo: limit) }
}

/// Install APK for a file that holds an app in parts (.apks, .xapk, .apkm):
/// the parts that fit this virtual machine go into one install session, each
/// read from the archive as it is sent (the archive is neither unpacked nor
/// copied), and the commit installs them together. A plain APK under one of
/// those names installs as an APK. An APK of 4 GiB or more is refused as it
/// is anywhere, but the archive may be larger than that. An .xapk's OBB files
/// are copied into the app's folder of internal storage once the app is in.
nonisolated enum ContainerInstallOperation {
    static let chunkBytes = ApkInstallOperation.chunkBytes
    /// With data to copy the bar is shared by the bytes of the parts and of
    /// the data, and Android's own install, whose length is not known, holds
    /// it between them; the last twentieth is the end.
    static let dataShare = 0.95

    /// Installs the archive at `file`, already coordinated (`name` is its
    /// name). Returns how it ended; a failure is thrown with its words.
    static func run(
        session: AdbSession, file: URL, name: String, device: String, bypassLowTargetSdkBlock: Bool,
        hostFree: @Sendable () -> UInt64, trail: ApkInstallTrail, data: ApkDataStep = ApkDataStep(),
        update: @escaping @Sendable (BusyUpdate) -> Void
    ) throws -> ApkInstallOutcome {
        do {
            return try install(
                session: session, file: file, name: name, device: device,
                bypassLowTargetSdkBlock: bypassLowTargetSdkBlock, hostFree: hostFree, trail: trail, data: data, update: update)
        } catch let error as ZipError {
            throw AdbFailure(message: ApkContainer.refusal(for: error).message)
        } catch let refusal as InstallRefusal {
            throw AdbFailure(message: refusal.message)
        }
    }

    private static func install(
        session: AdbSession, file: URL, name: String, device: String, bypassLowTargetSdkBlock: Bool,
        hostFree: @Sendable () -> UInt64, trail: ApkInstallTrail, data: ApkDataStep,
        update: @escaping @Sendable (BusyUpdate) -> Void
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
            let share = plan.data.isEmpty ? ApkInstallOperation.sendingShare : dataShare
            let throttle = ProgressThrottle { sent, all in
                update(BusyUpdate(progress: Double(sent) / Double(max(all, 1)) * share))
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
                progress: Double(counter.sent) / Double(max(total, 1)) * share,
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
            trail.committed = true
            if plan.data.isEmpty { return plan.unplacedData == 0 ? .installed : .installedWithoutData }
            return try placeData(
                plan, archive: archive, on: connection, watchdog: watchdog, trail: trail, step: data,
                progress: { delta in throttle.report(counter.add(delta), total) }, update: update)
        }
    }

    // MARK: The data

    /// Copies the data files of an .xapk into the app's folder, each under
    /// a hidden partial name and moved into place whole, after the app is
    /// installed. The step can be stopped (the Cancel button is back for it),
    /// and what stops it, or fails in it, leaves the app installed without
    /// its data. A partial file left in the guest is `trail`'s to remove.
    private static func placeData(
        _ plan: ApkContainerPlan, archive: ZipArchive, on connection: AdbConnection, watchdog: AdbWatchdog,
        trail: ApkInstallTrail, step: ApkDataStep, progress: (UInt64) -> Void,
        update: @escaping @Sendable (BusyUpdate) -> Void
    ) throws -> ApkInstallOutcome {
        // Every destination is the app's own OBB folder or the step does not begin.
        var places: [(file: ApkContainerPlan.DataFile, folder: String, path: String)] = []
        for file in plan.data {
            guard let place = plan.guestLocation(of: file) else { return .installedWithoutData }
            places.append((file, place.folder, place.path))
        }
        update(BusyUpdate(detail: String(localized: "Copying app data\u{2026}"), cancellable: true))
        watchdog.arm(idle: ApkInstallOperation.sendIdleLimit)
        do {
            if let partials = step.partials {
                try partials.importBegan(mark: step.run)
                trail.importOpen = true
            }
            for (index, place) in places.enumerated() {
                if places.count > 1 {
                    update(BusyUpdate(detail: String(localized: "Copying app data \(index + 1) of \(places.count)\u{2026}")))
                }
                guard let entry = archive.entry(named: place.file.entry) else { throw InstallRefusal.damaged }
                let made = try connection.runShell(GuestFiles.makeFolderCommand(for: place.folder))
                guard made.exitCode == 0 else { throw AdbError.remote("the folder for the app\u{2019}s data could not be made") }
                let source = ZipEntrySource(try archive.reader(for: entry, capacity: GuestFiles.uploadPiece))
                let temporary = GuestFiles.temporaryPath(
                    in: place.folder, run: step.run, token: String(UUID().uuidString.prefix(8)).lowercased())
                trail.partialFile = temporary
                var reported: UInt64 = 0
                try GuestFiles.upload(
                    source, modified: Date(), to: place.path, temporary: temporary, replacing: true, on: connection,
                    progress: { sent in
                        watchdog.touch()
                        progress(sent - reported)
                        reported = sent
                    })
                trail.partialFile = nil
            }
        } catch {
            // A stop, a timeout or a lost connection is the caller's to settle.
            if watchdog.currentCause != .none { throw error }
            if let temporary = trail.partialFile,
               (try? connection.runShell(GuestFiles.removeCommand(for: temporary)))?.exitCode == 0 {
                trail.partialFile = nil
            }
            endImport(trail, step)
            return .installedWithoutData
        }
        endImport(trail, step)
        return plan.unplacedData == 0 ? .installed : .installedWithoutData
    }

    /// Tells the ledger the data step ended, if it was told it began; its
    /// file is in place or known to be gone.
    private static func endImport(_ trail: ApkInstallTrail, _ step: ApkDataStep) {
        guard trail.importOpen, trail.partialFile == nil else { return }
        trail.importOpen = false
        step.partials?.importEnded(mark: step.run, clean: true)
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
