// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The preview's states of the root switch: Home with the row off, on and
/// paused, the warning that asks before it turns on, and the screens of the
/// boot and the notices. The mock release carries root only in these
/// screens, so every other screen is what it was without the feature.
@MainActor
enum RootPreview {
    /// Home's screens, which scroll to the Root section.
    static let scrollTargets: [String: String] = [
        "home-root": "root", "home-root-on": "root", "home-root-warning": "root", "home-root-paused": "root",
    ]

    static func handles(_ screen: String) -> Bool {
        screen.hasPrefix("home-root") || screen.hasPrefix("boot-root")
    }

    static func apply(screen: String, to model: AppModel) {
        guard handles(screen) else { return }
        model.rootAvailable = true
        switch screen {
        case "home-root-on":
            model.guest?.root.setOn(true)
        case "home-root-warning":
            // The warning is asked for once the screen is up, as a tap on the
            // switch asks: one asked for in the first frame is presented before
            // the window has its scene.
            Task { @MainActor in
                try? await Task.sleep(for: .milliseconds(1800))
                model.setRoot(true)
            }
        case "home-root-paused":
            model.guest?.root.setOn(true)
            model.guest?.root.attention = .failedStart
        default:
            break
        }
    }
}
