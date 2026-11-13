// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

// The Camera setting's saved choice, the session's mode, and the two
// statements the screens make about it (the notice and the rotation).
struct CameraTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    @Test func theChoiceIsOffForANewGuestAndForOneStoredBefore() throws {
        #expect(!Guest.fresh(on: host).camera)
        let before = Data(#"{"cores":2,"memoryMiB":1024,"capacityGiB":8,"microphone":true,"includeInBackups":false}"#.utf8)
        #expect(try JSONDecoder().decode(Guest.self, from: before).camera == false)
    }

    @Test func theChoiceIsKeptAsChosenThroughSaveAndErase() throws {
        var guest = Guest.fresh(on: host)
        guest.camera = true
        let saved = try JSONEncoder().encode(guest)
        #expect(try JSONDecoder().decode(Guest.self, from: saved).camera)
        #expect(try JSONDecoder().decode(Guest.self, from: JSONEncoder().encode(guest.erased)).camera)
        guest.camera = false
        #expect(try JSONDecoder().decode(Guest.self, from: JSONEncoder().encode(guest)).camera == false)
    }

    @Test func theChoiceDoesNotDisturbTheOthersAStoredGuestHas() throws {
        var guest = Guest.fresh(on: host)
        guest.microphone = true
        guest.location = true
        guest.haptics = false
        guest.camera = true
        let decoded = try JSONDecoder().decode(Guest.self, from: JSONEncoder().encode(guest))
        #expect(decoded.microphone && decoded.location && !decoded.haptics && decoded.camera)
    }

    @Test func aSessionIsLiveOnlyWhenTheSwitchIsOnAndIOSAllowsCapture() {
        let access: [CameraAccess] = [.unavailable, .notDetermined, .restricted, .denied, .authorized]
        for choice in [false, true] {
            for state in access {
                let expected: CameraMode = choice && state == .authorized ? .live : .blocked
                #expect(CameraSession.mode(choice: choice, access: state) == expected)
            }
        }
    }

    @Test func accessSaysWhatTheRowCanOffer() {
        let all: [CameraAccess] = [.unavailable, .notDetermined, .restricted, .denied, .authorized]
        #expect(all.filter(\.refuses) == [.restricted, .denied])
        #expect(all.filter(\.offered) == [.notDetermined, .restricted, .denied, .authorized])
    }

    @Test func onlyOtherAppsSharingTheScreenAndABlockedOpenRaiseANotice() {
        let all: [CameraCapture] = [
            .idle, .streaming, .interrupted, .interruptedByMultitasking, .interruptedBySystemPressure, .blockedInUse,
        ]
        #expect(all.compactMap { CameraNotice.raised(by: $0) } == [.pausedForOtherApps, .cameraOff])
        #expect(CameraNotice.raised(by: .interruptedByMultitasking) == .pausedForOtherApps)
        #expect(CameraNotice.raised(by: .blockedInUse) == .cameraOff)
    }

    @Test func theCameraOffNoticeFollowsTheOpenAndGoesWhenTheGuestClosesTheCamera() {
        var state = CameraNoticeState()
        #expect(state.showing(canTurnOn: true) == nil)
        state.captureChanged(.blockedInUse)
        #expect(state.showing(canTurnOn: true) == .cameraOff)
        state.captureChanged(.idle)
        #expect(state.showing(canTurnOn: true) == nil)
    }

    @Test func aDismissedCameraOffNoticeStaysGoneUntilTheGuestOpensTheCameraAgain() {
        var state = CameraNoticeState()
        state.captureChanged(.blockedInUse)
        state.dismiss()
        #expect(state.showing(canTurnOn: true) == nil)
        // The runtime says nothing more for the same open, and a repeat changes nothing.
        state.captureChanged(.blockedInUse)
        #expect(state.showing(canTurnOn: true) == nil, "the same open does not raise it again")
        state.captureChanged(.idle)
        #expect(state.showing(canTurnOn: true) == nil)
        state.captureChanged(.blockedInUse)
        #expect(state.showing(canTurnOn: true) == .cameraOff, "a new open does")
        // Dismissing with nothing up changes nothing for the next one.
        state.captureChanged(.idle)
        state.dismiss()
        state.captureChanged(.blockedInUse)
        #expect(state.showing(canTurnOn: true) == .cameraOff)
    }

    @Test func theCameraOffNoticeWaitsForARowThatCanBeTurnedOn() {
        var state = CameraNoticeState()
        state.captureChanged(.blockedInUse)
        #expect(state.showing(canTurnOn: false) == nil, "a device with no camera to share, or a switch already on, has nothing to tell")
        #expect(state.showing(canTurnOn: true) == .cameraOff)
        // The other notice does not depend on the row.
        state.captureChanged(.interruptedByMultitasking)
        #expect(state.showing(canTurnOn: false) == .pausedForOtherApps)
    }

    @Test func aDismissedNoticeOfOneKindDoesNotHideTheOtherKind() {
        var state = CameraNoticeState()
        state.captureChanged(.interruptedByMultitasking)
        state.dismiss()
        #expect(state.showing(canTurnOn: true) == nil)
        state.captureChanged(.blockedInUse)
        #expect(state.showing(canTurnOn: true) == .cameraOff)
        state.captureChanged(.streaming)
        state.captureChanged(.interruptedByMultitasking)
        #expect(state.showing(canTurnOn: true) == .pausedForOtherApps, "the end of a notice's state forgets its dismissal")
    }

    @Test func anInterruptionOtherThanSharingTheScreenKeepsTheStateButRaisesNothing() {
        var state = CameraNoticeState()
        for capture in [CameraCapture.streaming, .interrupted, .interruptedBySystemPressure] {
            state.captureChanged(capture)
            #expect(state.capture == capture && state.showing(canTurnOn: true) == nil)
        }
    }

    @Test func theRowOffersACameraByTheLayoutsRuleAndTheIPadAppOnAMacNone() {
        // Elsewhere: a built-in front camera, the rule the guest's layout follows.
        #expect(CameraAvailability.isAvailable(hasFrontCamera: true, isiOSAppOnMac: false))
        #expect(!CameraAvailability.isAvailable(hasFrontCamera: false, isiOSAppOnMac: false))
        // The iPad app on a Mac is offered none, whatever the Mac reports. The
        // decision is one constant.
        #expect(!CameraAvailability.offeredToIOSAppOnMac)
        #expect(!CameraAvailability.isAvailable(hasFrontCamera: true, isiOSAppOnMac: true))
        #expect(!CameraAvailability.isAvailable(hasFrontCamera: false, isiOSAppOnMac: true))
        // An unavailable camera is not offered, and the person has no question to answer.
        #expect(!CameraAccess.unavailable.offered && !CameraAccess.unavailable.refuses)
    }

    @Test func theGuestIsToldTheDeviceBodysTurnOnlyWhereTheHostOwnsTheRotation() {
        let axes: [MotionSample.Axes] = [.portrait, .landscapeLeft, .portraitUpsideDown, .landscapeRight]
        for interface in axes {
            #expect(CameraRotation.degrees(policy: .guest, interface: interface) == 0)
        }
        // The clockwise turn of the device from portrait: with the home button
        // on the left the device has turned a quarter clockwise.
        #expect(axes.map { CameraRotation.degrees(policy: .host, interface: $0) } == [0, 90, 180, 270])
        // The two landscape orientations are opposite turns.
        let left = CameraRotation.degrees(policy: .host, interface: .landscapeLeft)
        let right = CameraRotation.degrees(policy: .host, interface: .landscapeRight)
        #expect((left + right) % 360 == 0)
        #expect(axes.allSatisfy { CameraRotation.degrees(policy: .host, interface: $0) % 90 == 0 })
    }
}
