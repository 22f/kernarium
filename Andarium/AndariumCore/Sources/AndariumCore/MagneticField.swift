// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// How Core Motion's calibrated magnetic field becomes the reading the guest's
/// magnetometer reports (Android's `TYPE_MAGNETIC_FIELD`, microtesla, in the
/// device's own axes, calibrated, with a status).
///
/// The field comes only from device motion in the magnetic-north frame
/// (`CMDeviceMotion.magneticField`), never from the raw magnetometer, which
/// carries the device's own magnets, and never from a heading.
public enum MagneticField {
    /// The frame Core Motion expresses `magneticField` in.
    public enum Frame: Equatable, Sendable {
        /// The device's own axes, as the SDK header says ("with respect to the
        /// device"): the field is the guest's as it is.
        case device
        /// The attitude's reference frame (X magnetic north, Z up): the field is
        /// turned into the device's axes by the attitude.
        case reference
    }

    /// The one constant that decides which frame the field is in. The SDK
    /// header (`CMDeviceMotion.h`) states the device frame. The probe on an iPad
    /// could not confirm it by itself: the iPad lay with its y axis horizontal,
    /// so every candidate had a near-zero y component. A second pose, or the
    /// guest's own attitude check against Core Motion's, settles it; if it
    /// shows the reference frame, this is the only line that changes.
    public static let reportedFrame = Frame.device

    /// A rotation matrix as Core Motion gives it (`CMAttitude.rotationMatrix`):
    /// row by row, taking a vector of the reference frame to the device's
    /// axes (a device lying flat reads gravity along its own z, so the matrix
    /// applied to (0, 0, -1) is the gravity the device measures).
    public struct Rotation: Equatable, Sendable {
        public var m11, m12, m13: Double
        public var m21, m22, m23: Double
        public var m31, m32, m33: Double

        public init(
            m11: Double, m12: Double, m13: Double,
            m21: Double, m22: Double, m23: Double,
            m31: Double, m32: Double, m33: Double
        ) {
            self.m11 = m11
            self.m12 = m12
            self.m13 = m13
            self.m21 = m21
            self.m22 = m22
            self.m23 = m23
            self.m31 = m31
            self.m32 = m32
            self.m33 = m33
        }

        public static let identity = Rotation(m11: 1, m12: 0, m13: 0, m21: 0, m22: 1, m23: 0, m31: 0, m32: 0, m33: 1)

        func applied(_ x: Double, _ y: Double, _ z: Double) -> (Double, Double, Double) {
            (
                m11 * x + m12 * y + m13 * z,
                m21 * x + m22 * y + m23 * z,
                m31 * x + m32 * y + m33 * z
            )
        }
    }

    /// The field in the device's own axes, from the one Core Motion reported in `frame`.
    public static func inDeviceAxes(
        x: Double, y: Double, z: Double, attitude: Rotation, reportedIn frame: Frame = reportedFrame
    ) -> (x: Double, y: Double, z: Double) {
        switch frame {
        case .device: (x, y, z)
        case .reference: attitude.applied(x, y, z)
        }
    }

    /// Android's `SensorStatus` for Core Motion's calibration accuracy, whose
    /// raw values are -1 (uncalibrated), 0 (low), 1 (medium) and 2 (high):
    /// unreliable 0, low 1, medium 2, high 3. Anything else is clamped.
    public static func status(accuracyRawValue: Int) -> UInt8 {
        UInt8(min(max(accuracyRawValue + 1, 0), 3))
    }
}
