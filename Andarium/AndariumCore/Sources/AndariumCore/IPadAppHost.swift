// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What runs the application when it is the iPad app on a Mac or on an Apple
/// Vision Pro, as the system and the hardware say it. Two features are not
/// offered on either (Add to Home Screen, the camera), and both ask here.
public struct IPadAppHost: Equatable, Sendable {
    public var isiOSAppOnMac: Bool
    public var isiOSAppOnVision: Bool

    /// The hardware name of an Apple Vision Pro starts with this, whatever the
    /// application running on it is.
    public static let visionMachinePrefix = "RealityDevice"

    public init(isiOSAppOnMac: Bool, isiOSAppOnVision: Bool) {
        self.isiOSAppOnMac = isiOSAppOnMac
        self.isiOSAppOnVision = isiOSAppOnVision
    }

    /// `visionAnswer` is what the system says about the Vision Pro, which it
    /// does from iOS 26.1 (`ProcessInfo.isiOSAppOnVision`) and not before: nil
    /// is a system that does not answer. The iPad app on a visionOS that
    /// reports an iOS below 26.1 is told apart by the hardware's name, so that
    /// the host the features exclude is the same one on every system the
    /// application runs on.
    public init(isiOSAppOnMac: Bool, visionAnswer: Bool?, machine: String) {
        self.init(
            isiOSAppOnMac: isiOSAppOnMac,
            isiOSAppOnVision: visionAnswer ?? machine.hasPrefix(Self.visionMachinePrefix))
    }

    /// The host has a Home Screen to add to: an iPhone or an iPad.
    public var hasHomeScreen: Bool { !isiOSAppOnMac && !isiOSAppOnVision }
}
