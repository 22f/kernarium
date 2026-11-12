// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

// The Camera setting's saved choice, the session's mode, and the three
// statements the screens make about it (the notice, the footnote and the
// rotation).
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

    @Test func onlyOtherAppsSharingTheScreenRaisesTheNotice() {
        let all: [CameraCapture] = [.idle, .streaming, .interrupted, .interruptedByMultitasking, .interruptedBySystemPressure]
        #expect(all.filter { CameraNotice.isUp(for: $0) } == [.interruptedByMultitasking])
    }

    @Test func theFootnoteShowsOnlyForCameraOnWithGyroscopeOffOnADeviceWithSensors() {
        for camera in [false, true] {
            for gyroscope in [false, true] {
                for sensors in [false, true] {
                    let shown = CameraFootnote.isUp(cameraOn: camera, gyroscopeOn: gyroscope, sensorsAvailable: sensors)
                    #expect(shown == (camera && !gyroscope && sensors))
                }
            }
        }
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
