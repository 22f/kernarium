// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// iOS's camera permission as a test plays it: the access the system reports,
/// and the question it asks the person, answered when the test says.
@MainActor
final class FakeCamera: CameraAccessSource {
    var access: CameraAccess
    private(set) var asked = 0
    private var pending: [@MainActor @Sendable (CameraAccess) -> Void] = []

    init(_ access: CameraAccess = .notDetermined) { self.access = access }

    func request(_ answered: @escaping @MainActor @Sendable (CameraAccess) -> Void) {
        asked += 1
        pending.append(answered)
    }

    /// The person answers the question iOS is showing.
    func answer(_ access: CameraAccess) {
        self.access = access
        pending.removeFirst()(access)
    }
}

/// Camera asks iOS when it is turned on, shows itself off and unusable when iOS
/// refuses or the device has no camera, is read once when the guest starts
/// (live only while it is on and allowed), and keeps the person's choice
/// across launches. Nothing here opens a camera.
@MainActor
struct CameraFlowTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private struct Setup {
        let model: AppModel
        let driver: StubDriver
        let system: FakeCamera
    }

    private func setup(_ access: CameraAccess = .notDetermined, controller: Bool = true, chosen: Bool = false) -> Setup {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1"
        )
        var guest = Guest.fresh(on: host)
        guest.camera = chosen
        let model = AppModel(guest: guest, host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        // A test never opens the system's Settings.
        model.openSettings = {}
        let fake = FakeCamera(access)
        if controller { model.camera = CameraController(model: model, source: fake) }
        return Setup(model: model, driver: driver, system: fake)
    }

    // MARK: The switch

    @Test func itIsOffForANewGuestAndTheSessionIsBlocked() {
        let s = setup(.authorized)
        #expect(s.model.guest?.camera == false)
        #expect(!s.model.cameraShownOn)
        #expect(s.model.cameraMode == .blocked)
        #expect(s.system.asked == 0)
    }

    @Test func turningItOnAsksIOSAtOnceAndTheSessionIsLiveOnceItAllows() {
        let s = setup()
        s.model.setCamera(true)
        #expect(s.system.asked == 1)
        #expect(s.model.guest?.camera == true)
        #expect(s.model.cameraShownOn)
        // Until the person has answered the session is still blocked.
        #expect(s.model.cameraMode == .blocked)
        s.system.answer(.authorized)
        #expect(s.model.cameraAccess == .authorized)
        #expect(s.model.cameraShownOn && s.model.cameraMode == .live)
        #expect(s.model.permissionPrompt == nil)
        #expect(s.driver.savedGuest?.camera == true)
    }

    @Test func aRefusalTakesTheChoiceBackAndTheSessionStaysBlocked() {
        let s = setup()
        s.model.setCamera(true)
        s.system.answer(.denied)
        #expect(s.model.cameraAccess == .denied)
        #expect(s.model.guest?.camera == false)
        #expect(!s.model.cameraShownOn && s.model.cameraMode == .blocked)
        #expect(s.driver.savedGuest?.camera == false)
    }

    @Test func iOSIsAskedOnlyOnce() {
        let s = setup()
        s.model.setCamera(true)
        s.system.answer(.authorized)
        s.model.setCamera(false)
        s.model.setCamera(true)
        #expect(s.system.asked == 1)
        #expect(s.model.cameraMode == .live)
        s.model.setCamera(false)
        #expect(s.model.cameraMode == .blocked && s.driver.savedGuest?.camera == false)
    }

    @Test func aPreviewWithoutAControllerTakesTheAccessAsGiven() {
        let s = setup(controller: false)
        // The preview states a device with a camera iOS has not been asked about.
        s.model.cameraAccess = .notDetermined
        s.model.setCamera(true)
        #expect(s.model.cameraShownOn && s.model.cameraAccess == .authorized)
    }

    // MARK: Refused, restricted, absent

    @Test func afterARefusalTheSwitchExplainsAndOffersSettings() {
        let s = setup(.denied)
        var opened = 0
        s.model.openSettings = { opened += 1 }
        s.model.setCamera(true)
        #expect(s.model.permissionPrompt == .camera)
        #expect(s.model.guest?.camera == false)
        #expect(s.system.asked == 0)
        #expect(opened == 0)
        s.model.openCameraSettings()
        #expect(opened == 1 && s.model.cameraAwaitsSettings)
    }

    @Test func allowingItInSettingsTurnsTheSwitchTheyTappedOnWhenAndariumIsActive() {
        let s = setup(.denied)
        s.model.setCamera(true)
        s.model.openCameraSettings()
        #expect(!s.model.cameraShownOn)
        s.system.access = .authorized
        s.model.camera?.refresh()
        #expect(s.model.cameraShownOn && s.model.cameraMode == .live)
        #expect(s.driver.savedGuest?.camera == true)
        #expect(!s.model.cameraAwaitsSettings)
    }

    @Test func accessAnotherAppSettingTookAwayTurnsTheSwitchOffAndBlocksTheSession() {
        let s = setup(.authorized, chosen: true)
        #expect(s.model.cameraMode == .live)
        s.system.access = .denied
        s.model.camera?.refresh()
        #expect(!s.model.cameraShownOn && s.model.guest?.camera == false)
        #expect(s.model.cameraMode == .blocked)
    }

    @Test func aRestrictedCameraIsExplainedAndNeverOffersSettings() {
        let s = setup(.restricted)
        s.model.setCamera(true)
        #expect(s.model.permissionPrompt == .camera)
        let alert = CameraWords.alert(for: .restricted)
        #expect(alert?.offersSettings == false)
        #expect(CameraWords.alert(for: .denied)?.offersSettings == true)
        #expect(CameraWords.alert(for: .authorized) == nil && CameraWords.alert(for: .notDetermined) == nil)
        #expect(CameraWords.detail(for: .restricted) == "Restricted on this device")
        #expect(CameraWords.detail(for: .denied) == "Not allowed in Settings")
        #expect(CameraWords.detail(for: .authorized) == nil && CameraWords.detail(for: .unavailable) == nil)
    }

    @Test func aDeviceWithoutACameraCannotTurnItOn() {
        let s = setup(.unavailable, chosen: true)
        // A choice stored before is dropped once the device says it has none.
        s.model.camera?.refresh()
        #expect(s.model.guest?.camera == false)
        s.model.setCamera(true)
        #expect(s.model.guest?.camera == false && !s.model.cameraShownOn)
        #expect(s.model.permissionPrompt == nil && s.system.asked == 0)
        #expect(s.model.cameraMode == .blocked)
    }

    // MARK: Kept across launches

    @Test func theChoiceAndTheSessionModeSurviveARelaunch() throws {
        let first = setup(.notDetermined)
        first.model.setCamera(true)
        first.system.answer(.authorized)
        let stored = try #require(first.driver.savedGuest)
        let data = try JSONEncoder().encode(stored)

        // The next launch loads the guest and reads the access iOS keeps.
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1")
        let guest = try JSONDecoder().decode(Guest.self, from: data)
        let model = AppModel(guest: guest, host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        model.camera = CameraController(model: model, source: FakeCamera(.authorized))
        #expect(model.cameraShownOn && model.cameraMode == .live)

        // iOS no longer allows it at that launch: off, and blocked.
        let revoked = AppModel(guest: guest, host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let denied = FakeCamera(.authorized)
        revoked.camera = CameraController(model: revoked, source: denied)
        denied.access = .denied
        revoked.camera?.refresh()
        #expect(!revoked.cameraShownOn && revoked.cameraMode == .blocked && revoked.guest?.camera == false)
    }

    // MARK: The notice for other apps sharing the screen

    @Test func theNoticeFollowsTheMultitaskingInterruptionAlone() {
        let s = setup(.authorized, chosen: true)
        #expect(!s.model.cameraNoticeShowing)
        for capture in [CameraCapture.streaming, .interrupted, .interruptedBySystemPressure] {
            s.model.cameraCaptureChanged(capture)
            #expect(!s.model.cameraNoticeShowing)
        }
        s.model.cameraCaptureChanged(.interruptedByMultitasking)
        #expect(s.model.cameraNoticeShowing)
        s.model.cameraCaptureChanged(.streaming)
        #expect(!s.model.cameraNoticeShowing)
    }

    @Test func aDismissedNoticeStaysGoneUntilTheNextInterruption() {
        let s = setup(.authorized, chosen: true)
        s.model.cameraCaptureChanged(.interruptedByMultitasking)
        s.model.dismissCameraNotice()
        #expect(!s.model.cameraNoticeShowing)
        s.model.cameraCaptureChanged(.interruptedByMultitasking)
        #expect(!s.model.cameraNoticeShowing, "the same interruption does not raise it again")
        s.model.cameraCaptureChanged(.streaming)
        s.model.cameraCaptureChanged(.interruptedByMultitasking)
        #expect(s.model.cameraNoticeShowing, "a new interruption does")
    }

    @Test func theEndOfARunClearsTheNotice() {
        let s = setup(.authorized, chosen: true)
        s.model.cameraCaptureChanged(.interruptedByMultitasking)
        s.model.runEnded(RunEnd(result: 0), uptimeSeconds: 10)
        #expect(!s.model.cameraNoticeShowing && s.model.cameraCapture == .idle)
    }

    // MARK: The runtime's words

    @Test func theRuntimeIsToldTheModeInItsOwnWords() {
        #expect(LiveRuntime.cameraRuntimeMode(.blocked) == UInt32(KERNARIUM_RUNTIME_ANDROID_CAMERA_BLOCKED))
        #expect(LiveRuntime.cameraRuntimeMode(.live) == UInt32(KERNARIUM_RUNTIME_ANDROID_CAMERA_LIVE))
        #expect(LiveRuntime.cameraRuntimeMode(.blocked) != LiveRuntime.cameraRuntimeMode(.live))
    }

    @Test func theRuntimesStatesAreReadAsTheyAreSaid() {
        let said: [(Int, CameraCapture)] = [
            (KERNARIUM_RUNTIME_CAMERA_STATE_IDLE, .idle),
            (KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING, .streaming),
            (KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED, .interrupted),
            (KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_MULTITASKING, .interruptedByMultitasking),
            (KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_SYSTEM_PRESSURE, .interruptedBySystemPressure),
        ]
        for (state, capture) in said {
            #expect(LiveRuntime.cameraCapture(runtimeState: Int32(state)) == capture)
        }
        // A state this application does not know is an interruption without the notice.
        let unknown = LiveRuntime.cameraCapture(runtimeState: 99)
        #expect(unknown == .interrupted && !CameraNotice.isUp(for: unknown))
    }

    // MARK: The notes under Capabilities

    @Test func theFootnoteAppearsOnlyForCameraOnWithGyroscopeOffAndSensorsToShare() {
        let footnote = CameraWords.gyroscopeFootnote
        func notes(
            reason: String? = nil, access: CameraAccess = .authorized, camera: Bool, gyroscope: Bool, sensors: Bool = true
        ) -> [String] {
            CameraWords.notes(
                gyroscopeReason: reason, access: access, cameraOn: camera, gyroscopeOn: gyroscope, sensorsAvailable: sensors)
        }
        #expect(notes(camera: true, gyroscope: false) == [footnote])
        #expect(notes(camera: false, gyroscope: false).isEmpty)
        #expect(notes(camera: true, gyroscope: true).isEmpty)
        // The notes about the sensors come first and take its place.
        #expect(notes(reason: "No sensors.", camera: true, gyroscope: false, sensors: false) == ["No sensors."])
        #expect(notes(reason: "Problem.", camera: true, gyroscope: false) == ["Problem."])
        #expect(notes(camera: true, gyroscope: false, sensors: false).isEmpty)
        // A device without a camera says so beside whatever else it says.
        #expect(notes(access: .unavailable, camera: false, gyroscope: true) == [CameraWords.noCamera])
        #expect(notes(reason: "No sensors.", access: .unavailable, camera: false, gyroscope: false, sensors: false)
            == ["No sensors.", CameraWords.noCamera])
    }

    @Test func theWordsAreThoseOfTheDesign() {
        #expect(CameraWords.title == "Camera")
        #expect(CameraWords.noCamera == "This device has no camera to share.")
        #expect(CameraWords.noticeTitle == "Camera paused while other apps share the screen")
        #expect(CameraWords.gyroscopeFootnote == "Turn on Gyroscope so that photos taken sideways are saved the right way up.")
    }
}

/// The Capabilities rows: Camera follows Microphone.
@MainActor
struct CameraRowTests {
    @Test func cameraFollowsMicrophone() {
        let rows = CapabilitiesSection.rows(hapticsAvailable: true)
        #expect(rows == [.location, .microphone, .camera, .gyroscope, .haptics])
        #expect(CapabilitiesSection.rows(hapticsAvailable: false) == [.location, .microphone, .camera, .gyroscope])
    }
}
