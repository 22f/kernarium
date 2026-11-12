// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CoreMotion
import Foundation
import os

private nonisolated struct MotionConnector: GuestStreamOpener {
    func openStream() throws -> Int32 {
        let descriptor = kernarium_runtime_loader_connect_vsock(MotionStream.port, 250)
        guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: -descriptor) ?? .EIO) }
        return descriptor
    }
}

/// The device's accelerometer and gyroscope. Core Motion needs no iOS
/// permission for raw readings.
///
/// The manager is the process's one and is never released: Core Motion asks
/// for one per application, and releasing a manager crashes inside Core
/// Motion, in work on its own threads (reproduced with the manager alone, in
/// a loop). A model built and released over again reaches it. A device
/// starts and stops its sensors on the manager and stops the ones it started
/// when it goes.
private final class CoreMotionDevice: MotionDevice, @unchecked Sendable {
    private static let sharedManager = CMMotionManager()
    private var manager: CMMotionManager { Self.sharedManager }
    private let queue: OperationQueue = {
        let queue = OperationQueue()
        queue.name = "andarium-motion"
        queue.maxConcurrentOperationCount = 1
        return queue
    }()
    private let started = OSAllocatedUnfairLock(initialState: Set<MotionSample.Sensor>())

    deinit {
        for sensor in started.withLock({ $0 }) { stop(sensor) }
    }

    var hasAccelerometer: Bool { manager.isAccelerometerAvailable }
    var hasGyroscope: Bool { manager.isGyroAvailable }

    func start(
        _ sensor: MotionSample.Sensor, interval: TimeInterval,
        handler: @escaping @Sendable (Double, Double, Double) -> Void,
        failure: @escaping @Sendable () -> Void
    ) {
        stop(sensor)
        switch sensor {
        case .accelerometer:
            guard manager.isAccelerometerAvailable else { return }
            manager.accelerometerUpdateInterval = interval
            started.withLock { _ = $0.insert(sensor) }
            manager.startAccelerometerUpdates(to: queue) { data, error in
                if error != nil { failure() }
                guard let a = data?.acceleration else { return }
                handler(a.x, a.y, a.z)
            }
        case .gyroscope:
            guard manager.isGyroAvailable else { return }
            manager.gyroUpdateInterval = interval
            started.withLock { _ = $0.insert(sensor) }
            manager.startGyroUpdates(to: queue) { data, error in
                if error != nil { failure() }
                guard let g = data?.rotationRate else { return }
                handler(g.x, g.y, g.z)
            }
        }
    }

    func stop(_ sensor: MotionSample.Sensor) {
        started.withLock { _ = $0.remove(sensor) }
        switch sensor {
        case .accelerometer: manager.stopAccelerometerUpdates()
        case .gyroscope: manager.stopGyroUpdates()
        }
    }
}

/// The device's motion for the running guest. Core Motion runs only for a
/// booted, foreground guest on a device that has the sensors, and then only
/// for the sensors an application in the guest has active (`MotionSession`).
/// The axes are set when a run begins and never change per sample
/// (`MotionMount` decides when they move). No samples are retained across
/// backgrounding, power-off, or a guest restart.
@MainActor
final class MotionSensors {
    private let device: any MotionDevice
    private let opener: any GuestStreamOpener
    private lazy var session = MotionSession(opener: opener, device: device)
    private var axes = MotionSample.Axes.portrait

    /// The guest's motion service is reached over the runtime's vsock, and the
    /// sensors are the device's own; a test gives others.
    init(opener: any GuestStreamOpener = MotionConnector(), device: any MotionDevice = CoreMotionDevice()) {
        self.opener = opener
        self.device = device
    }

    /// The device has sensors to share. A Mac and the Simulator have none, and
    /// then nothing is read and no connection is held for the sensors.
    var available: Bool { device.hasAccelerometer || device.hasGyroscope }

    /// What is wanted now: the guest being in front, the axes the sensors are
    /// mounted to, and whether the guest follows the device's orientation
    /// (which holds the connection on a device without sensors too).
    func update(foreground: Bool, axes: MotionSample.Axes, followsOrientation: Bool = false) {
        self.axes = axes
        session.update(foreground: foreground, axes: axes, followsOrientation: followsOrientation)
    }

    /// The device's orientation, or nil once it is not being observed. The
    /// guest is given it if it takes orientations.
    func setOrientation(_ orientation: GuestOrientation?) { session.setOrientation(orientation) }

    /// The period the readings belong to is over (the mount or the rotation
    /// policy changed); the next update begins a new run.
    func restart() { session.restart() }

    func stop() {
        session.update(foreground: false, axes: axes)
        session.setOrientation(nil)
    }
}
