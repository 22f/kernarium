// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// What the system says about Andarium's use of the device's cameras, and the
/// one call that can ask. The device application reads iOS; a test gives its
/// own.
@MainActor
protocol CameraAccessSource {
    /// The access now: `unavailable` when the device has no camera to share.
    /// Reading it never shows a prompt and never uses a camera.
    var access: CameraAccess { get }
    /// Asks the person, which iOS does only while they have not answered
    /// before; otherwise it answers with what they said.
    func request(_ answered: @escaping @MainActor @Sendable (CameraAccess) -> Void)
}

/// The Camera setting's conversation with the system, for the device
/// application and the Simulator preview alike: turning it on asks iOS at once,
/// the first time; an answer, or access the person gave or took away in
/// Settings, goes to the model, which keeps the switch. Unlike the microphone,
/// nothing tells the guest: the camera is read once, when the guest starts
/// (`AppModel.cameraMode`).
@MainActor
final class CameraController {
    private weak var model: AppModel?
    private let source: any CameraAccessSource

    init(model: AppModel, source: any CameraAccessSource) {
        self.model = model
        self.source = source
        model.cameraAccess = source.access
    }

    /// The switch changed, and the model has stored the choice.
    func setOn(_ on: Bool) {
        let access = source.access
        model?.cameraAccessChanged(access)
        guard on, access == .notDetermined else { return }
        source.request { [weak self] answer in self?.model?.cameraAccessChanged(answer) }
    }

    /// Looks at the permission again: at launch, whenever Andarium becomes
    /// active (the person may have changed it in Settings) and as a start
    /// reads it.
    func refresh() {
        model?.cameraAccessChanged(source.access)
    }
}

/// The words of the Camera setting: the row's detail line, its alerts, the
/// notes under Capabilities and the notice in a session. They are one place so
/// the text can change without touching the logic.
enum CameraWords {
    static let title = String(localized: "Camera")
    static let denied = String(localized: "Not allowed in Settings")
    static let restricted = String(localized: "Restricted on this device")
    static let noCamera = String(localized: "This device has no camera to share.")
    static let gyroscopeFootnote = String(
        localized: "Turn on Gyroscope so that photos taken sideways are saved the right way up.")
    static let noticeTitle = String(localized: "Camera paused while other apps are on screen")

    /// The line under Camera: why it cannot be used; none otherwise.
    static func detail(for access: CameraAccess) -> String? {
        switch access {
        case .denied: denied
        case .restricted: restricted
        case .unavailable, .notDetermined, .authorized: nil
        }
    }

    /// What VoiceOver adds to the row.
    static func hint(for access: CameraAccess) -> String {
        switch access {
        case .denied:
            String(localized: "Andarium is not allowed to use the camera. Turning this on offers to open Settings.")
        case .restricted:
            String(localized: "The camera is restricted on this device. Turning this on explains why.")
        case .unavailable, .notDetermined, .authorized:
            ""
        }
    }

    /// An alert about the camera: its title and message, and whether Settings
    /// has the way out.
    struct Alert: Equatable {
        var title: String
        var message: String
        var offersSettings: Bool
    }

    /// The alert for turning Camera on while iOS refuses it; nil for any other
    /// access.
    static func alert(for access: CameraAccess) -> Alert? {
        switch access {
        case .denied:
            Alert(
                title: String(localized: "Allow camera access"),
                message: String(localized: "iOS is not allowing Andarium to use the camera. Turn on Camera for Andarium in Settings, then come back to use it."),
                offersSettings: true)
        case .restricted:
            Alert(
                title: String(localized: "Camera is restricted"),
                message: String(localized: "This device\u{2019}s settings, such as Screen Time or a profile, do not let apps use the camera, so Andarium cannot share it."),
                offersSettings: false)
        case .unavailable, .notDetermined, .authorized:
            nil
        }
    }

    /// The notes under Capabilities, top to bottom. A note about the sensors
    /// themselves (`gyroscopeReason`) takes the place of the footnote about
    /// photos taken sideways, which needs them; the note that the device has
    /// no camera stands beside either.
    static func notes(
        gyroscopeReason: String?, access: CameraAccess, cameraOn: Bool, gyroscopeOn: Bool, sensorsAvailable: Bool
    ) -> [String] {
        var notes: [String] = []
        if let gyroscopeReason {
            notes.append(gyroscopeReason)
        } else if CameraFootnote.isUp(cameraOn: cameraOn, gyroscopeOn: gyroscopeOn, sensorsAvailable: sensorsAvailable) {
            notes.append(gyroscopeFootnote)
        }
        if access == .unavailable { notes.append(noCamera) }
        return notes
    }
}
