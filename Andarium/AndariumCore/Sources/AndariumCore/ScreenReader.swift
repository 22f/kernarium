// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// TalkBack, Android's screen reader, in the guest. Andarium turns it on and
/// off over adb the way Android's own Accessibility page does: the service
/// joins or leaves the secure setting `enabled_accessibility_services`, and
/// `accessibility_enabled` follows whether any service is left. Every command
/// addresses the foreground user, whom it names (`--user`), and a change reads
/// the service list, edits it and reads it back in one shell command, so a
/// service Android turned on since the host last looked stays on. The system
/// image carries TalkBack and its voice; a guest whose system has none offers
/// no switch.
public enum ScreenReader {
    /// The TalkBack builds Andarium recognises, by their service: the
    /// open-source build the system image carries, then Google's own.
    public static let services = [
        "com.android.talkback/com.google.android.marvin.talkback.TalkBackService",
        "com.google.android.marvin.talkback/com.google.android.marvin.talkback.TalkBackService",
    ]

    /// The first words of every command: the foreground user's number, which
    /// the rest names. The command ends here when Android names none.
    private static let user = "user=$(am get-current-user); case \"$user\" in ''|*[!0-9]*) exit 1;; esac"

    /// What a read and a change end with: the output `parse` reads. Each
    /// command's status is a marker, so a failing command is not read as an
    /// empty answer. `pm list packages` filters by substring and prints
    /// `package:<name>` lines.
    private static let report =
        "packages=$(pm list packages --user $user talkback); pm=$?; "
        + "services=$(settings --user $user get secure enabled_accessibility_services); sg=$?; "
        + "echo \"pm=$pm\"; echo \"sg=$sg\"; echo \"$packages\"; echo \"services=$services\""

    /// What to run in the guest's shell to read the state; `parse` reads its
    /// output.
    public static let stateCommand = "\(user); \(report)"

    /// The state from the output of `stateCommand` or `changeCommand`; nil when
    /// the output is not what they print, or a command in them failed.
    public static func parse(_ output: String) -> ScreenReaderState? {
        var packages = Set<String>()
        var enabled: [String]?
        var succeeded = Set<String>()
        for raw in output.split(whereSeparator: \.isNewline) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("package:") {
                packages.insert(String(line.dropFirst("package:".count)))
            } else if line.hasPrefix("services=") {
                enabled = serviceList(String(line.dropFirst("services=".count)))
            } else if line == "pm=0" || line == "sg=0" {
                succeeded.insert(line)
            }
        }
        guard succeeded.count == 2, let enabled else { return nil }
        let installed = services.first { packages.contains(package(of: $0)) }
        return ScreenReaderState(installedService: installed, enabledServices: enabled)
    }

    /// The state from a finished `stateCommand` or `changeCommand`: nil when
    /// the shell, or a command in it, failed.
    public static func parse(_ result: ShellResult) -> ScreenReaderState? {
        guard result.exitCode == 0 else { return nil }
        return parse(result.text)
    }

    /// The services in the setting's value: colon-separated components, or
    /// `null` when the setting was never written.
    public static func serviceList(_ value: String) -> [String] {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed != "null" else { return [] }
        return trimmed.split(separator: ":").map(String.init).filter { !$0.isEmpty }
    }

    /// The command that turns TalkBack's `service` on or off: it reads the
    /// setting itself, takes every known TalkBack out of the list, adds
    /// `service` at the end when turning on, writes the list back (or deletes
    /// the setting and turns accessibility off when none is left) and ends with
    /// the report of `stateCommand`. It fails when a write does.
    public static func changeCommand(turningOn on: Bool, service: String) -> String {
        let known = services.map(shellQuoted).joined(separator: "|")
        let adding = on ? "list=\"${list:+$list:}\"\(shellQuoted(service)); " : ""
        return [
            user,
            "old=$(settings --user $user get secure enabled_accessibility_services) || exit 1",
            "list=; ifs=$IFS; IFS=:; for s in $old; do case \"$s\" in ''|null|\(known)) ;; "
                + "*) list=\"${list:+$list:}$s\";; esac; done; IFS=$ifs",
            adding
                + "if [ -n \"$list\" ]; then "
                + "settings --user $user put secure enabled_accessibility_services \"$list\" && "
                + "settings --user $user put secure accessibility_enabled 1 || exit 1; else "
                + "settings --user $user delete secure enabled_accessibility_services >/dev/null; "
                + "settings --user $user put secure accessibility_enabled 0 || exit 1; fi",
            report,
        ].joined(separator: "; ")
    }

    /// Opens Android's Accessibility page, where TalkBack's own settings and
    /// the other services are.
    public static let settingsCommand =
        "\(user); am start --user $user -a android.settings.ACCESSIBILITY_SETTINGS >/dev/null"

    /// The state a change leaves in a mock guest, which has no shell.
    public static func state(afterTurningOn on: Bool, _ state: ScreenReaderState) -> ScreenReaderState {
        var after = state
        after.enabledServices.removeAll { services.contains($0) }
        if on, let service = state.installedService { after.enabledServices.append(service) }
        return after
    }

    static func package(of service: String) -> String {
        String(service.prefix { $0 != "/" })
    }

    static func shellQuoted(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }
}

/// TalkBack in the guest as Android last reported it.
public struct ScreenReaderState: Equatable, Sendable {
    /// The service of the TalkBack the guest has, from `ScreenReader.services`;
    /// nil when it has none.
    public var installedService: String?
    /// Every accessibility service the guest has turned on, in its order.
    public var enabledServices: [String]

    public init(installedService: String?, enabledServices: [String]) {
        self.installedService = installedService
        self.enabledServices = enabledServices
    }

    /// TalkBack is installed and on.
    public var isOn: Bool {
        guard let installedService else { return false }
        return enabledServices.contains(installedService)
    }

    public var isInstalled: Bool { installedService != nil }
}

/// What the TalkBack switch of the session's Settings offers.
public enum ScreenReaderOffer: Equatable, Sendable {
    /// Not read yet.
    case unknown
    /// The last read got no answer: Android did not say.
    case failed
    case on
    case off
    /// The guest's system has no TalkBack.
    case unavailable

    public init(state: ScreenReaderState?, readFailed: Bool = false) {
        if readFailed {
            self = .failed
            return
        }
        guard let state else {
            self = .unknown
            return
        }
        if state.isInstalled {
            self = state.isOn ? .on : .off
        } else {
            self = .unavailable
        }
    }

    /// Whether the switch shows TalkBack on.
    public var isOn: Bool { self == .on }

    /// Whether the switch can be used.
    public var canChange: Bool {
        switch self {
        case .on, .off: true
        case .unknown, .failed, .unavailable: false
        }
    }
}
