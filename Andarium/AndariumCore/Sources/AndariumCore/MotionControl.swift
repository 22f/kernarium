// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What the guest's sensor service tells the host over the motion connection:
/// which of the two sensors an application has active, and the sampling period
/// each asked for. The service sends one frame when the host connects and
/// another at every change; the host answers only with sensor readings and,
/// to a service that has one, the device's orientation.
///
/// A frame is 16 bytes, little endian: "ANC1", a uint32 mask of the active
/// sensors (bit 0 accelerometer, bit 1 gyroscope), then the uint32 sampling
/// period in microseconds of the accelerometer and of the gyroscope. A period
/// outside 10 ms to 1 s is brought into it, and the period of a sensor that is
/// not active is not read. A frame with another magic or a reserved mask bit
/// is a protocol error.
///
/// A service that has the device-orientation sensor sends "ANC2" instead, with
/// the same layout and bit 2 of the mask for that sensor (it has no period).
/// Only such a service takes orientation records (`OrientationRecord`): an
/// older one ends the connection on one, so the host sends none before an
/// "ANC2" frame has arrived on the connection.
public struct MotionDemand: Equatable, Sendable {
    /// Whether the guest has the device-orientation sensor, and whether the
    /// window manager has it enabled.
    public enum OrientationSensor: Equatable, Sendable {
        /// The service sends "ANC1": it has no such sensor.
        case absent
        /// The service has the sensor and nothing has it enabled.
        case idle
        /// The service has the sensor and the window manager has it enabled.
        case active
    }

    public static let frameSize = 16
    public static let minimumPeriod: UInt32 = 10_000
    public static let maximumPeriod: UInt32 = 1_000_000

    /// The requested sampling period of each sensor an application has
    /// active, in microseconds; nil for a sensor nobody uses.
    public var accelerometer: UInt32?
    public var gyroscope: UInt32?
    public var orientationSensor: OrientationSensor

    public init(
        accelerometer: UInt32? = nil, gyroscope: UInt32? = nil, orientationSensor: OrientationSensor = .absent
    ) {
        self.accelerometer = accelerometer.map(Self.clamped)
        self.gyroscope = gyroscope.map(Self.clamped)
        self.orientationSensor = orientationSensor
    }

    /// No application uses the accelerometer or the gyroscope.
    public var isIdle: Bool { accelerometer == nil && gyroscope == nil }

    /// The guest takes orientation records.
    public var acceptsOrientation: Bool { orientationSensor != .absent }

    public init?(frame: Data) {
        guard frame.count == Self.frameSize else { return nil }
        let bytes = [UInt8](frame)
        guard bytes[0] == 0x41, bytes[1] == 0x4e, bytes[2] == 0x43 else { return nil }
        let withOrientation: Bool
        switch bytes[3] {
        case 0x31: withOrientation = false
        case 0x32: withOrientation = true
        default: return nil
        }
        func word(_ offset: Int) -> UInt32 {
            (0..<4).reduce(0) { $0 | UInt32(bytes[offset + $1]) << UInt32(8 * $1) }
        }
        let mask = word(4)
        guard mask & ~(withOrientation ? 0b111 : 0b11) == 0 else { return nil }
        self.init(
            accelerometer: mask & 1 != 0 ? word(8) : nil,
            gyroscope: mask & 2 != 0 ? word(12) : nil,
            orientationSensor: !withOrientation ? .absent : mask & 4 != 0 ? .active : .idle
        )
    }

    private static func clamped(_ period: UInt32) -> UInt32 {
        min(max(period, minimumPeriod), maximumPeriod)
    }
}

/// How often each sensor of the device is read, in seconds; nil leaves it off.
public struct MotionPlan: Equatable, Sendable {
    public var accelerometer: TimeInterval?
    public var gyroscope: TimeInterval?

    public init(accelerometer: TimeInterval? = nil, gyroscope: TimeInterval? = nil) {
        self.accelerometer = accelerometer
        self.gyroscope = gyroscope
    }

    public static let off = MotionPlan()

    public func interval(of sensor: MotionSample.Sensor) -> TimeInterval? {
        switch sensor {
        case .accelerometer: accelerometer
        case .gyroscope: gyroscope
        }
    }
}

/// When the device's sensors are read, and how often.
///
/// The host reads a sensor only while the device has it, the guest is in the
/// foreground, the guest's sensor service is connected and an application in
/// the guest has the sensor active. The connection itself is held while the
/// device has sensors and the guest is in the foreground, or while the guest
/// follows the device's orientation and is in the foreground, even when
/// nothing is active, because the guest says what it needs over it.
///
/// A guest image whose service never sends a frame (the one before the
/// service learned to) is served as it always was: until a frame arrives,
/// both sensors are read continuously at 100 Hz. A lost connection forgets
/// what the last service said, since the next connection may be to another.
public struct MotionSampling: Equatable, Sendable {
    /// The continuous rate of a service that does not say what it needs.
    public static let legacyInterval: TimeInterval = 0.01
    /// The host reads a sensor this many times per requested period, so the
    /// guest's own pacing, not the timing of the host's readings, sets the
    /// rate applications see.
    public static let oversample = 2.0

    /// The device has sensors to share: an accelerometer or a gyroscope. A
    /// device without (a Mac, the Simulator) reads nothing and holds no
    /// connection for them.
    public var hasSensors = false
    /// The guest runs a normal boot, in front, and has settled its rotation.
    public var foreground = false
    /// The guest follows the device's orientation (an iPhone): the connection
    /// is held for it whether or not the device has sensors, because the
    /// orientation is the display's behaviour and not a reading an application
    /// takes.
    public var followsOrientation = false
    /// The device's orientation now; nil until the device reports one, and
    /// again nil when it stops reporting (the foreground ended).
    public var orientation: GuestOrientation?
    public private(set) var connected = false
    /// What the service last said; nil until it says anything.
    public private(set) var demand: MotionDemand?
    /// What the guest was last given on this connection.
    private var orientationGiven: GuestOrientation?

    public init() {}

    /// The connection to the service is wanted.
    public var wantsConnection: Bool { (hasSensors || followsOrientation) && foreground }

    public mutating func connectionChanged(_ connected: Bool) {
        self.connected = connected
        demand = nil
        orientationGiven = nil
    }

    public mutating func received(_ demand: MotionDemand) {
        // The window manager enabling the sensor is a new subscription, which
        // is given the device's orientation again (the guest would hold it
        // already; this does not depend on that).
        if demand.orientationSensor == .active, self.demand?.orientationSensor != .active {
            orientationGiven = nil
        }
        self.demand = demand
    }

    /// The orientation to send the guest now, if there is one, once: the
    /// device's, when the guest has said that it takes them and has not been
    /// given this one on this connection. Nothing is sent for an orientation
    /// the device has not reported, and the guest keeps the last it was given.
    public mutating func orientationToSend() -> GuestOrientation? {
        guard followsOrientation, foreground, connected, demand?.acceptsOrientation == true,
              let orientation, orientation != orientationGiven
        else { return nil }
        orientationGiven = orientation
        return orientation
    }

    public var plan: MotionPlan {
        guard hasSensors, foreground, connected else { return .off }
        guard let demand else {
            return MotionPlan(accelerometer: Self.legacyInterval, gyroscope: Self.legacyInterval)
        }
        return MotionPlan(
            accelerometer: demand.accelerometer.map(Self.interval(forPeriod:)),
            gyroscope: demand.gyroscope.map(Self.interval(forPeriod:))
        )
    }

    /// The reading interval for a requested period in microseconds.
    public static func interval(forPeriod microseconds: UInt32) -> TimeInterval {
        max(legacyInterval, Double(microseconds) / 1_000_000 / oversample)
    }
}
