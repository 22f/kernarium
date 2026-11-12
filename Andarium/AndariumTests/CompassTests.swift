// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import CoreMotion
import Foundation
import Testing
@testable import Andarium

/// What the application does about the magnetic field where the hardware, or the
/// person's motion permission, may be missing: the boot request states the
/// compass only for a device that can give a field, the gate that says so, and
/// what Core Motion's handler may call a failure. The field's path through the
/// session is `MotionCompassTests` (AndariumCore); the readings of a real
/// device are checked on one.
struct CompassHostTests {
    @Test func theBootRequestStatesTheCompassOnlyForADeviceThatCanGiveAField() {
        let haptics = UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS)
        let orientation = UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_ORIENTATION)
        let compass = UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_COMPASS)
        #expect(compass == 4)
        // A request that does not say the compass is today's.
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: false) == 0)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: false, followsDeviceOrientation: true) == orientation)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: true, followsDeviceOrientation: true) == haptics | orientation)
        // Each of the three alone, and every combination.
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: false, compassAvailable: true) == compass)
        for h in [false, true] {
            for o in [false, true] {
                for c in [false, true] {
                    let expected = (h ? haptics : 0) | (o ? orientation : 0) | (c ? compass : 0)
                    #expect(
                        LiveRuntime.hostFeatures(hapticsAvailable: h, followsDeviceOrientation: o, compassAvailable: c)
                            == expected, "haptics \(h), orientation \(o), compass \(c)")
                }
            }
        }
    }

    /// Counts what an assessment reads, in order.
    private final class Reads: @unchecked Sendable {
        var order: [String] = []
    }

    private func assessed(
        _ magnetometer: Bool = true, _ deviceMotion: Bool = true, _ northFrame: Bool = true,
        _ authorization: CMAuthorizationStatus? = nil, reads: Reads = Reads()
    ) -> CompassSource.Assessment {
        CompassSource.assess(
            magnetometer: magnetometer, deviceMotion: deviceMotion,
            northFrame: {
                reads.order.append("frames")
                return northFrame
            },
            authorization: {
                reads.order.append("permission")
                return authorization
            })
    }

    @Test func theGuestIsOfferedAMagnetometerOnlyWhereEverythingItNeedsIsThere() {
        #expect(assessed().available)
        // The sensor, device motion and the magnetic-north frame are each needed.
        #expect(!assessed(false).available)
        #expect(!assessed(true, false).available)
        #expect(!assessed(true, true, false).available)
        // Before iOS 27.2 there is no permission to ask about.
        #expect(assessed(true, true, true, nil).available)
        #expect(assessed().permission == .notStated)
        // From 27.2: refused by the person or the system ends the offer; not yet
        // asked, or allowed, leaves it. Nothing is requested here either way.
        #expect(assessed(true, true, true, .notDetermined).available)
        #expect(assessed(true, true, true, .authorized).available)
        #expect(!assessed(true, true, true, .denied).available)
        #expect(!assessed(true, true, true, .restricted).available)
        #expect(assessed(true, true, true, .denied).permission == .status(.denied))
        // A refusal does not matter to a device that could not give a field anyway.
        #expect(!assessed(false, true, true, .authorized).available)
        for status in [CMAuthorizationStatus.notDetermined, .restricted, .denied, .authorized] {
            #expect(CompassSource.permits(status) == (status == .notDetermined || status == .authorized))
        }
    }

    @Test func aDeviceThatCannotHaveAFieldIsNotAskedAboutTheFramesOrThePermission() {
        // iOS on a Mac does not state either (the permission status is marked
        // unavailable on macOS): the question ends at the first two answers.
        for (magnetometer, deviceMotion) in [(false, false), (false, true), (true, false)] {
            let reads = Reads()
            let assessment = assessed(magnetometer, deviceMotion, reads: reads)
            #expect(reads.order.isEmpty, "magnetometer \(magnetometer), device motion \(deviceMotion)")
            #expect(assessment == CompassSource.Assessment())
            #expect(assessment.permission == .notRead && assessment.northFrame == nil)
        }
    }

    @Test func thePermissionIsReadOnlyAfterTheFramesAndOnlyIfTheyAreThere() {
        let reads = Reads()
        _ = assessed(true, true, true, .authorized, reads: reads)
        #expect(reads.order == ["frames", "permission"])
        let withoutFrame = Reads()
        let assessment = assessed(true, true, false, .authorized, reads: withoutFrame)
        #expect(withoutFrame.order == ["frames"])
        #expect(!assessment.available && assessment.northFrame == false && assessment.permission == .notRead)
    }

    @Test func theStatusHasWordsForALog() {
        #expect(CompassSource.name(.notRead) == "not read")
        #expect(CompassSource.name(.notStated) == "not stated before iOS 27.2")
        #expect(CompassSource.name(.status(.notDetermined)) == "notDetermined")
        #expect(CompassSource.name(.status(.restricted)) == "restricted")
        #expect(CompassSource.name(.status(.denied)) == "denied")
        #expect(CompassSource.name(.status(.authorized)) == "authorized")
    }

    @Test func onlyTheMovementNoticeIsNotAFailure() {
        // Core Motion's "the device must move to calibrate" (101), once, in the handler.
        let notice = NSError(domain: CMErrorDomain, code: Int(CMErrorDeviceRequiresMovement.rawValue))
        #expect(notice.code == 101)
        #expect(CompassSource.isCalibrationNotice(notice))
        // Every other code of Core Motion, and the same number from another domain, is one.
        for code in [CMErrorNULL, CMErrorTrueNorthNotAvailable, CMErrorUnknown, CMErrorNotAvailable,
                     CMErrorNotAuthorized, CMErrorNilData] {
            #expect(!CompassSource.isCalibrationNotice(NSError(domain: CMErrorDomain, code: Int(code.rawValue))), "\(code)")
        }
        #expect(!CompassSource.isCalibrationNotice(NSError(domain: NSPOSIXErrorDomain, code: 101)))
        #expect(!CompassSource.isCalibrationNotice(CancellationError()))
    }

    /// A device that has a field, or not, and nothing else.
    private final class FieldOnly: MotionDevice, @unchecked Sendable {
        let hasAccelerometer = false
        let hasGyroscope = false
        let hasMagnetometer: Bool
        init(_ hasMagnetometer: Bool) { self.hasMagnetometer = hasMagnetometer }
        func start(
            _ sensor: MotionSample.Sensor, interval: TimeInterval,
            handler: @escaping @Sendable (Double, Double, Double) -> Void, failure: @escaping @Sendable () -> Void
        ) {}
        func stop(_ sensor: MotionSample.Sensor) {}
    }

    private struct NoGuest: GuestStreamOpener {
        func openStream() throws -> Int32 { throw POSIXError(.ECONNREFUSED) }
    }

    @Test func theApplicationOffersTheCompassWhereItsDeviceHasAField() {
        #expect(MotionSensors(opener: NoGuest(), device: FieldOnly(true)).compassAvailable)
        #expect(!MotionSensors(opener: NoGuest(), device: FieldOnly(false)).compassAvailable)
    }

    /// The Simulator and an iPad app on a Mac have no magnetometer: no bit, so no
    /// sensor the guest could list and nothing feeds.
    @Test func theSimulatorAndAMacAreNotOfferedACompass() {
        #if targetEnvironment(simulator)
        #expect(!MotionSensors().compassAvailable, "The Simulator has no magnetometer")
        #else
        if ProcessInfo.processInfo.isiOSAppOnMac { #expect(!MotionSensors().compassAvailable, "A Mac has no magnetometer") }
        #endif
    }
}
