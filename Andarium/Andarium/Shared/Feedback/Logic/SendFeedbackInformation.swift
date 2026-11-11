// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import AndariumCore
import Foundation

/// The basic information at the end of the mail: plain text, English keys
/// whatever the interface language, since the receiver reads it. It holds
/// nothing that singles out the device or the person: no device name, no
/// vendor identifier, no account and no path, which this type has no field for.
nonisolated struct SendFeedbackInformation: Equatable, Sendable {
    /// The virtual machine's settings.
    struct VirtualMachine: Equatable, Sendable {
        var vcpus: Int
        var memoryMiB: Int
        /// How the display is set, in words ("1206 x 2622 px, Retina").
        var display: String
        /// The loader's estimate of graphics memory for these settings.
        var graphicsMiB: Int?
    }

    var appVersion: String
    var build: String
    var configuration: String
    /// The hardware's machine identifier ("iPhone18,1").
    var deviceModel: String
    var systemVersion: String
    var language: String
    /// nil before a virtual machine is created.
    var virtualMachine: VirtualMachine?
    var androidRelease: String
    /// Seconds the virtual machine has run; nil while none runs.
    var runningSeconds: Int?
    /// The free space as the application shows it.
    var freeSpaceBytes: UInt64
    var thermalState: String

    var lines: [String] {
        var lines = [
            "App: Andarium \(appVersion) (\(build)), \(configuration)",
            "Device: \(deviceModel)",
            "iOS: \(systemVersion)",
            "Language: \(language)",
        ]
        if let machine = virtualMachine {
            var line = "Virtual machine: \(Formats.vcpus(machine.vcpus)), \(Formats.size(mib: machine.memoryMiB)) memory"
            line += ", display \(machine.display)"
            if let graphics = machine.graphicsMiB { line += ", graphics memory about \(Formats.size(mib: graphics))" }
            lines.append(line)
        } else {
            lines.append("Virtual machine: not created")
        }
        lines.append("Android: \(androidRelease)")
        lines.append("Running: \(runningSeconds.map { "yes, for \(Self.duration($0))" } ?? "no")")
        lines.append("Free space: \(Formats.size(bytes: freeSpaceBytes))")
        lines.append("Thermal state: \(thermalState)")
        return lines.map(Self.oneLine)
    }

    var text: String { lines.joined(separator: "\n") }

    /// "45 s", "42 min" or "1 h 12 min".
    static func duration(_ seconds: Int) -> String {
        let seconds = max(seconds, 0)
        if seconds < 60 { return "\(seconds) s" }
        let minutes = seconds / 60
        if minutes < 60 { return "\(minutes) min" }
        return "\(minutes / 60) h \(minutes % 60) min"
    }

    private static func oneLine(_ text: String) -> String {
        text.components(separatedBy: .newlines).joined(separator: " ")
    }
}

#endif
