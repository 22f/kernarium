// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing

@testable import AndariumCore

/// What the host sends a guest that has the device-orientation sensor, and
/// reads from one: the mapping from iOS's orientation, the record against the
/// golden frames, the guest's "ANC2" frames, the rules for when a record is
/// sent, and the session over a fake guest with the Gyroscope switch on and
/// off.
struct MotionOrientationTests {
    // MARK: The mapping from iOS

    /// The device's orientation as Core Motion's gravity says it, in the
    /// device's own frame (x to the right of the screen, y to its top): the
    /// vector of an upright portrait device is (0, -1). Android's definition
    /// of the sensor's value (types.hal, DEVICE_ORIENTATION) names the device
    /// axis that points up: +y is 0, +x is 1, -y is 2 and -x is 3. Gravity
    /// points down, so the axis that points up is the opposite one.
    private func androidValue(gravityX x: Int, gravityY y: Int) -> UInt8? {
        switch (-x, -y) {
        case (0, 1): 0
        case (1, 0): 1
        case (0, -1): 2
        case (-1, 0): 3
        default: nil
        }
    }

    @Test func eachIOSOrientationMapsToTheNumberAndroidsDefinitionGivesItsGravity() {
        // The gravity of each orientation is the device check's own: -x is
        // reported landscapeLeft (ROTCHECK, iPhone 15 Pro Max, iOS 27.0.1), and
        // the others follow from the device's turn.
        let table: [(DeviceOrientationReading, gravityX: Int, gravityY: Int)] = [
            (.portrait, 0, -1),
            (.landscapeLeft, -1, 0),
            (.portraitUpsideDown, 0, 1),
            (.landscapeRight, 1, 0),
        ]
        for (reading, x, y) in table {
            let mapped = GuestOrientation(reading)
            #expect(mapped?.rawValue == androidValue(gravityX: x, gravityY: y), "\(reading)")
        }
        // Spelled out, the table the app states.
        #expect(GuestOrientation(.portrait) == .upright)
        #expect(GuestOrientation(.landscapeLeft) == .turnedCounterClockwise)
        #expect(GuestOrientation(.portraitUpsideDown) == .upsideDown)
        #expect(GuestOrientation(.landscapeRight) == .turnedClockwise)
        #expect(GuestOrientation.allCases.map(\.rawValue) == [0, 1, 2, 3])
    }

    @Test func noOrientationIsReportedForTheFlatOrUnknown() {
        #expect(GuestOrientation(.unknown) == nil)
        #expect(GuestOrientation(.faceUp) == nil)
        #expect(GuestOrientation(.faceDown) == nil)
        #expect(DeviceOrientationReading.allCases.compactMap(GuestOrientation.init).count == 4)
    }

    @Test func theReadingsAreUIKitsNumbers() {
        // UIDeviceOrientation: unknown 0, portrait 1, portraitUpsideDown 2,
        // landscapeLeft 3, landscapeRight 4, faceUp 5, faceDown 6. The
        // application checks them against UIKit itself.
        #expect(DeviceOrientationReading.allCases.map(\.rawValue) == [0, 1, 2, 3, 4, 5, 6])
        #expect(DeviceOrientationReading(rawValue: 3) == .landscapeLeft)
        #expect(DeviceOrientationReading(rawValue: 4) == .landscapeRight)
        #expect(DeviceOrientationReading(rawValue: 7) == nil)
    }

    // MARK: The wire

    @Test func theRecordIsTheGoldenFrame() throws {
        for orientation in GuestOrientation.allCases {
            let record = OrientationRecord.encoded(orientation)
            #expect(Array(record) == (try MotionOrientationFixture.bytes("orientation-\(orientation.rawValue)")))
            #expect(record.count == 20)
        }
    }

    @Test func theGuestsFramesAreReadAsTheFixtureSays() throws {
        let expected: [String: MotionDemand] = [
            "control-v1-idle": MotionDemand(),
            "control-v1-motion": MotionDemand(accelerometer: 20_000, gyroscope: 1_000_000),
            "control-v2-idle": MotionDemand(orientationSensor: .idle),
            "control-v2-orientation": MotionDemand(orientationSensor: .active),
            "control-v2-all": MotionDemand(accelerometer: 20_000, gyroscope: 1_000_000, orientationSensor: .active),
        ]
        var seen = 0
        for frame in try MotionOrientationFixture.frames() where frame.name.hasPrefix("control-") {
            seen += 1
            #expect(MotionDemand(frame: Data(frame.bytes)) == expected[frame.name], "\(frame.name)")
        }
        #expect(seen == expected.count)
        var rejected = 0
        for frame in try MotionOrientationFixture.frames() where frame.name.hasPrefix("reject-control-") {
            rejected += 1
            #expect(MotionDemand(frame: Data(frame.bytes)) == nil, "\(frame.name)")
        }
        #expect(rejected == 3)
        #expect(MotionDemand(orientationSensor: .idle).acceptsOrientation)
        #expect(!MotionDemand().acceptsOrientation)
    }

    // MARK: When a record is sent

    private func following() -> MotionSampling {
        var sampling = MotionSampling()
        sampling.followsOrientation = true
        sampling.foreground = true
        sampling.connectionChanged(true)
        return sampling
    }

    @Test func nothingIsSentToAGuestThatHasNotSaidItTakesOrientations() {
        var sampling = following()
        sampling.orientation = .turnedClockwise
        #expect(sampling.orientationToSend() == nil)
        // An older image: its frames are "ANC1".
        sampling.received(MotionDemand(accelerometer: 20_000))
        #expect(sampling.orientationToSend() == nil)
        sampling.received(MotionDemand(orientationSensor: .idle))
        #expect(sampling.orientationToSend() == .turnedClockwise)
    }

    @Test func eachOrientationIsSentOnceAndOnlyOnAChange() {
        var sampling = following()
        sampling.received(MotionDemand(orientationSensor: .idle))
        #expect(sampling.orientationToSend() == nil)
        sampling.orientation = .upright
        #expect(sampling.orientationToSend() == .upright)
        #expect(sampling.orientationToSend() == nil)
        sampling.orientation = .upright
        #expect(sampling.orientationToSend() == nil)
        sampling.orientation = .turnedCounterClockwise
        #expect(sampling.orientationToSend() == .turnedCounterClockwise)
        // A flat device reports none: the guest keeps the last it was given.
        sampling.orientation = nil
        #expect(sampling.orientationToSend() == nil)
        sampling.orientation = .turnedCounterClockwise
        #expect(sampling.orientationToSend() == nil)
        sampling.orientation = .upsideDown
        #expect(sampling.orientationToSend() == .upsideDown)
    }

    @Test func theCurrentOrientationIsSentAgainWhenTheGuestEnablesTheSensorAndOnANewConnection() {
        var sampling = following()
        sampling.orientation = .turnedClockwise
        sampling.received(MotionDemand(orientationSensor: .idle))
        #expect(sampling.orientationToSend() == .turnedClockwise)
        // The window manager enables the sensor: once more.
        sampling.received(MotionDemand(orientationSensor: .active))
        #expect(sampling.orientationToSend() == .turnedClockwise)
        // Any other frame while it stays enabled changes nothing.
        sampling.received(MotionDemand(accelerometer: 20_000, orientationSensor: .active))
        #expect(sampling.orientationToSend() == nil)
        // It is disabled and enabled again.
        sampling.received(MotionDemand(orientationSensor: .idle))
        #expect(sampling.orientationToSend() == nil)
        sampling.received(MotionDemand(orientationSensor: .active))
        #expect(sampling.orientationToSend() == .turnedClockwise)
        // A new connection (the guest restarted its service, or the host came
        // back from the background) starts again from what the service says.
        sampling.connectionChanged(false)
        sampling.connectionChanged(true)
        #expect(sampling.orientationToSend() == nil)
        sampling.received(MotionDemand(orientationSensor: .idle))
        #expect(sampling.orientationToSend() == .turnedClockwise)
    }

    @Test func nothingIsSentWhenTheGuestDoesNotFollowOrIsNotInFront() {
        var sampling = following()
        sampling.orientation = .upsideDown
        sampling.received(MotionDemand(orientationSensor: .idle))
        sampling.foreground = false
        #expect(sampling.orientationToSend() == nil)
        sampling.foreground = true
        sampling.followsOrientation = false
        #expect(sampling.orientationToSend() == nil)
        sampling.followsOrientation = true
        sampling.connectionChanged(false)
        #expect(sampling.orientationToSend() == nil)
    }

    @Test func theConnectionIsHeldForTheOrientationWithTheSwitchOffAndSensorsStayOff() {
        var sampling = following()
        #expect(!sampling.enabled && sampling.wantsConnection)
        // The legacy continuous reading of a service that has not spoken is
        // the switch's, not the orientation's.
        #expect(sampling.plan == .off)
        sampling.received(MotionDemand(accelerometer: 20_000, orientationSensor: .active))
        #expect(sampling.plan == .off)
        sampling.enabled = true
        #expect(sampling.plan != .off)
        sampling.followsOrientation = false
        #expect(sampling.wantsConnection)
        sampling.enabled = false
        #expect(!sampling.wantsConnection)
        sampling.followsOrientation = true
        sampling.foreground = false
        #expect(!sampling.wantsConnection)
    }

    // MARK: A session over a fake guest

    /// The guest's end of the motion connection: it opens one socket pair per
    /// connection, says what its service says and keeps what the host writes.
    private final class Guest: GuestStreamOpener, @unchecked Sendable {
        private let lock = NSLock()
        private var ends: [Int32] = []
        private var down = false
        func openStream() throws -> Int32 {
            if lock.withLock({ down }) { throw POSIXError(.ECONNREFUSED) }
            var sockets: [Int32] = [-1, -1]
            guard socketpair(AF_UNIX, SOCK_STREAM, 0, &sockets) == 0 else { throw POSIXError(.EIO) }
            lock.withLock { ends.append(sockets[1]) }
            return sockets[0]
        }
        var connections: Int { lock.withLock { ends.count } }
        var current: Int32? { lock.withLock { ends.last } }
        deinit { for end in ends { close(end) } }

        /// Android powers off: nothing listens.
        func goDown() { lock.withLock { down = true } }

        /// A frame of the service, as the fixture has it.
        func say(_ name: String) throws {
            let bytes = try MotionOrientationFixture.bytes(name)
            if let end = current { _ = write(end, bytes, bytes.count) }
        }

        /// The first `count` bytes of a frame of the service, as a connection that breaks off would leave them.
        func sayPartly(_ name: String, count: Int) throws {
            let bytes = Array(try MotionOrientationFixture.bytes(name).prefix(count))
            if let end = current { _ = write(end, bytes, bytes.count) }
        }

        /// The service goes away without Android powering off: the connection ends, and a new one is made.
        func hangUp() {
            if let end = current { shutdown(end, SHUT_RDWR) }
        }

        /// The next record the host wrote, or nil within `timeout` milliseconds.
        func record(timeout: Int = 1500) -> [UInt8]? {
            guard let end = current else { return nil }
            var result: [UInt8] = []
            let deadline = ContinuousClock.now + .milliseconds(Int64(timeout))
            while result.count < 20, ContinuousClock.now < deadline {
                var descriptor = pollfd(fd: end, events: Int16(POLLIN), revents: 0)
                guard poll(&descriptor, 1, 10) > 0 else { continue }
                var bytes = [UInt8](repeating: 0, count: 20 - result.count)
                let size = recv(end, &bytes, bytes.count, MSG_DONTWAIT)
                guard size > 0 else { break }
                result.append(contentsOf: bytes.prefix(size))
            }
            return result.count == 20 ? result : nil
        }

        func sawClose(timeout: Int = 1500) -> Bool {
            guard let end = current else { return false }
            var descriptor = pollfd(fd: end, events: Int16(POLLIN), revents: 0)
            guard poll(&descriptor, 1, Int32(timeout)) > 0 else { return false }
            var byte: UInt8 = 0
            return recv(end, &byte, 1, MSG_DONTWAIT) == 0
        }
    }

    private final class Device: MotionDevice, @unchecked Sendable {
        let hasAccelerometer = true
        let hasGyroscope = true
        private let lock = NSLock()
        private var running: Set<MotionSample.Sensor> = []
        func start(
            _ sensor: MotionSample.Sensor, interval: TimeInterval,
            handler: @escaping @Sendable (Double, Double, Double) -> Void, failure: @escaping @Sendable () -> Void
        ) { lock.withLock { _ = running.insert(sensor) } }
        func stop(_ sensor: MotionSample.Sensor) { lock.withLock { _ = running.remove(sensor) } }
        var started: Set<MotionSample.Sensor> { lock.withLock { running } }
    }

    private final class Problems: @unchecked Sendable {
        private let lock = NSLock()
        private var seen: [MotionProblem?] = []
        func record(_ problem: MotionProblem?) { lock.withLock { seen.append(problem) } }
        var failures: [MotionProblem] { lock.withLock { seen.compactMap { $0 } } }
    }

    private func eventually(_ timeout: Double = 3, _ condition: () -> Bool) -> Bool {
        let deadline = ContinuousClock.now + .milliseconds(Int64(timeout * 1000))
        while ContinuousClock.now < deadline {
            if condition() { return true }
            usleep(5_000)
        }
        return condition()
    }

    private func session(_ guest: Guest, _ device: Device, _ problems: Problems = Problems()) -> MotionSession {
        MotionSession(opener: guest, device: device, retryDelay: .milliseconds(40), problem: problems.record)
    }

    private func fixture(_ orientation: GuestOrientation) throws -> [UInt8] {
        try MotionOrientationFixture.bytes("orientation-\(orientation.rawValue)")
    }

    @Test func theSwitchOffStillGivesAGuestThatTakesThemTheDevicesOrientation() throws {
        let guest = Guest()
        let device = Device()
        let session = session(guest, device)
        session.update(enabled: false, foreground: true, axes: .portrait, followsOrientation: true)
        #expect(eventually { guest.connections == 1 })
        session.setOrientation(.turnedCounterClockwise)
        session.settle()
        // The service has not spoken: nothing is sent, and no sensor runs.
        #expect(guest.record(timeout: 80) == nil)
        try guest.say("control-v2-idle")
        #expect(guest.record() == (try fixture(.turnedCounterClockwise)))
        // Only changes after that, in order.
        session.setOrientation(.turnedCounterClockwise)
        session.setOrientation(.upsideDown)
        session.setOrientation(nil)
        session.setOrientation(.upright)
        #expect(guest.record() == (try fixture(.upsideDown)))
        #expect(guest.record() == (try fixture(.upright)))
        #expect(guest.record(timeout: 80) == nil)
        // The window manager enables the sensor: the current value once more.
        try guest.say("control-v2-orientation")
        #expect(guest.record() == (try fixture(.upright)))
        session.settle()
        #expect(device.started.isEmpty)
        session.update(enabled: false, foreground: false, axes: .portrait, followsOrientation: true)
        #expect(guest.sawClose())
    }

    @Test func aServiceWithoutTheSensorIsSentNothingAndIsNotReportedAsMissing() throws {
        let guest = Guest()
        let device = Device()
        let problems = Problems()
        let session = session(guest, device, problems)
        session.update(enabled: false, foreground: true, axes: .portrait, followsOrientation: true)
        #expect(eventually { guest.connections == 1 })
        session.setOrientation(.turnedClockwise)
        try guest.say("control-v1-idle")
        session.settle()
        #expect(guest.record(timeout: 100) == nil)
        #expect(problems.failures.isEmpty)
        session.update(enabled: false, foreground: false, axes: .portrait, followsOrientation: true)
    }

    @Test func aGuestThatIsNotThereIsNotReportedToTheSwitchWhileItIsOff() {
        let guest = Guest()
        let device = Device()
        let problems = Problems()
        let off = session(guest, device, problems)
        guest.goDown()
        off.update(enabled: false, foreground: true, axes: .portrait, followsOrientation: true)
        usleep(200_000)
        off.settle()
        #expect(problems.failures.isEmpty)
        off.update(enabled: false, foreground: false, axes: .portrait)
        // With the switch on the same absence is the switch's problem.
        let on = session(guest, device, problems)
        on.update(enabled: true, foreground: true, axes: .portrait, followsOrientation: true)
        #expect(eventually { problems.failures == [.guestUnavailable] })
        on.update(enabled: false, foreground: false, axes: .portrait)
    }

    @Test func comingBackFromTheBackgroundGivesTheCurrentOrientationOnANewConnection() throws {
        let guest = Guest()
        let device = Device()
        let session = session(guest, device)
        session.update(enabled: true, foreground: true, axes: .portrait, followsOrientation: true)
        #expect(eventually { guest.connections == 1 })
        session.setOrientation(.turnedClockwise)
        try guest.say("control-v2-idle")
        #expect(guest.record() == (try fixture(.turnedClockwise)))
        // The guest leaves the foreground: the connection ends and the
        // application forgets the orientation, whose reports stop.
        session.update(enabled: true, foreground: false, axes: .portrait, followsOrientation: true)
        session.setOrientation(nil)
        #expect(guest.sawClose())
        session.update(enabled: true, foreground: true, axes: .portrait, followsOrientation: true)
        #expect(eventually { guest.connections == 2 })
        try guest.say("control-v2-idle")
        session.settle()
        // Nothing is sent from before the background; the first report after
        // it is the one the guest is given.
        #expect(guest.record(timeout: 100) == nil)
        session.setOrientation(.upsideDown)
        #expect(guest.record() == (try fixture(.upsideDown)))
        session.update(enabled: false, foreground: false, axes: .portrait)
    }

    @Test func aNewConnectionStartsFromWhatTheServiceSaysOnIt() throws {
        let guest = Guest()
        let device = Device()
        let session = session(guest, device)
        session.update(enabled: false, foreground: true, axes: .portrait, followsOrientation: true)
        #expect(eventually { guest.connections == 1 })
        session.setOrientation(.turnedClockwise)
        // The start of a frame ("ANC"), and the connection ends with it. Read as the start of the
        // next connection's first frame it would be a bad one.
        try guest.sayPartly("control-v2-orientation", count: 3)
        session.settle()
        guest.hangUp()
        #expect(eventually { guest.connections == 2 })
        // The new connection is not the old one: the start of the frame is gone, and nothing is
        // sent until the service says it takes orientations on this connection.
        #expect(guest.record(timeout: 100) == nil)
        // Its first frame is a whole one, with a sensor in use and periods already set:
        // it is read at once, and the orientation goes out.
        try guest.say("control-v2-all")
        #expect(guest.record() == (try fixture(.turnedClockwise)))
        // The service says it takes orientations on one connection, and then ends it.
        guest.hangUp()
        #expect(eventually { guest.connections == 3 })
        // A frame on the third connection is not read as the continuation of anything, and
        // what the second said does not carry over.
        #expect(guest.record(timeout: 100) == nil)
        try guest.say("control-v2-idle")
        #expect(guest.record() == (try fixture(.turnedClockwise)))
        session.update(enabled: false, foreground: false, axes: .portrait)
    }

    @Test func theSwitchDecidesTheSensorsWhateverTheGuestAsksFor() throws {
        let guest = Guest()
        let device = Device()
        let session = session(guest, device)
        session.update(enabled: false, foreground: true, axes: .portrait, followsOrientation: true)
        #expect(eventually { guest.connections == 1 })
        try guest.say("control-v2-all")
        session.settle()
        // The service asks for the accelerometer and the gyroscope, and the switch is off:
        // no reading is started, and the orientation still flows.
        #expect(device.started.isEmpty)
        session.setOrientation(.upsideDown)
        #expect(guest.record() == (try fixture(.upsideDown)))
        #expect(device.started.isEmpty)
        // The switch comes on: the sensors the service asked for are read, and the orientation
        // does not stop.
        session.update(enabled: true, foreground: true, axes: .portrait, followsOrientation: true)
        #expect(eventually { device.started == [.accelerometer, .gyroscope] })
        session.setOrientation(.upright)
        #expect(guest.record() == (try fixture(.upright)))
        // And off again: the readings stop, the connection stays.
        session.update(enabled: false, foreground: true, axes: .portrait, followsOrientation: true)
        #expect(eventually { device.started.isEmpty })
        session.setOrientation(.turnedCounterClockwise)
        #expect(guest.record() == (try fixture(.turnedCounterClockwise)))
        #expect(guest.connections == 1)
        session.update(enabled: false, foreground: false, axes: .portrait)
    }

    @Test func anOlderServiceKeepsTheOldMotionPathAndGetsNoRecord() throws {
        let guest = Guest()
        let device = Device()
        let problems = Problems()
        let session = session(guest, device, problems)
        session.update(enabled: true, foreground: true, axes: .portrait, followsOrientation: true)
        #expect(eventually { guest.connections == 1 })
        session.setOrientation(.turnedClockwise)
        // "ANC1": the sensors it asks for are read, as before, and no orientation is sent.
        try guest.say("control-v1-motion")
        #expect(eventually { device.started == [.accelerometer, .gyroscope] })
        #expect(guest.record(timeout: 100) == nil)
        try guest.say("control-v1-idle")
        #expect(eventually { device.started.isEmpty })
        session.setOrientation(.upsideDown)
        #expect(guest.record(timeout: 100) == nil)
        #expect(guest.connections == 1)
        #expect(problems.failures.isEmpty)
        session.update(enabled: false, foreground: false, axes: .portrait)
    }

    @Test func aSessionThatDoesNotFollowTheOrientationSendsNone() throws {
        let guest = Guest()
        let device = Device()
        let session = session(guest, device)
        session.update(enabled: true, foreground: true, axes: .portrait)
        #expect(eventually { guest.connections == 1 })
        session.setOrientation(.turnedClockwise)
        try guest.say("control-v2-orientation")
        session.settle()
        #expect(guest.record(timeout: 100) == nil)
        // The sensors are the switch's, as before: this guest uses neither.
        #expect(device.started.isEmpty)
        session.update(enabled: false, foreground: false, axes: .portrait)
    }
}
