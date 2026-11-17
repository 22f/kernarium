// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore
@testable import AndariumFeedbackLogic

/// The findings of the second review of the bug report reader (C1 to C6): the kept tail after a read that
/// threw, a bounded tail, nested readers that are not kept for ever, a tree built from the structural name, two
/// end records that both fit, and the parser's use of the name clamp.
struct SendFeedbackReviewTests {
    /// A source that cancels the task that reads from it, once armed, on its next read.
    final class CancelOnRead: SendFeedbackByteSource, @unchecked Sendable {
        private let base: SendFeedbackMemoryBytes
        private let lock = NSLock()
        private var cancelNext = false

        init(_ data: Data) { base = SendFeedbackMemoryBytes(data) }

        var length: UInt64 { base.length }
        func arm() { lock.withLock { cancelNext = true } }

        func read(at offset: UInt64, count: Int) throws -> Data {
            let cancel = lock.withLock { () -> Bool in
                let was = cancelNext
                cancelNext = false
                return was
            }
            if cancel { withUnsafeCurrentTask { $0?.cancel() } }
            return try base.read(at: offset, count: count)
        }
    }

    private func reader(_ source: any SendFeedbackByteSource) throws -> SendFeedbackArchiveReader {
        SendFeedbackArchiveReader(try SendFeedbackArchive.open(source))
    }

    private func textReader(_ archive: SendFeedbackArchiveReader, entry: Int = 0) async throws -> SendFeedbackTextReader {
        guard case .text(let text) = try await archive.content(of: entry) else {
            Issue.record("the entry is not shown as text")
            throw SendFeedbackArchiveError.damaged
        }
        return text
    }

    // MARK: C1 the kept tail after a read that threw

    /// Raw deflate: `blocks` of 16 KiB, each of one letter (A, B, ...), each after 10,000 empty stored blocks, so
    /// that one production call reaches over several input chunks. A stored block of 16 KiB is `00 00 40 FF BF`.
    private func lettersStream(blocks: Int) -> Data {
        var payload = Data()
        for index in 0..<blocks {
            payload.append(contentsOf: [0, 0, 0x40, 0xFF, 0xBF])
            payload.append(Data(repeating: UInt8(65 + index % 26), count: 16_384))
            for _ in 0..<10_000 { payload.append(contentsOf: [0, 0, 0, 0xFF, 0xFF]) }
        }
        return payload
    }

    private func deflatedEntry(_ payload: Data, size: Int) -> SyntheticZip.Entry {
        var entry = SyntheticZip.Entry.stored("x.txt", payload)
        entry.method = 8
        entry.size = UInt32(size)
        return entry
    }

    @Test func aReadThatFailedDoesNotLeaveBytesThatAreReturnedAsAnEarlierPlace() async throws {
        // Six letters of 16 KiB, then a block of a type that does not exist; the directory declares 40 windows.
        var payload = lettersStream(blocks: 6)
        payload.append(0x07)
        let archive = try reader(SendFeedbackMemoryBytes(SyntheticZip([deflatedEntry(payload, size: 40 * 16_384)]).data()))
        let text = try await textReader(archive)
        _ = try await text.window(0)
        _ = try await text.window(1)
        await #expect(throws: SendFeedbackArchiveError.damaged) { try await text.window(12) }
        // Window 4 is what the stream holds at 64 KiB: right, or an error, never what was kept before.
        let got = try await text.window(4)
        #expect(got.text.utf8.count == 16_384 && got.text.utf8.allSatisfy { $0 == 69 }, "window 4 is E and nothing else")
    }

    @Test func aReadThatWasCancelledDoesNotLeaveBytesThatAreReturnedAsAnEarlierPlace() async throws {
        var payload = lettersStream(blocks: 40)
        payload.append(contentsOf: [1, 0, 0, 0xFF, 0xFF])
        let data = SyntheticZip([deflatedEntry(payload, size: 40 * 16_384)]).data()
        let source = CancelOnRead(data)
        let archive = try SendFeedbackArchive.open(source)
        let stream = SendFeedbackEntryStream(try archive.location(of: archive.listing.entries[0]))
        _ = try stream.read(at: 0, count: 100)
        source.arm()
        nonisolated(unsafe) let unsafeStream = stream
        let cancelled = await Task { () -> Bool in
            do {
                _ = try unsafeStream.read(at: 12 * 16_384, count: 100)
                return false
            } catch is CancellationError {
                return true
            } catch {
                return false
            }
        }.value
        #expect(cancelled, "the read was cancelled")
        // Where the inflater had got to when it stopped: a read just behind it is what the tail was for.
        let place = max(1, stream.emittedBytes) - 1
        let got = try stream.read(at: place, count: 8)
        let letter = UInt8(65 + Int(place / 16_384) % 26)
        #expect(got == Data(repeating: letter, count: 8), "the bytes at \(place) are \(letter), whatever was read before")
    }

    @Test func theTailNeverHoldsMoreThanItsBoundAndAWholeEntryReadDoesNotFeedIt() async throws {
        var payload = lettersStream(blocks: 8)
        payload.append(contentsOf: [1, 0, 0, 0xFF, 0xFF])
        let size = 8 * 16_384
        let data = SyntheticZip([deflatedEntry(payload, size: size)]).data()
        let archive = try SendFeedbackArchive.open(SendFeedbackMemoryBytes(data))
        let location = try archive.location(of: archive.listing.entries[0])
        let stream = SendFeedbackEntryStream(location)
        // A jump forward is skipped in chunks, which are appended to the tail as they come.
        _ = try stream.read(at: 100_000, count: 100)
        #expect(stream.tailCount <= SendFeedbackEntryStream.tailBytes)
        var offset: UInt64 = 100_100
        while offset < UInt64(size) {
            _ = try stream.read(at: offset, count: 16_384)
            #expect(stream.tailCount <= SendFeedbackEntryStream.tailBytes)
            offset += 16_384
        }
        let whole = SendFeedbackEntryStream(location)
        let bytes = try whole.read(at: 0, count: size, keepsTail: false)
        #expect(bytes.count == size && whole.tailCount == 0, "a read of the whole entry keeps nothing")
        // And what it leaves behind still reads right.
        #expect(try whole.read(at: 5 * 16_384, count: 4) == Data(repeating: 70, count: 4))
    }

    // MARK: C5 two end records that both fit

    @Test func anEmptyEndRecordInTheCommentThatPointsAtItselfMakesTheArchiveUnreadable() throws {
        var fake = Data([0x50, 0x4B, 0x05, 0x06]) + Data(count: 18)
        let entries = [SyntheticZip.Entry.stored("real.txt", Data("real".utf8))]
        let position = SyntheticZip(entries, comment: fake).data().count - 22
        for index in 0..<4 { fake[16 + index] = UInt8((position >> (index * 8)) & 255) }
        let data = SyntheticZip(entries, comment: fake).data()
        #expect(throws: SendFeedbackArchiveError.damaged) { try SendFeedbackArchive.open(SendFeedbackMemoryBytes(data)) }
    }

    @Test func anOrdinaryCommentStillOpensTheArchive() throws {
        let entries = [SyntheticZip.Entry.stored("real.txt", Data("real".utf8))]
        let plain = try SendFeedbackArchive.open(SendFeedbackMemoryBytes(
            SyntheticZip(entries, comment: Data("made by a build server".utf8)).data()))
        #expect(plain.listing.entries.map(\.name) == ["real.txt"])
        // The four bytes of the signature in a comment, with no end record that reaches the end of the file.
        let signature = Data([0x50, 0x4B, 0x05, 0x06]) + Data(repeating: 0x41, count: 24)
        let odd = try SendFeedbackArchive.open(SendFeedbackMemoryBytes(SyntheticZip(entries, comment: signature).data()))
        #expect(odd.listing.entries.map(\.name) == ["real.txt"])
    }

    // MARK: C4 the tree is built from the structural name

    @Test func aLongFolderNameChangesNeitherTheHierarchyNorTheFile() throws {
        let long = String(repeating: "x", count: 200)
        let archive = try reader(SendFeedbackMemoryBytes(SyntheticZip([
            .stored("folder/\(long)/real.txt", Data("private".utf8)),
        ]).data()))
        // "folder" holds one folder and nothing else, so the two are one row, as Files shows a chain.
        let top = archive.tree.level(at: "")
        let chain = try #require(top.folders.first)
        #expect(top.folders.count == 1 && top.files.isEmpty)
        #expect(chain.path == "folder/\(long)", "the hierarchy is the archive's")
        #expect(chain.label == "folder/" + String(repeating: "x", count: SendFeedbackArchiveLimits.maxNameCharacters) + "\u{2026}",
                "only what is drawn is shortened")
        let inside = archive.tree.level(at: "folder")
        #expect(inside.files.isEmpty && inside.folders.map(\.path) == ["folder/\(long)"])
        let deepest = archive.tree.level(at: chain.path)
        #expect(deepest.files.map(\.label) == ["real.txt"] && deepest.files[0].entry.id == 0)
    }

    // MARK: C6 the parser reads a name only up to the bound

    @Test func theParserKeepsOfANameOnlyWhatTheBoundLetsBeRead() throws {
        let long = String(repeating: "n", count: 5_000) + "/"
        let archive = try SendFeedbackArchive.open(SendFeedbackMemoryBytes(SyntheticZip([.stored(long, Data())]).data()))
        let entry = archive.listing.entries[0]
        #expect(entry.path.utf8.count == SendFeedbackArchiveLimits.maxNameBytes, "the name was read to the bound and no further")
        #expect(entry.isFolder, "a name that is cut is still told by its last byte")
    }
}
