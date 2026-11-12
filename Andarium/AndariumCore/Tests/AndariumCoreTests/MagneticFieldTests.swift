// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// How Core Motion's field becomes the guest's: which frame it is in (one
/// constant decides it, and each candidate is checked), and the status.
struct MagneticFieldTests {
    /// The attitude of an iPad lying on a stand, from the device probe: a
    /// rotation about the device's y axis of about 22 degrees (rotation matrix
    /// of `CMAttitude`, row by row), with the calibrated field it reported.
    private let stand = MagneticField.Rotation(
        m11: 0.928, m12: -0.008, m13: 0.372,
        m21: 0.009, m22: 1.000, m23: -0.002,
        m31: -0.372, m32: 0.006, m33: 0.928)
    private let probeField = (x: 52.68, y: 0.62, z: -44.58)

    private func expectClose(
        _ actual: (x: Double, y: Double, z: Double), _ expected: (Double, Double, Double), accuracy: Double = 0.02
    ) {
        #expect(abs(actual.x - expected.0) <= accuracy, "x \(actual.x) vs \(expected.0)")
        #expect(abs(actual.y - expected.1) <= accuracy, "y \(actual.y) vs \(expected.1)")
        #expect(abs(actual.z - expected.2) <= accuracy, "z \(actual.z) vs \(expected.2)")
    }

    @Test func theSdkHeadersFrameIsTheOneTheConstantStates() {
        // CMDeviceMotion.h: the field is "with respect to the device". If a
        // second pose or the attitude check says otherwise, this constant and
        // this test change together, and nothing else.
        #expect(MagneticField.reportedFrame == .device)
    }

    @Test func aFieldInTheDeviceFrameIsTheGuestsAsItIs() {
        let guest = MagneticField.inDeviceAxes(
            x: probeField.x, y: probeField.y, z: probeField.z, attitude: stand, reportedIn: .device)
        #expect(guest.x == probeField.x && guest.y == probeField.y && guest.z == probeField.z)
        // The attitude does not matter.
        let again = MagneticField.inDeviceAxes(x: 1, y: 2, z: 3, attitude: .identity, reportedIn: .device)
        #expect(again.x == 1 && again.y == 2 && again.z == 3)
    }

    @Test func aFieldInTheReferenceFrameIsTurnedIntoTheDevicesAxesByTheAttitude() {
        // The matrix takes the reference frame to the device: the gravity of a
        // device on the stand, (0, 0, -1) in the reference frame, is what the
        // device measured on it (0.35 along -x, none along y, 0.94 along -z).
        let gravity = MagneticField.inDeviceAxes(x: 0, y: 0, z: -1, attitude: stand, reportedIn: .reference)
        expectClose(gravity, (-0.372, 0.002, -0.928), accuracy: 0.001)
        // The probe's field taken as a reference-frame one: R times B.
        let field = MagneticField.inDeviceAxes(
            x: probeField.x, y: probeField.y, z: probeField.z, attitude: stand, reportedIn: .reference)
        expectClose(field, (32.34, 1.22, -60.95), accuracy: 0.05)
        // The identity attitude changes nothing.
        let same = MagneticField.inDeviceAxes(x: 1, y: 2, z: 3, attitude: .identity, reportedIn: .reference)
        #expect(same.x == 1 && same.y == 2 && same.z == 3)
        // A device turned a quarter about z: the field north of it is along its -y.
        let quarter = MagneticField.Rotation(m11: 0, m12: 1, m13: 0, m21: -1, m22: 0, m23: 0, m31: 0, m32: 0, m33: 1)
        let turned = MagneticField.inDeviceAxes(x: 30, y: 0, z: -40, attitude: quarter, reportedIn: .reference)
        expectClose(turned, (0, -30, -40), accuracy: 0.0001)
    }

    @Test func theMagnitudeIsKeptByEitherCandidate() {
        func norm(_ v: (x: Double, y: Double, z: Double)) -> Double { (v.x * v.x + v.y * v.y + v.z * v.z).squareRoot() }
        let reference = MagneticField.inDeviceAxes(
            x: probeField.x, y: probeField.y, z: probeField.z, attitude: stand, reportedIn: .reference)
        let device = MagneticField.inDeviceAxes(
            x: probeField.x, y: probeField.y, z: probeField.z, attitude: stand, reportedIn: .device)
        #expect(abs(norm(reference) - norm(device)) < 0.2)
    }

    @Test func calibrationAccuracyBecomesAndroidsStatusPlusOne() {
        // Core Motion: uncalibrated -1, low 0, medium 1, high 2.
        // Android: unreliable 0, low 1, medium 2, high 3.
        #expect(MagneticField.status(accuracyRawValue: -1) == 0)
        #expect(MagneticField.status(accuracyRawValue: 0) == 1)
        #expect(MagneticField.status(accuracyRawValue: 1) == 2)
        #expect(MagneticField.status(accuracyRawValue: 2) == 3)
        // A value the SDK does not have is brought into the range.
        #expect(MagneticField.status(accuracyRawValue: -7) == 0)
        #expect(MagneticField.status(accuracyRawValue: 3) == 3)
        #expect(MagneticField.status(accuracyRawValue: Int.max - 1) == 3)
    }
}
