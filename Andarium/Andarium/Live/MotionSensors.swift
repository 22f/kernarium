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

/// What the compass needs of Core Motion, decided apart from the manager so a
/// test can reach every case.
nonisolated enum CompassSource {
    /// Core Motion reports once, in the handler, that the device must move for
    /// its field to be calibrated (`CMErrorDeviceRequiresMovement`, 101). The
    /// readings follow once the device is moved, so it is no failure. (The
    /// probe never saw it, so what Core Motion delivers meanwhile is not known.)
    static func isCalibrationNotice(_ error: any Error) -> Bool {
        let error = error as NSError
        return error.domain == CMErrorDomain && error.code == Int(CMErrorDeviceRequiresMovement.rawValue)
    }

    /// Whether the person's motion permission leaves the field readable.
    /// iOS 27.2 states one (`CMMotionManager.authorizationStatus`); nothing is
    /// requested and no usage string is added, so a status that says the
    /// person or the system refused ends the offer. Not determined and
    /// authorized leave it.
    static func permits(_ status: CMAuthorizationStatus) -> Bool {
        status != .denied && status != .restricted
    }

    /// What the offer was decided on. A reading is nil when the answers before it
    /// made it needless: a Mac has no magnetometer, and iOS asks nothing more
    /// there (the frames and the permission status are not read on a Mac).
    struct Assessment: Equatable {
        var available = false
        var northFrame: Bool?
        /// `.notStated` before iOS 27.2, `.notRead` when an earlier answer ended the question.
        var permission = Permission.notRead
    }

    enum Permission: Equatable {
        case notRead
        case notStated
        case status(CMAuthorizationStatus)
    }

    /// The guest is offered a magnetometer only where the device has one, can
    /// run device motion in the magnetic-north frame (the field is taken from
    /// it, calibrated, never from the raw magnetometer), and the permission,
    /// where the system states one, does not refuse it. The two answers that
    /// every device can give come first; the frames and the permission status,
    /// which the system layer of an iPad app on a Mac may not implement, are
    /// read only when a magnetometer and device motion exist.
    static func assess(
        magnetometer: Bool, deviceMotion: Bool, northFrame: () -> Bool, authorization: () -> CMAuthorizationStatus?
    ) -> Assessment {
        guard magnetometer, deviceMotion else { return Assessment() }
        var assessment = Assessment()
        let frame = northFrame()
        assessment.northFrame = frame
        guard frame else { return assessment }
        let status = authorization()
        assessment.permission = status.map(Permission.status) ?? .notStated
        assessment.available = status.map(permits) ?? true
        return assessment
    }

    static func name(_ permission: Permission) -> String {
        switch permission {
        case .notRead: return "not read"
        case .notStated: return "not stated before iOS 27.2"
        case .status(let status):
            switch status {
            case .notDetermined: return "notDetermined"
            case .restricted: return "restricted"
            case .denied: return "denied"
            case .authorized: return "authorized"
            @unknown default: return "unknown(\(status.rawValue))"
            }
        }
    }
}

/// Core Motion's device motion in the magnetic-north frame, for the magnetic
/// field's source (`MagneticFieldSource`): the calibrated field with its
/// accuracy, and the errors told apart. It runs on a queue of its own,
/// because stopping updates cancels every operation on the queue they were
/// given, and the raw accelerometer and gyroscope have theirs.
private final class CoreMotionFieldBackend: MagneticFieldBackend, @unchecked Sendable {
    private let manager: CMMotionManager
    private let queue: OperationQueue = {
        let queue = OperationQueue()
        queue.name = "andarium-motion-field"
        queue.maxConcurrentOperationCount = 1
        return queue
    }()

    init(manager: CMMotionManager) { self.manager = manager }

    func begin(interval: TimeInterval, events: @escaping @Sendable (MagneticFieldEvent) -> Void) {
        manager.deviceMotionUpdateInterval = interval
        manager.showsDeviceMovementDisplay = false
        manager.startDeviceMotionUpdates(using: .xMagneticNorthZVertical, to: queue) { data, error in
            if let error {
                let nsError = error as NSError
                events(
                    CompassSource.isCalibrationNotice(error)
                        ? .needsMovement : .failed(reason: "\(nsError.domain) \(nsError.code)"))
            }
            guard let data else { return }
            // The field is "with respect to the device": the guest's as it is.
            let calibrated = data.magneticField
            events(
                .reading(
                    x: calibrated.field.x, y: calibrated.field.y, z: calibrated.field.z,
                    status: MagneticField.status(accuracyRawValue: Int(calibrated.accuracy.rawValue))))
        }
    }

    /// A run in flight is retimed in place: Core Motion follows the property
    /// while it runs (checked on an iPad).
    func retime(interval: TimeInterval) { manager.deviceMotionUpdateInterval = interval }

    func end() { manager.stopDeviceMotionUpdates() }
}

/// The device's accelerometer, gyroscope and magnetic field. Core Motion needs
/// no usage string for raw readings, and none is added for the field: the
/// motion permission iOS 27.2 states is read, never requested
/// (`CompassSource.permits`).
///
/// The manager is the process's one and is never released: Core Motion asks
/// for one per application, and releasing a manager crashes inside Core
/// Motion, in work on its own threads (reproduced with the manager alone, in
/// a loop). A model built and released over again reaches it. A device
/// starts and stops its sensors on the manager and stops the ones it started
/// when it goes.
///
/// The magnetic field is `MagneticFieldSource` on `CoreMotionFieldBackend`;
/// the raw accelerometer and gyroscope keep their own handlers and intervals
/// beside it.
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
    private let field = MagneticFieldSource(
        backend: CoreMotionFieldBackend(manager: CoreMotionDevice.sharedManager),
        log: { note in
            switch note {
            case .needsMovement:
                // Not a failure: once, and the readings are waited for.
                #if !APP_STORE
                print("Andarium: compass: Core Motion asks the device to be moved to calibrate the field; "
                    + "the readings follow once it is")
                #endif
                break
            case .silent:
                print("Andarium: compass: device motion delivered nothing in 5 s; the magnetometer stops for this run")
            case .failed(let reason):
                print("Andarium: compass: device motion failed (\(reason)); the magnetometer stops for this run")
            }
        })
    private static let reported = OSAllocatedUnfairLock(initialState: false)

    deinit {
        for sensor in started.withLock({ $0 }) { stop(sensor) }
        field.stop()
    }

    var hasAccelerometer: Bool { manager.isAccelerometerAvailable }
    var hasGyroscope: Bool { manager.isGyroAvailable }

    var hasMagnetometer: Bool {
        let assessment = CompassSource.assess(
            magnetometer: manager.isMagnetometerAvailable, deviceMotion: manager.isDeviceMotionAvailable,
            northFrame: { CMMotionManager.availableAttitudeReferenceFrames().contains(.xMagneticNorthZVertical) },
            authorization: {
                if #available(iOS 27.2, *) { return CMMotionManager.authorizationStatus }
                return nil
            })
        #if !APP_STORE
        // Once, so that a tester's log says whether this iOS states a motion
        // permission and what it was.
        if Self.reported.withLock({ let first = !$0; $0 = true; return first }) {
            print("Andarium: compass: magnetic-north frame \(assessment.northFrame.map(String.init) ?? "not read"), "
                + "motion permission \(CompassSource.name(assessment.permission)): offered \(assessment.available)")
        }
        #endif
        return assessment.available
    }

    func start(
        _ sensor: MotionSample.Sensor, interval: TimeInterval,
        handler: @escaping @Sendable (Double, Double, Double) -> Void,
        failure: @escaping @Sendable () -> Void
    ) {
        // The field has its own start: it needs a status and its own failure rule.
        if sensor == .magnetometer { return }
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
        case .magnetometer:
            return
        }
    }

    func stop(_ sensor: MotionSample.Sensor) {
        started.withLock { _ = $0.remove(sensor) }
        switch sensor {
        case .accelerometer: manager.stopAccelerometerUpdates()
        case .gyroscope: manager.stopGyroUpdates()
        case .magnetometer: stopMagnetometer()
        }
    }

    func startMagnetometer(
        interval: TimeInterval,
        handler: @escaping @Sendable (Double, Double, Double, UInt8) -> Void,
        failure: @escaping @Sendable () -> Void
    ) {
        field.start(interval: interval, handler: handler, failure: failure)
    }

    func stopMagnetometer() { field.stop() }
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

    /// The guest is to be offered a magnetometer: the device has one that Core
    /// Motion can give calibrated, and no permission refuses it.
    var compassAvailable: Bool { device.hasMagnetometer }

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
