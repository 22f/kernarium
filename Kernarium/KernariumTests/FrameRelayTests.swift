// SPDX-License-Identifier: GPL-2.0-or-later
import CoreVideo
import Foundation
import Testing
@testable import Kernarium

/// The relay between the runtime's frame thread and the main thread keeps only
/// the newest frame, so a busy main thread never queues frames.
@MainActor
struct FrameRelayTests {
    private func buffer(_ width: Int) throws -> CVPixelBuffer {
        var created: CVPixelBuffer?
        let status = CVPixelBufferCreate(
            kCFAllocatorDefault, width, 2, kCVPixelFormatType_32BGRA, nil,
            &created
        )
        #expect(status == kCVReturnSuccess)
        return try #require(created)
    }

    @Test
    func framesArrivingBeforeTheMainThreadRunsCollapseToTheNewest() throws {
        var hops: [@Sendable () -> Void] = []
        let relay = FrameRelay { hops.append($0) }
        var shown: [Int] = []
        relay.handler = { shown.append(CVPixelBufferGetWidth($0)) }

        relay.receive(try buffer(2))
        relay.receive(try buffer(4))
        relay.receive(try buffer(6))
        #expect(hops.count == 1)
        #expect(shown.isEmpty)

        hops.removeFirst()()
        #expect(shown == [6])
    }

    @Test
    func aFrameAfterTheHopRunsSchedulesAnother() throws {
        var hops: [@Sendable () -> Void] = []
        let relay = FrameRelay { hops.append($0) }
        var shown: [Int] = []
        relay.handler = { shown.append(CVPixelBufferGetWidth($0)) }

        relay.receive(try buffer(2))
        hops.removeFirst()()
        relay.receive(try buffer(4))
        #expect(hops.count == 1)
        hops.removeFirst()()
        #expect(shown == [2, 4])
    }

    @Test
    func aFrameWithNoHandlerIsDropped() throws {
        var hops: [@Sendable () -> Void] = []
        let relay = FrameRelay { hops.append($0) }
        relay.receive(try buffer(2))
        hops.removeFirst()()
        // Nothing to assert beyond not crashing; a later frame still arrives.
        var shown = 0
        relay.handler = { _ in shown += 1 }
        relay.receive(try buffer(2))
        hops.removeFirst()()
        #expect(shown == 1)
    }

    /// A delivery that is still waiting for the main thread when the window is
    /// disarmed holds the relay it came through. It must show nothing once the
    /// display has been flushed.
    @Test
    func aDeliveryPendingWhenTheWindowIsDisarmedShowsNothingAfterIt() async throws {
        let window = GuestPictureInPicture.shared
        window.arm(rate: (width: 64, framesPerSecond: 2))
        let relay = try #require(window.relay)
        let production = relay.handler
        var shown = 0
        relay.handler = { frame in
            shown += 1
            production?(frame)
        }

        // The hop to the main thread cannot run before this test yields.
        relay.receive(try buffer(4))
        window.disarm()
        #expect(window.relay == nil)
        try await Task.sleep(for: .milliseconds(200))
        #expect(shown == 0)
    }
}
