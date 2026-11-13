// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AVFoundation
import Foundation

/// What iOS says about Andarium's cameras. Reading the access and looking for
/// cameras never shows a prompt, never opens a camera and never lights iOS's
/// camera indicator; only `request` can show iOS's question, and the person's
/// turning Camera on is what calls it.
struct SystemCameraSource: CameraAccessSource {
    /// Whether a device without a camera reports `unavailable`. The Simulator
    /// preview's system mode has none and still runs the permission
    /// conversation against the Simulator's own, so it sets this off.
    let requiresCamera: Bool

    init(requiresCamera: Bool = true) {
        self.requiresCamera = requiresCamera
    }

    var access: CameraAccess {
        if requiresCamera, !Self.hasCamera { return .unavailable }
        return Self.access(of: AVCaptureDevice.authorizationStatus(for: .video))
    }

    func request(_ answered: @escaping @MainActor @Sendable (CameraAccess) -> Void) {
        AVCaptureDevice.requestAccess(for: .video) { granted in
            Task { @MainActor in answered(granted ? .authorized : .denied) }
        }
    }

    /// The device has a camera the guest could be given: the same rule the
    /// guest's layout follows (a built-in front camera), and none for the iPad
    /// app on a Mac (`CameraAvailability`).
    static var hasCamera: Bool {
        CameraAvailability.isAvailable(
            hasFrontCamera: hasFrontCamera, isiOSAppOnMac: ProcessInfo.processInfo.isiOSAppOnMac)
    }

    /// A built-in camera faces the person: among the cameras the runtime
    /// discovers, one in the front position. An external camera has none. The
    /// device types are those of `discover` in
    /// darwin/platform/apple/camera_capture.mm, the foldable's two included.
    static var hasFrontCamera: Bool {
        var types: [AVCaptureDevice.DeviceType] = [
            .builtInWideAngleCamera, .builtInUltraWideCamera, .builtInTelephotoCamera,
            .builtInDualCamera, .builtInDualWideCamera, .builtInTripleCamera,
        ]
        if #available(iOS 27.1, *) {
            types += [.builtInOuterUltraWideCamera, .builtInInnerUltraWideCamera]
        }
        return !AVCaptureDevice.DiscoverySession(deviceTypes: types, mediaType: .video, position: .front)
            .devices.isEmpty
    }

    static func access(of status: AVAuthorizationStatus) -> CameraAccess {
        switch status {
        case .notDetermined: .notDetermined
        case .restricted: .restricted
        case .denied: .denied
        case .authorized: .authorized
        @unknown default: .denied
        }
    }
}
