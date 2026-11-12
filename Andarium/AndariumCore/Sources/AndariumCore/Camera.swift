// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// How far the system lets Andarium use this device's cameras, in the terms the
/// Camera row and the start need. The application reads it from iOS; reading it
/// never shows a prompt.
public enum CameraAccess: Equatable, Sendable {
    /// The device has no camera to share (the Simulator, a Mac without one).
    case unavailable
    /// iOS has not asked the person yet.
    case notDetermined
    /// Something other than the person forbids it (Screen Time, a profile).
    case restricted
    /// The person refused it, or took it away in Settings.
    case denied
    /// Andarium may capture.
    case authorized

    /// Nothing can be captured and the person has no question left to answer.
    public var refuses: Bool { self == .restricted || self == .denied }

    /// The person may turn Camera on: there is a camera, and iOS has not
    /// refused it.
    public var offered: Bool { self != .unavailable }
}

/// Whether the device has a camera to share. The guest is given cameras when
/// the host has a built-in front camera (the layout's rule), so the Camera row
/// offers the same. The iPad app running on a Mac is offered none: the system
/// reports the Mac's cameras (and a Continuity iPhone's) without a position, the
/// layout does not count them, and a hidden window would keep one open.
public enum CameraAvailability {
    /// Whether the iPad app on a Mac is offered the Mac's camera. This is the
    /// one place to turn it on once the Mac's cameras are checked.
    public static let offeredToIOSAppOnMac = false

    public static func isAvailable(hasFrontCamera: Bool, isiOSAppOnMac: Bool) -> Bool {
        if isiOSAppOnMac, !offeredToIOSAppOnMac { return false }
        return hasFrontCamera
    }
}

/// What a session's cameras do, fixed when the guest starts: the guest lists
/// the same cameras either way.
public enum CameraMode: Equatable, Sendable {
    /// Every frame is black, and the host opens no capture session, so iOS's
    /// camera indicator cannot light.
    case blocked
    /// A guest app that opens a camera sees the host's.
    case live
}

/// The session's camera mode: live only while Camera is on and iOS lets
/// Andarium capture at the moment the guest starts. Off, access that iOS
/// refuses or has not been asked for, and a device without cameras are all
/// blocked.
public enum CameraSession {
    public static func mode(choice: Bool, access: CameraAccess) -> CameraMode {
        choice && access == .authorized ? .live : .blocked
    }
}

/// What the host is doing for the guest's cameras, as the runtime reports it.
public enum CameraCapture: Equatable, Sendable {
    /// No guest app has a camera open.
    case idle
    case streaming
    /// The capture cannot run for a reason that ends by itself: another app or
    /// a call has the camera.
    case interrupted
    /// The same, because other apps share the screen (iPad Split View, Slide
    /// Over).
    case interruptedByMultitasking
    /// The same, because the device is hot.
    case interruptedBySystemPressure
}

/// The notice an iPad shows while other apps share the screen: the camera
/// pauses then, and the guest gets black frames until they no longer do. Only
/// that interruption raises it.
public enum CameraNotice {
    public static func isUp(for capture: CameraCapture) -> Bool {
        capture == .interruptedByMultitasking
    }
}

/// The note under Capabilities for photos taken sideways: apps in the guest
/// turn a photo upright from the accelerometer, so with Gyroscope off a
/// landscape photo is saved tagged portrait. It shows only while Camera is on,
/// Gyroscope is off and the device has the sensors to share; a note about the
/// sensors themselves takes its place.
public enum CameraFootnote {
    public static func isUp(cameraOn: Bool, gyroscopeOn: Bool, sensorsAvailable: Bool) -> Bool {
        cameraOn && !gyroscopeOn && sensorsAvailable
    }
}

/// The turn of the device body the guest is told, for its cameras. Where the
/// host owns the guest's rotation (an iPad window) the guest stays upright in
/// the window while the device, which carries every built-in sensor, turns under
/// it. The runtime derives each camera's SENSOR_ORIENTATION from this turn when
/// the guest starts (a back camera adds it, a front camera subtracts it) and
/// turns the picture by the change since, so the guest's photos and previews
/// come out upright. Where the guest owns the rotation it compensates itself
/// and the turn is zero.
public enum CameraRotation {
    /// The device body's clockwise turn from portrait, as its user sees it:
    /// 0, 90, 180 or 270.
    public static func degrees(policy: GuestRotationPolicy, interface: MotionSample.Axes) -> UInt32 {
        guard policy == .host else { return 0 }
        switch interface {
        case .portrait: return 0
        // The window's home button is on the left: the device is turned
        // clockwise a quarter.
        case .landscapeLeft: return 90
        case .portraitUpsideDown: return 180
        case .landscapeRight: return 270
        }
    }
}
