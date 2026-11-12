// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What a harness launch asks of Andarium, for automation. The application
/// names each setting in the environment (`LaunchEnvironment.swift`, which an
/// App Store build leaves out, so no such name is in it) and answers this
/// type with the text it found. A value that is out of range, off its step or
/// malformed is ignored, as it is in Kernarium.
public struct LaunchOverrides: Equatable, Sendable {
    /// The settings a launch can state.
    public enum Setting: CaseIterable, Sendable {
        case vCPUCount, memoryMiB, diskGiB
        /// "native", or the number of guest pixels to a point.
        case displayScale
        /// Retina mode on ("1", native) or off ("0", 1.0): what harnesses
        /// stated before `displayScale`, which is read where this is stated
        /// and that is not.
        case retinaMode
        /// "normal" or "recovery": start at launch.
        case start
        /// Kernarium's boot mode, whose "recovery" also asks for a start.
        case bootMode
        /// "1" erases the guest's data before the start.
        case erase
        /// "1" opens the session with its statistics on screen and prints
        /// each reading; "0" keeps them off.
        case stats
        /// A separate guest for a harness run, leaving the ordinary guest alone.
        case instance
        /// "1" runs the guest as an iPhone's, whose orientation it follows
        /// (the host's device is not an iPhone, as in a Mac test); "0" as an
        /// iPad's.
        case followsOrientation
    }

    public var vCPUCount: Int?
    public var memoryMiB: Int?
    public var capacityGiB: Int?
    /// The display's resolution this launch states; it is that run's and not
    /// the person's choice, so it is not kept.
    public var displayScale: GuestDisplayScale?
    /// Start at launch, creating the guest first when there is none.
    public var start: StartMode?
    /// Erase the guest's data before the start.
    public var erase = false
    /// Show the statistics from the start of the run, and print them, or keep
    /// them off. Where the launch states neither, `StatsPreference` decides.
    public var statistics: Bool?
    public var instance: String?
    /// Whether the guest follows the device's orientation, where the launch
    /// states it instead of the device's idiom deciding.
    public var followsDeviceOrientation: Bool?

    public init() {}

    /// `processorCount` is the host's vCPU limit (`HostDevice.processorCount`):
    /// the vCPU count states 1 through it and nothing outside. `value` is the
    /// text the launch gave a setting, if it gave one.
    public init(processorCount: Int, value: (Setting) -> String?) {
        func integer(_ setting: Setting, in range: ClosedRange<Int>) -> Int? {
            value(setting).flatMap { Int($0) }.flatMap { range.contains($0) ? $0 : nil }
        }
        func flag(_ setting: Setting) -> Bool? {
            switch value(setting) {
            case "0"?: false
            case "1"?: true
            default: nil
            }
        }
        vCPUCount = integer(.vCPUCount, in: 1...max(processorCount, 1))
        memoryMiB = integer(.memoryMiB, in: Limits.runtimeMemoryRangeMiB)
            .flatMap { MemoryPolicy.runtimeAccepts($0) ? $0 : nil }
        capacityGiB = integer(.diskGiB, in: Limits.capacityRangeGiB)
        displayScale = value(.displayScale).flatMap { GuestDisplayScale(text: $0) }
            ?? flag(.retinaMode).map { $0 ? .native : .points }
        switch value(.start) {
        case "normal"?: start = .normal
        case "recovery"?: start = .recovery
        default:
            // Kernarium's boot-mode name asks for a start only where the
            // launch names Recovery; the others leave Home alone.
            if value(.bootMode) == "recovery" { start = .recovery }
        }
        erase = value(.erase) == "1"
        statistics = flag(.stats)
        followsDeviceOrientation = flag(.followsOrientation)
        if let name = value(.instance), (1...64).contains(name.utf8.count),
           name.utf8.allSatisfy({
               (65...90).contains($0) || (97...122).contains($0) ||
                   (48...57).contains($0) || $0 == 45 || $0 == 95
           }) {
            instance = name
        } else if value(.instance) != nil {
            // A malformed selector must never erase or start the ordinary guest.
            start = nil
            erase = false
        }
    }

    public var isEmpty: Bool { self == LaunchOverrides() }

    /// The guest as this launch asks for it. A guest not yet created takes
    /// the capacity at once; an existing one can only grow. A stored vCPU
    /// count above what the host offers comes down to it. A memory size the
    /// environment states is kept as stated, for automation; any other is
    /// taken within the bound of the vCPU count the guest then has, as a
    /// stored size is on load. A capacity the launch states is held to
    /// `capacityMaximumGiB`, the largest data disk the carried system accepts.
    public func applied(
        to guest: Guest, on host: HostDevice, capacityMaximumGiB: Int = Limits.capacityRangeGiB.upperBound
    ) -> Guest {
        var guest = guest
        guest.cores = min(max(guest.cores, 1), host.processorCount)
        if let vCPUCount { guest.cores = min(vCPUCount, host.processorCount) }
        if let memoryMiB {
            guest.memoryMiB = memoryMiB
        } else {
            guest = guest.memoryWithinBound(on: host)
        }
        if let capacityGiB { guest.setCapacity(capacityGiB, maximum: capacityMaximumGiB) }
        return guest
    }
}

/// The display option set on Home, kept across launches. The guest's display
/// always follows the window, so that is not an option; a value an earlier
/// version stored for it is ignored.
///
/// The resolution (`ResolutionChoices`) follows the device until the person
/// sets it: native where the system lets the application use more than
/// `nativeDefaultAboveBytes`, and where it does not the choice nearest 1.5.
/// A choice made on Home is kept as made and the default never replaces it.
/// Native is stored as the symbol and not as the screen's number, which
/// differs between devices and display zooms.
public struct DisplayOptions: Codable, Equatable, Sendable {
    /// The stored form's key in the defaults database.
    public static let storageKey = "andarium.display"

    /// What the person chose for the resolution; nil until they choose.
    public var chosenScale: GuestDisplayScale?

    public init(chosenScale: GuestDisplayScale? = nil) {
        self.chosenScale = chosenScale
    }

    /// The resolution is native by default above this much memory the system
    /// lets the application use (4 GiB).
    public static let nativeDefaultAboveBytes: UInt64 = 4 << 30

    /// The default for `memoryLimitBytes`, what the system holds the
    /// application's memory to (`MemoryLimit.bytes`; zero where it sets none,
    /// a Mac, which has more than the threshold): native above it, and the
    /// choice nearest 1.5 elsewhere.
    public static func defaultScale(memoryLimitBytes: UInt64, choices: ResolutionChoices) -> GuestDisplayScale {
        if memoryLimitBytes == 0 || memoryLimitBytes > nativeDefaultAboveBytes { return .native }
        return choices.scale(at: choices.index(nearest: .factor(ResolutionChoices.preferredMiddle)))
    }

    /// The resolution as the guest draws: the person's choice, else the
    /// default, either as a choice of `choices`.
    public func scale(memoryLimitBytes: UInt64, choices: ResolutionChoices) -> GuestDisplayScale {
        choices.resolve(
            chosenScale ?? Self.defaultScale(memoryLimitBytes: memoryLimitBytes, choices: choices))
    }

    // The stored form keeps `scale`. An earlier form kept Retina mode as
    // `retina` and `chosen` (whether the person set it): their choice of on is
    // native and of off 1.0, and stays their choice. A form from before the
    // default has no `chosen`: its `true` was a choice, and its `false` may
    // only have been the old default, so it counts as no choice.
    private enum CodingKeys: String, CodingKey { case scale, retina, chosen }

    public init(from decoder: any Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        if values.contains(.scale) {
            // A value that is no scale is no choice.
            chosenScale = try? values.decode(GuestDisplayScale.self, forKey: .scale)
            return
        }
        let retina = try values.decodeIfPresent(Bool.self, forKey: .retina) ?? false
        let chosen: Bool
        switch try values.decodeIfPresent(Bool.self, forKey: .chosen) {
        case let stated?: chosen = stated
        case nil: chosen = retina
        }
        chosenScale = chosen ? (retina ? .native : .points) : nil
    }

    public func encode(to encoder: any Encoder) throws {
        var values = encoder.container(keyedBy: CodingKeys.self)
        try values.encodeIfPresent(chosenScale, forKey: .scale)
    }
}

/// The Linux key codes Andarium sends the guest for its side keys. Android's
/// generic key layout maps each to the Android key of the same meaning.
public enum GuestKey: UInt16, CaseIterable, Sendable {
    case power = 116
    case volumeUp = 115
    case volumeDown = 114
    case back = 158
    case home = 172
}
