// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import Compression
import Foundation

// A reader for the zip a bug report is, for the viewer that shows what the
// attached report holds before it is sent. Everything in the archive was
// written by the virtual machine or by a file the person picked, so nothing in
// it is trusted: names, sizes and bytes are bounded, no name becomes a path,
// and no entry is read whole. The directory is read once; an entry is read in
// windows, on demand, straight from the file (stored) or through a streaming
// inflater (deflated), and nothing is extracted to disk.
//
// Only a plain archive is read: one disk, no ZIP64 records. That is what
// Android makes and what `BugReportArchive` amends; any other says so.

/// The bounds of what the viewer accepts from an archive.
nonisolated enum SendFeedbackArchiveLimits {
    /// Entries listed; more are counted and not listed.
    static let maxEntries = 4_096
    /// Bytes of the central directory read; a longer one lists what fits.
    static let maxDirectoryBytes = 4 << 20
    /// Bytes of one entry's name that are read.
    static let maxNameBytes = 1_024
    /// Characters of a name that are shown.
    static let maxNameCharacters = 160
    /// The most an entry may declare, to be shown (a larger one is listed, with its size).
    static let maxEntryBytes: UInt64 = 512 << 20
    /// The bytes of an entry that make a window of text.
    static let windowBytes = 16 << 10
    /// How much of an entry's start says what kind of content it is.
    static let sniffBytes = 4_096
    /// How much compressed input is read at a time while inflating.
    static let inputChunkBytes = 64 << 10
    /// The most one call may read of an entry whole, whatever the caller asks for (a picture to be drawn).
    static let maxWholeEntryBytes = 32 << 20
    /// The most a deflate stream can expand by (1032 to 1), which no honest size is above.
    static let maxDeflateRatio: UInt64 = 1_032
}

nonisolated enum SendFeedbackArchiveError: Error, Equatable, Sendable {
    /// There is no end of directory record: not a zip, or cut short.
    case notAnArchive
    /// The directory or an entry contradicts itself, or runs past the file.
    case damaged
    /// Several disks, or ZIP64.
    case unsupported
    /// The file could not be read.
    case unavailable
    /// An entry is larger than the caller allows to be read whole.
    case tooLarge
}

nonisolated struct SendFeedbackArchiveEntry: Hashable, Sendable, Identifiable {
    /// Why an entry cannot be shown, whatever it holds.
    enum Problem: Hashable, Sendable {
        case encrypted, unsupportedCompression, zip64, tooLarge, damaged
    }

    /// The place in the directory.
    let id: Int
    /// The name for the screen: decoded as UTF-8 with what is not text replaced, cut to a length, and never used as a path.
    let name: String
    let isFolder: Bool
    /// The size the directory declares, which the bytes are read against.
    let size: UInt64
    let compressedSize: UInt64
    let method: UInt16
    let headerOffset: UInt64
    let problem: Problem?
}

nonisolated struct SendFeedbackArchiveListing: Equatable, Sendable {
    var entries: [SendFeedbackArchiveEntry]
    /// The number of entries the archive says it holds.
    var declaredCount: Int

    /// Entries the archive has that are not listed (above the bound).
    var unlistedCount: Int { max(0, declaredCount - entries.count) }
    var totalBytes: UInt64 { entries.reduce(0) { $0 + $1.size } }
}

nonisolated struct SendFeedbackArchive: Sendable {
    let source: any SendFeedbackByteSource
    let listing: SendFeedbackArchiveListing
    /// 0 for the file itself, 1 for an archive stored inside it.
    let depth: Int

    private static let endSignature: UInt32 = 0x0605_4b50
    private static let zip64LocatorSignature: UInt32 = 0x0706_4b50
    private static let centralSignature: UInt32 = 0x0201_4b50
    static let localSignature: UInt32 = 0x0403_4b50
    private static let endRecordBytes = 22
    private static let centralRecordBytes = 46

    /// Reads the directory of the archive in `source`.
    static func open(_ source: any SendFeedbackByteSource, depth: Int = 0) throws -> SendFeedbackArchive {
        let length = source.length
        guard length >= UInt64(endRecordBytes) else { throw SendFeedbackArchiveError.notAnArchive }
        let tailSize = Int(min(length, UInt64(endRecordBytes + 0xFFFF)))
        let tailStart = length - UInt64(tailSize)
        let tail = try read(source, at: tailStart, count: tailSize)
        // The end record is the last signature whose comment reaches the end of the file.
        var found: Int?
        var index = tail.count - endRecordBytes
        while index >= 0 {
            if tail.u32(at: index) == endSignature, index + endRecordBytes + Int(tail.u16(at: index + 20)) == tail.count {
                found = index
                break
            }
            index -= 1
        }
        guard let end = found else { throw SendFeedbackArchiveError.notAnArchive }
        let endOffset = tailStart + UInt64(end)
        let disk = tail.u16(at: end + 4), directoryDisk = tail.u16(at: end + 6)
        let diskEntries = Int(tail.u16(at: end + 8)), count = Int(tail.u16(at: end + 10))
        let directorySize = UInt64(tail.u32(at: end + 12)), directoryOffset = UInt64(tail.u32(at: end + 16))
        if disk != 0 || directoryDisk != 0 { throw SendFeedbackArchiveError.unsupported }
        if count == 0xFFFF || directorySize == 0xFFFF_FFFF || directoryOffset == 0xFFFF_FFFF
            || (end >= 20 && tail.u32(at: end - 20) == zip64LocatorSignature) {
            throw SendFeedbackArchiveError.unsupported
        }
        guard diskEntries == count, directoryOffset + directorySize <= endOffset else {
            throw SendFeedbackArchiveError.damaged
        }
        if count == 0 { return SendFeedbackArchive(source: source, listing: .init(entries: [], declaredCount: 0), depth: depth) }
        let readSize = Int(min(directorySize, UInt64(SendFeedbackArchiveLimits.maxDirectoryBytes)))
        let directory = try read(source, at: directoryOffset, count: readSize)
        guard directory.count == readSize else { throw SendFeedbackArchiveError.damaged }

        var entries: [SendFeedbackArchiveEntry] = []
        var position = 0
        // A directory longer than what is read lists what fits in the part that was.
        let truncatedRead = UInt64(readSize) < directorySize
        while entries.count < min(count, SendFeedbackArchiveLimits.maxEntries) {
            guard position + centralRecordBytes <= directory.count else {
                if truncatedRead { break }
                throw SendFeedbackArchiveError.damaged
            }
            guard directory.u32(at: position) == centralSignature else { throw SendFeedbackArchiveError.damaged }
            let nameLength = Int(directory.u16(at: position + 28))
            let extraLength = Int(directory.u16(at: position + 30))
            let commentLength = Int(directory.u16(at: position + 32))
            let recordEnd = position + centralRecordBytes + nameLength + extraLength + commentLength
            guard recordEnd <= directory.count else {
                if truncatedRead { break }
                throw SendFeedbackArchiveError.damaged
            }
            let nameStart = position + centralRecordBytes
            let rawName = directory.subdata(
                in: (directory.startIndex + nameStart)..<(directory.startIndex + nameStart + min(nameLength, SendFeedbackArchiveLimits.maxNameBytes)))
            entries.append(entry(
                id: entries.count, record: directory, at: position, rawName: rawName, nameWasCut: nameLength > SendFeedbackArchiveLimits.maxNameBytes,
                sourceLength: length))
            position = recordEnd
        }
        return SendFeedbackArchive(
            source: source, listing: .init(entries: entries, declaredCount: count), depth: depth)
    }

    private static func entry(
        id: Int, record: Data, at position: Int, rawName: Data, nameWasCut: Bool, sourceLength: UInt64
    ) -> SendFeedbackArchiveEntry {
        let flags = record.u16(at: position + 8)
        let method = record.u16(at: position + 10)
        let compressed = UInt64(record.u32(at: position + 20))
        let size = UInt64(record.u32(at: position + 24))
        let headerOffset = UInt64(record.u32(at: position + 42))
        let name = SendFeedbackArchiveNames.display(rawName, cut: nameWasCut)
        let isFolder = rawName.last == UInt8(ascii: "/") && size == 0
        var problem: SendFeedbackArchiveEntry.Problem?
        if compressed == 0xFFFF_FFFF || size == 0xFFFF_FFFF || headerOffset == 0xFFFF_FFFF {
            problem = .zip64
        } else if flags & 1 != 0 || method == 99 {
            problem = .encrypted
        } else if method != 0 && method != 8 {
            problem = .unsupportedCompression
        } else if size > SendFeedbackArchiveLimits.maxEntryBytes {
            problem = .tooLarge
        } else if method == 0 && compressed != size {
            problem = .damaged
        } else if method == 8 && size > (compressed + 1) * SendFeedbackArchiveLimits.maxDeflateRatio {
            // More than a deflate stream of this length can hold: the size is a lie.
            problem = .damaged
        } else if headerOffset + 30 + compressed > sourceLength {
            // The entry runs past the end of the file: it was cut short.
            problem = .damaged
        }
        return SendFeedbackArchiveEntry(
            id: id, name: name, isFolder: isFolder, size: size, compressedSize: compressed, method: method,
            headerOffset: headerOffset, problem: isFolder ? nil : problem)
    }

    /// `count` bytes at `offset`, all of them or an error.
    private static func read(_ source: any SendFeedbackByteSource, at offset: UInt64, count: Int) throws -> Data {
        do {
            return try source.read(at: offset, count: count)
        } catch {
            throw SendFeedbackArchiveError.unavailable
        }
    }

    // MARK: Entries

    /// Where an entry's data begins and how it is stored, once its local header agrees with the directory.
    func location(of entry: SendFeedbackArchiveEntry) throws -> SendFeedbackEntryLocation {
        if let problem = entry.problem { throw problem == .tooLarge ? SendFeedbackArchiveError.unsupported : .damaged }
        let header: Data
        do {
            header = try source.read(at: entry.headerOffset, count: 30)
        } catch {
            throw SendFeedbackArchiveError.unavailable
        }
        guard header.count == 30, header.u32(at: 0) == Self.localSignature else { throw SendFeedbackArchiveError.damaged }
        let dataStart = entry.headerOffset + 30 + UInt64(header.u16(at: 26)) + UInt64(header.u16(at: 28))
        guard dataStart + entry.compressedSize <= source.length else { throw SendFeedbackArchiveError.damaged }
        return SendFeedbackEntryLocation(
            source: source, dataStart: dataStart, compressedSize: entry.compressedSize, size: entry.size,
            deflated: entry.method == 8)
    }
}

/// How an entry's name reaches the screen. The name is data of the archive: it is shown, never joined to a path.
nonisolated enum SendFeedbackArchiveNames {
    static func display(_ raw: Data, cut: Bool) -> String {
        var scalars = String(decoding: raw, as: UTF8.self).unicodeScalars.map { scalar in
            SendFeedbackNames.isControl(scalar) ? "\u{FFFD}" : scalar
        }
        var shortened = cut
        if scalars.count > SendFeedbackArchiveLimits.maxNameCharacters {
            scalars.removeLast(scalars.count - SendFeedbackArchiveLimits.maxNameCharacters)
            shortened = true
        }
        var text = String(String.UnicodeScalarView(scalars))
        if shortened { text += "\u{2026}" }
        return text
    }
}

/// Where the bytes of one entry are, as its local header and the directory agree.
nonisolated struct SendFeedbackEntryLocation: Sendable {
    let source: any SendFeedbackByteSource
    let dataStart: UInt64
    let compressedSize: UInt64
    let size: UInt64
    let deflated: Bool

    /// A whole source as one stored entry (a plain file).
    init(plain source: any SendFeedbackByteSource) {
        self.source = source
        dataStart = 0
        compressedSize = source.length
        size = source.length
        deflated = false
    }

    init(source: any SendFeedbackByteSource, dataStart: UInt64, compressedSize: UInt64, size: UInt64, deflated: Bool) {
        self.source = source
        self.dataStart = dataStart
        self.compressedSize = compressedSize
        self.size = size
        self.deflated = deflated
    }
}

/// Random reads of one entry's bytes. A stored entry is read where it lies; a
/// deflated one through a streaming inflater that is run forward, and started
/// again for a read that lies behind it. Never more than one input chunk and
/// the bytes asked for are in memory. Not thread safe: one reader at a time
/// (`SendFeedbackTextReader` owns it).
nonisolated final class SendFeedbackEntryStream {
    let location: SendFeedbackEntryLocation
    private var inflater: Inflater?
    private var input = Data()
    private var inputPosition = 0
    private var compressedRead: UInt64 = 0
    /// Bytes of the entry the inflater has produced so far, kept or not.
    private var emitted: UInt64 = 0
    private var finished = false

    var size: UInt64 { location.size }

    init(_ location: SendFeedbackEntryLocation) {
        self.location = location
    }

    /// Up to `count` bytes at `offset`; fewer only at the entry's end.
    func read(at offset: UInt64, count: Int) throws -> Data {
        guard count > 0, offset < size else { return Data() }
        let wanted = Int(min(UInt64(count), size - offset))
        if !location.deflated { return try readStored(at: offset, count: wanted) }
        if offset < emitted { restart() }
        try skip(offset - emitted)
        var out = Data(count: wanted)
        let produced = try out.withUnsafeMutableBytes { try produce(into: $0) }
        guard produced == wanted else { throw SendFeedbackArchiveError.damaged }
        return out
    }

    private func readStored(at offset: UInt64, count: Int) throws -> Data {
        let data: Data
        do {
            data = try location.source.read(at: location.dataStart + offset, count: count)
        } catch {
            throw SendFeedbackArchiveError.unavailable
        }
        guard data.count == count else { throw SendFeedbackArchiveError.damaged }
        return data
    }

    private func restart() {
        inflater = nil
        input = Data()
        inputPosition = 0
        compressedRead = 0
        emitted = 0
        finished = false
    }

    /// Runs the inflater forward over `count` bytes nobody asked for.
    private func skip(_ count: UInt64) throws {
        guard count > 0 else { return }
        var remaining = count
        var scratch = [UInt8](repeating: 0, count: SendFeedbackArchiveLimits.inputChunkBytes)
        while remaining > 0 {
            try Task.checkCancellation()
            let step = Int(min(remaining, UInt64(scratch.count)))
            let produced = try scratch.withUnsafeMutableBytes { buffer in
                try produce(into: UnsafeMutableRawBufferPointer(rebasing: buffer[0..<step]))
            }
            guard produced == step else { throw SendFeedbackArchiveError.damaged }
            remaining -= UInt64(produced)
        }
    }

    /// Fills `out` from the inflater; fewer bytes only when the entry's declared size is reached.
    private func produce(into out: UnsafeMutableRawBufferPointer) throws -> Int {
        let room = Int(min(UInt64(out.count), size - emitted))
        var produced = 0
        var stalled = 0
        let inflater = try self.inflater ?? makeInflater()
        while produced < room {
            if inputPosition == input.count, compressedRead < location.compressedSize {
                let step = Int(min(UInt64(SendFeedbackArchiveLimits.inputChunkBytes), location.compressedSize - compressedRead))
                do {
                    input = try location.source.read(at: location.dataStart + compressedRead, count: step)
                } catch {
                    throw SendFeedbackArchiveError.unavailable
                }
                guard !input.isEmpty else { throw SendFeedbackArchiveError.damaged }
                compressedRead += UInt64(input.count)
                inputPosition = 0
            }
            let allRead = compressedRead == location.compressedSize
            let result = input.withUnsafeBytes { source in
                inflater.run(
                    source: UnsafeRawBufferPointer(rebasing: source[inputPosition...]), finalize: allRead,
                    into: UnsafeMutableRawBufferPointer(rebasing: out[produced..<room]))
            }
            inputPosition += result.consumed
            produced += result.produced
            emitted += UInt64(result.produced)
            switch result.status {
            case .more: break
            case .end: finished = true
            case .failed: throw SendFeedbackArchiveError.damaged
            }
            if finished {
                // The stream ended before the size the directory declares.
                if produced < room { throw SendFeedbackArchiveError.damaged }
                break
            }
            if result.consumed == 0 && result.produced == 0 {
                stalled += 1
                // All the input is in and nothing more comes: cut short, or a stream that never ends.
                if allRead && inputPosition == input.count || stalled > 4 { throw SendFeedbackArchiveError.damaged }
            } else {
                stalled = 0
            }
        }
        return produced
    }

    private func makeInflater() throws -> Inflater {
        guard let made = Inflater() else { throw SendFeedbackArchiveError.unavailable }
        inflater = made
        return made
    }

    /// Apple's streaming decoder for raw deflate (what a zip entry holds).
    private final class Inflater {
        enum Status { case more, end, failed }

        private var stream: compression_stream
        private let placeholder = UnsafeMutablePointer<UInt8>.allocate(capacity: 1)

        init?() {
            stream = compression_stream(
                dst_ptr: placeholder, dst_size: 0, src_ptr: UnsafePointer(placeholder), src_size: 0, state: nil)
            guard compression_stream_init(&stream, COMPRESSION_STREAM_DECODE, COMPRESSION_ZLIB) == COMPRESSION_STATUS_OK
            else {
                placeholder.deallocate()
                return nil
            }
        }

        deinit {
            compression_stream_destroy(&stream)
            placeholder.deallocate()
        }

        func run(
            source: UnsafeRawBufferPointer, finalize: Bool, into out: UnsafeMutableRawBufferPointer
        ) -> (status: Status, consumed: Int, produced: Int) {
            stream.src_ptr = source.baseAddress?.assumingMemoryBound(to: UInt8.self) ?? UnsafePointer(placeholder)
            stream.src_size = source.count
            stream.dst_ptr = out.baseAddress?.assumingMemoryBound(to: UInt8.self) ?? placeholder
            stream.dst_size = out.count
            let status = compression_stream_process(&stream, finalize ? Int32(COMPRESSION_STREAM_FINALIZE.rawValue) : 0)
            let consumed = source.count - stream.src_size
            let produced = out.count - stream.dst_size
            switch status {
            case COMPRESSION_STATUS_OK: return (.more, consumed, produced)
            case COMPRESSION_STATUS_END: return (.end, consumed, produced)
            default: return (.failed, consumed, produced)
            }
        }
    }
}

extension Data {
    fileprivate nonisolated func u16(at index: Int) -> UInt16 {
        UInt16(self[startIndex + index]) | UInt16(self[startIndex + index + 1]) << 8
    }

    fileprivate nonisolated func u32(at index: Int) -> UInt32 {
        UInt32(u16(at: index)) | UInt32(u16(at: index + 2)) << 16
    }
}

#endif
