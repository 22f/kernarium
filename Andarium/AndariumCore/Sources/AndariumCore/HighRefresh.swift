// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Settings' "120 Hz" under Performance: whether the next Android start asks
/// for a 120 Hz display. Off is the default and the display is what it has
/// always been. The choice lasts across launches. A display that is 120 Hz is
/// fixed at the start, so a change applies when Android next starts.
public struct HighRefreshPreference {
    /// The stored value's key in the defaults database; a missing value is the
    /// default, off.
    public static let key = "andarium.highRefresh"

    private let defaults: UserDefaults

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    /// The last value kept; off until one is.
    public var isOn: Bool { defaults.object(forKey: Self.key) as? Bool ?? false }

    public func keep(_ on: Bool) {
        defaults.set(on, forKey: Self.key)
    }

    /// How a launch begins.
    public struct Launch: Equatable, Sendable {
        /// Whether the setting is on from the start.
        public var isOn: Bool
        /// Whether what the person sets from here on is kept. A harness's
        /// launch decides for its run alone, so what the person chose stays.
        public var keepsChanges: Bool
    }

    /// The start of a launch. One that states the value starts as stated. Any
    /// other harness launch starts with the default, so that a measured run
    /// does not inherit the person's choice; only a launch of the person's own
    /// restores the kept value.
    public func launch(automated: Bool, stated: Bool? = nil) -> Launch {
        if let stated { return Launch(isOn: stated, keepsChanges: false) }
        return automated ? Launch(isOn: false, keepsChanges: false) : Launch(isOn: isOn, keepsChanges: true)
    }
}

/// What the 120 Hz setting asks of the boot, from the device and the choice.
public enum HighRefresh {
    /// A screen that can show more than 60 frames a second, an iPhone Pro or an
    /// iPad Pro, is what the setting is offered on.
    public static func isOffered(maximumFramesPerSecond: Int) -> Bool {
        maximumFramesPerSecond > 60
    }

    /// Whether the boot asks for the 120 Hz display: the setting is on, and
    /// this screen can show it. A device that cannot never asks, whatever a
    /// launch states.
    public static func asksAtBoot(isOn: Bool, maximumFramesPerSecond: Int) -> Bool {
        isOn && isOffered(maximumFramesPerSecond: maximumFramesPerSecond)
    }

    /// Whether Android is held to the 60 Hz rate while iOS is in Low Power
    /// Mode: the display is then the 120 Hz one, whose screen shows no more
    /// than 60 and so has no use for faster frames. A 60 Hz display needs no
    /// hold.
    public static func holdsAtSixty(displayIs120: Bool, lowPowerMode: Bool) -> Bool {
        displayIs120 && lowPowerMode
    }

    /// "on" while the next start asks for the 120 Hz display, "off" while it
    /// does not, in the fixed English the session record and the feedback
    /// information use.
    public static func state(isOn: Bool) -> String { isOn ? "on" : "off" }
}
