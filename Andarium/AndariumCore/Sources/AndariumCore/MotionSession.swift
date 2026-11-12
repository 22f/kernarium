// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The device's motion sensors as a session drives them. Each sensor is
/// started at an interval or stopped; `handler` is called on a queue of the
/// device's own with the reading in the device's own units, and `failure`
/// when the device can no longer be read.
public protocol MotionDevice: AnyObject, Sendable {
    var hasAccelerometer: Bool { get }
    var hasGyroscope: Bool { get }
    /// Starts the sensor, or restarts it at the new interval if it runs.
    func start(
        _ sensor: MotionSample.Sensor, interval: TimeInterval,
        handler: @escaping @Sendable (_ x: Double, _ y: Double, _ z: Double) -> Void,
        failure: @escaping @Sendable () -> Void
    )
    func stop(_ sensor: MotionSample.Sensor)
}

/// Shares the device's sensors with the guest while the guest is in front,
/// wherever the device has them, and reads each sensor only while the guest
/// says an application uses it (`MotionSampling`). The axes are set when a run
/// begins and never change per sample; a new mounting is a new run
/// (`MotionMount` decides when). No samples are retained across a run.
///
/// Where the guest follows the device's orientation (an iPhone), the same
/// connection also gives a guest that has the device-orientation sensor the
/// orientation the device reports (`setOrientation`), on a device without
/// sensors too. It is sent when it changes and, once, on each connection and
/// each time the guest enables the sensor; nothing runs for it between changes.
///
/// Every call returns at once; the work is in order on one queue, and the
/// device and the stream are only touched there.
public final class MotionSession: @unchecked Sendable {
    private let opener: any GuestStreamOpener
    private let device: any MotionDevice
    private let retryDelay: DispatchTimeInterval
    private let queue = DispatchQueue(label: "andarium-motion-session")

    // Touched only on `queue`.
    private var sampling = MotionSampling()
    private var axes = MotionSample.Axes.portrait
    private var stream: MotionStream?
    private var streamEpoch: UInt64 = 0
    private var run: UInt64 = 0
    private var applied = MotionPlan.off

    public init(
        opener: any GuestStreamOpener, device: any MotionDevice,
        retryDelay: DispatchTimeInterval = .seconds(1)
    ) {
        self.opener = opener
        self.device = device
        self.retryDelay = retryDelay
    }

    deinit { stream?.stop() }

    /// What is wanted now. A run begins when the device has sensors, or the
    /// guest follows the device's orientation, and the guest is in front. It
    /// ends when either stops being true or the axes change.
    public func update(foreground: Bool, axes: MotionSample.Axes, followsOrientation: Bool = false) {
        queue.async { [self] in
            sampling.hasSensors = device.hasAccelerometer || device.hasGyroscope
            sampling.followsOrientation = followsOrientation
            if !followsOrientation { sampling.orientation = nil }
            sampling.foreground = foreground
            if axes != self.axes, stream != nil { endRun() }
            self.axes = axes
            if sampling.wantsConnection {
                if stream == nil { beginRun() }
                reconcile()
            } else if stream != nil {
                endRun()
            }
        }
    }

    /// The device's orientation now, or nil once it is not being reported (the
    /// next report is then the first of a new period). The guest is given it
    /// at once if it takes orientations and has not had this one.
    public func setOrientation(_ orientation: GuestOrientation?) {
        queue.async { [self] in
            sampling.orientation = orientation
            if stream != nil { reconcile() }
        }
    }

    /// Ends the run now: the pending samples belong to a period that is over.
    /// The next `update` begins a new one.
    public func restart() {
        queue.async { [self] in
            if stream != nil { endRun() }
        }
    }

    /// Returns when everything asked of the session so far has been done.
    public func settle() {
        queue.sync {}
    }

    /// What the guest's service last said on the connection; nil until it
    /// says anything (an older image never does).
    public var demand: MotionDemand? { queue.sync { sampling.demand } }

    /// How the device's sensors are being read now.
    public var plan: MotionPlan { queue.sync { applied } }

    // MARK: A run

    private func beginRun() {
        run &+= 1
        let current = run
        sampling.connectionChanged(false)
        applied = .off
        let stream = MotionStream(
            opener: opener, retryDelay: retryDelay,
            connectionChanged: { [weak self] connected in
                self?.queue.async { [weak self] in
                    guard let self, run == current else { return }
                    sampling.connectionChanged(connected)
                    reconcile()
                }
            },
            demandChanged: { [weak self] demand in
                self?.queue.async { [weak self] in
                    guard let self, run == current else { return }
                    sampling.received(demand)
                    reconcile()
                }
            }
        )
        self.stream = stream
        streamEpoch = stream.start()
    }

    private func endRun() {
        run &+= 1
        for sensor in [MotionSample.Sensor.accelerometer, .gyroscope] where applied.interval(of: sensor) != nil {
            device.stop(sensor)
        }
        applied = .off
        stream?.stop()
        stream = nil
        sampling.connectionChanged(false)
    }

    /// Starts, stops and retimes the device's sensors to match the plan.
    private func reconcile() {
        guard let stream else { return }
        let plan = sampling.plan
        let current = run
        let epoch = streamEpoch
        let axes = axes
        for sensor in [MotionSample.Sensor.accelerometer, .gyroscope] {
            let wanted = hasSensor(sensor) ? plan.interval(of: sensor) : nil
            guard wanted != applied.interval(of: sensor) else { continue }
            if let wanted {
                device.start(sensor, interval: wanted, handler: { x, y, z in
                    stream.submit(MotionSample(sensor: sensor, x: x, y: y, z: z, axes: axes), epoch: epoch)
                }, failure: { [weak self] in
                    self?.queue.async { [weak self] in
                        guard let self, run == current else { return }
                        endRun()
                    }
                })
            } else {
                device.stop(sensor)
            }
            switch sensor {
            case .accelerometer: applied.accelerometer = wanted
            case .gyroscope: applied.gyroscope = wanted
            }
        }
        if let orientation = sampling.orientationToSend() {
            stream.submit(orientation, epoch: epoch)
        }
    }

    private func hasSensor(_ sensor: MotionSample.Sensor) -> Bool {
        switch sensor {
        case .accelerometer: device.hasAccelerometer
        case .gyroscope: device.hasGyroscope
        }
    }
}
