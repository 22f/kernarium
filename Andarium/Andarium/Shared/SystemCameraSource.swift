// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AVFoundation

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

    /// The device has a camera the guest could be given: a built-in one or an
    /// external one an iPad takes.
    static var hasCamera: Bool {
        let types: [AVCaptureDevice.DeviceType] = [
            .builtInWideAngleCamera, .builtInUltraWideCamera, .builtInTelephotoCamera,
            .builtInDualCamera, .builtInDualWideCamera, .builtInTripleCamera, .builtInTrueDepthCamera,
            .external,
        ]
        return !AVCaptureDevice.DiscoverySession(deviceTypes: types, mediaType: .video, position: .unspecified)
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
