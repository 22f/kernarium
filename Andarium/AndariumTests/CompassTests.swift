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

    @Test func theGuestIsOfferedAMagnetometerOnlyWhereEverythingItNeedsIsThere() {
        func offered(
            _ magnetometer: Bool = true, _ deviceMotion: Bool = true, _ northFrame: Bool = true,
            _ authorization: CMAuthorizationStatus? = nil
        ) -> Bool {
            CompassSource.isAvailable(
                magnetometer: magnetometer, deviceMotion: deviceMotion, northFrame: northFrame,
                authorization: authorization)
        }
        #expect(offered())
        // The sensor, device motion and the magnetic-north frame are each needed.
        #expect(!offered(false))
        #expect(!offered(true, false))
        #expect(!offered(true, true, false))
        // Before iOS 27.2 there is no permission to ask about.
        #expect(offered(true, true, true, nil))
        // From 27.2: refused by the person or the system ends the offer; not yet
        // asked, or allowed, leaves it. Nothing is requested here either way.
        #expect(offered(true, true, true, .notDetermined))
        #expect(offered(true, true, true, .authorized))
        #expect(!offered(true, true, true, .denied))
        #expect(!offered(true, true, true, .restricted))
        // A refusal does not matter to a device that could not give a field anyway.
        #expect(!offered(false, true, true, .authorized))
        for status in [CMAuthorizationStatus.notDetermined, .restricted, .denied, .authorized] {
            #expect(CompassSource.permits(status) == (status == .notDetermined || status == .authorized))
        }
    }

    @Test func theStatusHasWordsForALog() {
        #expect(CompassSource.name(nil) == "not stated before iOS 27.2")
        #expect(CompassSource.name(.notDetermined) == "notDetermined")
        #expect(CompassSource.name(.restricted) == "restricted")
        #expect(CompassSource.name(.denied) == "denied")
        #expect(CompassSource.name(.authorized) == "authorized")
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
