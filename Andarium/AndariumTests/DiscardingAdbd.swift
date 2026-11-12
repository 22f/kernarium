// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Darwin
import Foundation

/// An adbd that takes whatever a transfer sends and keeps none of it: the far
/// end of a socket pair for a test that moves a large file and watches the
/// application's memory, not the guest's. It speaks the wire format on a
/// thread of its own, one connection for each `connect`, and serves the three
/// services a transfer uses: `abb_exec` (the install stream: it counts the
/// bytes, then prints `installOutput`), `sync:` (`SEND`, `STA2`, `QUIT`; the
/// file's length is all it remembers) and `shell,v2,raw:` (`df` answers with
/// `guestFreeKiB`, anything else exits 0). Every message it handles is inside
/// an autorelease pool of its own, so the memory it uses is never the memory
/// the test measures.
nonisolated final class DiscardingAdbd: AdbConnector, @unchecked Sendable {
    private let lock = NSLock()
    private var transports: [DescriptorTransport] = []
    private var opened: [String] = []
    private var commands: [String] = []
    private var installedBytes: UInt64 = 0
    private var sentBytes: UInt64 = 0
    private var connections = 0

    /// What the install stream prints when its bytes have all arrived.
    var installOutput = "Success\n"
    /// Prints `installOutput` and ends the install stream after its first
    /// write instead of reading the whole file, as `createSession` failing
    /// does.
    var failsAtOnce = false
    /// The free space `df` reports for any path, in kibibytes.
    var guestFreeKiB: UInt64 = 64 << 20

    /// Every service a connection opened, in order.
    var servicesOpened: [String] { lock.withLock { opened } }
    /// The shell commands run.
    var shellCommands: [String] { lock.withLock { commands } }
    /// The bytes the install stream took, and the bytes `SEND` frames carried.
    var installBytes: UInt64 { lock.withLock { installedBytes } }
    var syncBytes: UInt64 { lock.withLock { sentBytes } }
    /// How many times a client connected.
    var connectionCount: Int { lock.withLock { connections } }

    func connect() throws -> any AdbTransport {
        var pair: [Int32] = [0, 0]
        guard socketpair(AF_UNIX, SOCK_STREAM, 0, &pair) == 0 else { throw AdbError.io(errno) }
        let client = DescriptorTransport(descriptor: pair[0])
        let server = DescriptorTransport(descriptor: pair[1])
        lock.withLock {
            connections += 1
            transports += [client, server]
        }
        let thread = Thread { [self] in
            try? serve(server)
            server.shutdown()
        }
        thread.name = "discarding-adbd"
        thread.start()
        return client
    }

    func shutdown() {
        let all = lock.withLock { transports }
        for transport in all { transport.shutdown() }
    }

    // MARK: Wire

    private func receive(_ transport: DescriptorTransport) throws -> AdbMessage {
        let header = try transport.readExactly(AdbWire.headerSize)
        let length = Int(Self.le32(header, 12))
        let payload = length > 0 ? try transport.readExactly(length) : Data()
        return AdbMessage(
            command: Self.le32(header, 0), arg0: Self.le32(header, 4), arg1: Self.le32(header, 8), payload: payload)
    }

    private func send(_ transport: DescriptorTransport, _ message: AdbMessage) throws {
        try transport.writeAll(message.encoded())
    }

    private static func le32(_ data: Data, _ offset: Int) -> UInt32 {
        (0..<4).reduce(UInt32(0)) { $0 | UInt32(data[data.startIndex + offset + $1]) << (8 * UInt32($1)) }
    }

    private static func bytes<T: FixedWidthInteger>(_ value: T) -> Data {
        withUnsafeBytes(of: value.littleEndian) { Data($0) }
    }

    private func serve(_ transport: DescriptorTransport) throws {
        let connect = try receive(transport)
        guard connect.command == AdbWire.cnxn else { return }
        try send(transport, AdbMessage(
            command: AdbWire.cnxn, arg0: AdbWire.version, arg1: UInt32(AdbWire.maxData),
            payload: Data("device::ro.product.name=fake;features=shell_v2,cmd,stat_v2,abb_exec\0".utf8)))
        var nextRemote: UInt32 = 100
        while true {
            let message = try autoreleasepool { try receive(transport) }
            guard message.command == AdbWire.open else { continue }
            var name = message.payload
            if name.last == 0 { name.removeLast() }
            let service = String(decoding: name, as: UTF8.self)
            lock.withLock { opened.append(service) }
            let stream = Channel(adbd: self, transport: transport, local: nextRemote, peer: message.arg0)
            nextRemote += 1
            try send(transport, AdbMessage(command: AdbWire.okay, arg0: stream.local, arg1: stream.peer))
            if service.hasPrefix("abb_exec:") {
                try serveInstall(stream, service: service)
            } else if service == "sync:" {
                try serveSync(stream)
            } else if service.hasPrefix("shell,v2,raw:") {
                try serveShell(stream, command: String(service.dropFirst("shell,v2,raw:".count)))
            }
            try stream.close()
        }
    }

    // MARK: Services

    private func serveInstall(_ stream: Channel, service: String) throws {
        let size = service.split(separator: "\0", omittingEmptySubsequences: false).last.flatMap { UInt64($0) } ?? 0
        var received: UInt64 = 0
        if failsAtOnce {
            // The first write is read and answered; then the exception.
            if let first = try autoreleasepool(invoking: { try stream.receive() }) { received += UInt64(first.count) }
        } else {
            while received < size {
                guard let piece = try autoreleasepool(invoking: { try stream.receive() }) else { break }
                received += UInt64(piece.count)
            }
        }
        lock.withLock { installedBytes += received }
        try stream.send(Data(installOutput.utf8))
    }

    private func serveShell(_ stream: Channel, command: String) throws {
        lock.withLock { commands.append(command) }
        var out = Data()
        if command.hasPrefix("df -Pk") {
            let text = "Filesystem 1024-blocks Used Available Capacity Mounted on\n"
                + "/dev/block/dm-7 67108864 1048576 \(guestFreeKiB) 2% /data\n"
            out = Data([1]) + Self.bytes(UInt32(text.utf8.count)) + Data(text.utf8)
        }
        let exit = Data([3]) + Self.bytes(UInt32(1)) + Data([0])
        try stream.send(out + exit)
    }

    private func serveSync(_ stream: Channel) throws {
        var lengths: [String: UInt64] = [:]
        while true {
            guard let header = try stream.take(8) else { return }
            let id = String(decoding: header.prefix(4), as: UTF8.self)
            let length = Int(Self.le32(header, 4))
            switch id {
            case "QUIT":
                return
            case "SEND":
                guard let request = try stream.take(length) else { return }
                let text = String(decoding: request, as: UTF8.self)
                let path = String(text[..<(text.lastIndex(of: ",") ?? text.endIndex)])
                var total: UInt64 = 0
                frames: while true {
                    guard let frame = try stream.take(8) else { return }
                    let frameLength = Int(Self.le32(frame, 4))
                    switch String(decoding: frame.prefix(4), as: UTF8.self) {
                    case "DATA":
                        guard try stream.skip(frameLength) else { return }
                        total += UInt64(frameLength)
                    case "DONE":
                        break frames
                    default:
                        return
                    }
                }
                lengths[path] = total
                lock.withLock { sentBytes += total }
                try stream.send(Data("OKAY".utf8) + Self.bytes(UInt32(0)))
            case "STA2":
                guard let request = try stream.take(length) else { return }
                let path = String(decoding: request, as: UTF8.self)
                let size = lengths[path]
                var reply = Data("STA2".utf8)
                reply += Self.bytes(UInt32(size == nil ? 2 : 0))
                reply += Self.bytes(UInt64(0)) + Self.bytes(UInt64(0))
                reply += Self.bytes(UInt32(size == nil ? 0 : 0o100664)) + Self.bytes(UInt32(1))
                reply += Self.bytes(UInt32(2000)) + Self.bytes(UInt32(2000))
                reply += Self.bytes(size ?? 0)
                reply += Self.bytes(UInt64(0)) + Self.bytes(UInt64(0)) + Self.bytes(UInt64(0))
                try stream.send(reply)
            default:
                try stream.send(Data("FAIL".utf8) + Self.bytes(UInt32(0)))
            }
        }
    }

    /// One stream of a connection, as a service sees it.
    private nonisolated final class Channel {
        unowned let adbd: DiscardingAdbd
        let transport: DescriptorTransport
        let local: UInt32
        let peer: UInt32
        private var closed = false
        private var payload = Data()
        private var offset = 0

        init(adbd: DiscardingAdbd, transport: DescriptorTransport, local: UInt32, peer: UInt32) {
            self.adbd = adbd
            self.transport = transport
            self.local = local
            self.peer = peer
        }

        /// The next write of the client, acknowledged; nil when it closed.
        func receive() throws -> Data? {
            while !closed {
                let message = try adbd.receive(transport)
                if message.command == AdbWire.write {
                    try adbd.send(transport, AdbMessage(command: AdbWire.okay, arg0: local, arg1: peer))
                    return message.payload
                }
                if message.command == AdbWire.close { closed = true }
            }
            return nil
        }

        /// Exactly `count` bytes of what the client wrote, or nil when it
        /// closed first.
        func take(_ count: Int) throws -> Data? {
            var taken = Data()
            while taken.count < count {
                if offset >= payload.count {
                    guard let next = try receive() else { return nil }
                    payload = next
                    offset = 0
                }
                let end = min(payload.count, offset + count - taken.count)
                taken.append(payload[(payload.startIndex + offset)..<(payload.startIndex + end)])
                offset = end
            }
            return taken
        }

        /// Drops `count` bytes of what the client wrote; false when it closed
        /// first.
        func skip(_ count: Int) throws -> Bool {
            var left = count
            while left > 0 {
                if offset >= payload.count {
                    guard let next = try autoreleasepool(invoking: { try receive() }) else { return false }
                    payload = next
                    offset = 0
                }
                let step = min(left, payload.count - offset)
                offset += step
                left -= step
            }
            return true
        }

        /// One write to the client, waiting for its acknowledgement. What the
        /// client writes meanwhile is acknowledged and dropped.
        func send(_ data: Data) throws {
            guard !closed else { return }
            try adbd.send(transport, AdbMessage(command: AdbWire.write, arg0: local, arg1: peer, payload: data))
            while true {
                let message = try adbd.receive(transport)
                if message.command == AdbWire.okay { return }
                if message.command == AdbWire.write {
                    try adbd.send(transport, AdbMessage(command: AdbWire.okay, arg0: local, arg1: peer))
                }
                if message.command == AdbWire.close {
                    closed = true
                    return
                }
            }
        }

        func close() throws {
            guard !closed else { return }
            closed = true
            try adbd.send(transport, AdbMessage(command: AdbWire.close, arg0: local, arg1: peer))
        }
    }
}
