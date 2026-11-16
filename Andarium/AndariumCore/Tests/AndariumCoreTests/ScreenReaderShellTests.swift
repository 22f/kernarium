// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The commands run in a real shell against stand-ins for `am`, `pm` and
/// `settings` that keep the setting in a file, so what each command does to
/// the list, and what it prints, is what is checked.
struct ScreenReaderShellTests {
    private let talkBack = ScreenReader.services[0]
    private let googleTalkBack = ScreenReader.services[1]
    private let switchAccess = "com.example.switch/.SwitchService"
    private let other = "com.example.other/.OtherService"

    /// A guest's tools in a temporary directory.
    final class FakeAndroid {
        let directory: URL

        init(packages: [String] = ["com.android.talkback"], user: String = "0") throws {
            directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try write("packages", packages.map { "package:\($0)" }.joined(separator: "\n"))
            try write("user", user)
            try tool("am", """
                case "$1" in
                get-current-user) cat "$FAKE/user" ;;
                start) echo "am $*" >>"$FAKE/log"; [ -e "$FAKE/fail-start" ] && exit 1; exit 0 ;;
                esac
                """)
            try tool("pm", """
                echo "pm $*" >>"$FAKE/log"
                [ -e "$FAKE/fail-pm" ] && exit 1
                cat "$FAKE/packages"
                """)
            try tool("settings", """
                echo "settings $*" >>"$FAKE/log"
                [ "$1" = --user ] && shift 2
                action=$1; key=$3
                case "$action" in
                get) [ -e "$FAKE/fail-get" ] && exit 1
                     if [ -e "$FAKE/$key" ]; then cat "$FAKE/$key"; else echo null; fi ;;
                put) [ -e "$FAKE/fail-put-$key" ] && exit 1; printf '%s' "$4" >"$FAKE/$key" ;;
                delete) [ -e "$FAKE/fail-delete" ] && exit 1; mv -f "$FAKE/$key" "$FAKE/deleted-$key" 2>/dev/null; exit 0 ;;
                esac
                """)
        }

        deinit { try? FileManager.default.removeItem(at: directory) }

        func write(_ name: String, _ text: String) throws {
            try text.write(to: directory.appendingPathComponent(name), atomically: true, encoding: .utf8)
        }

        func tool(_ name: String, _ body: String) throws {
            let url = directory.appendingPathComponent(name)
            try ("#!/bin/sh\n" + body + "\n").write(to: url, atomically: true, encoding: .utf8)
            try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: url.path)
        }

        func set(services: [String]?, enabled: Bool? = nil) throws {
            let key = directory.appendingPathComponent("enabled_accessibility_services")
            if let services { try services.joined(separator: ":").write(to: key, atomically: true, encoding: .utf8) }
            else { try? FileManager.default.removeItem(at: key) }
            if let enabled { try write("accessibility_enabled", enabled ? "1" : "0") }
        }

        func fail(_ name: String) throws { try write(name, "") }

        var services: String? { read("enabled_accessibility_services") }
        var accessibilityEnabled: String? { read("accessibility_enabled") }
        var log: [String] { (read("log") ?? "").split(separator: "\n").map(String.init) }

        private func read(_ name: String) -> String? {
            try? String(contentsOf: directory.appendingPathComponent(name), encoding: .utf8)
        }

        func run(_ command: String) throws -> ShellResult {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/bin/sh")
            process.arguments = ["-c", command]
            process.environment = ["PATH": "\(directory.path):/usr/bin:/bin", "FAKE": directory.path]
            let output = Pipe()
            process.standardOutput = output
            process.standardError = Pipe()
            try process.run()
            let data = output.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            return ShellResult(stdout: data, exitCode: Int(process.terminationStatus))
        }
    }

    // MARK: Reading

    @Test func aReadReportsTheGuestsState() throws {
        let guest = try FakeAndroid()
        try guest.set(services: [switchAccess, talkBack])
        let state = try #require(ScreenReader.parse(try guest.run(ScreenReader.stateCommand)))
        #expect(state.isOn && state.enabledServices == [switchAccess, talkBack])
        #expect(guest.log.allSatisfy { $0.contains("--user 0") })
    }

    @Test func eachFailingCommandMakesTheReadFail() throws {
        for failing in ["fail-pm", "fail-get"] {
            let guest = try FakeAndroid()
            try guest.fail(failing)
            let result = try guest.run(ScreenReader.stateCommand)
            // The shell's own status is zero: the markers carry the failure.
            #expect(result.exitCode == 0)
            #expect(ScreenReader.parse(result) == nil, "\(failing) read as a state")
        }
        let none = try FakeAndroid()
        try none.write("user", "")
        #expect(try none.run(ScreenReader.stateCommand).exitCode != 0)
    }

    @Test func theForegroundUserIsTheOneNamed() throws {
        let guest = try FakeAndroid(user: "10")
        _ = try guest.run(ScreenReader.stateCommand)
        _ = try guest.run(ScreenReader.changeCommand(turningOn: true, service: talkBack))
        _ = try guest.run(ScreenReader.settingsCommand)
        #expect(!guest.log.isEmpty && guest.log.allSatisfy { $0.contains("--user 10") })
    }

    // MARK: Changing

    @Test func turningOnAddsTalkBackAfterTheOtherServices() throws {
        let guest = try FakeAndroid()
        try guest.set(services: [switchAccess])
        let state = try #require(ScreenReader.parse(try guest.run(
            ScreenReader.changeCommand(turningOn: true, service: talkBack))))
        #expect(guest.services == "\(switchAccess):\(talkBack)")
        #expect(guest.accessibilityEnabled == "1")
        #expect(state.isOn && state.enabledServices == [switchAccess, talkBack])
    }

    @Test func turningOnTwiceDoesNotListTalkBackTwice() throws {
        let guest = try FakeAndroid()
        try guest.set(services: [talkBack])
        _ = try guest.run(ScreenReader.changeCommand(turningOn: true, service: talkBack))
        #expect(guest.services == talkBack)
    }

    @Test func turningOffKeepsTheOtherServicesOn() throws {
        let guest = try FakeAndroid()
        try guest.set(services: [talkBack, switchAccess, googleTalkBack])
        let state = try #require(ScreenReader.parse(try guest.run(
            ScreenReader.changeCommand(turningOn: false, service: talkBack))))
        #expect(guest.services == switchAccess)
        #expect(guest.accessibilityEnabled == "1")
        #expect(!state.isOn)
    }

    @Test func turningOffTheLastServiceTurnsAccessibilityOff() throws {
        let guest = try FakeAndroid()
        try guest.set(services: [talkBack], enabled: true)
        let state = try #require(ScreenReader.parse(try guest.run(
            ScreenReader.changeCommand(turningOn: false, service: talkBack))))
        #expect(guest.services == nil)
        #expect(guest.accessibilityEnabled == "0")
        #expect(state.enabledServices.isEmpty)
    }

    @Test func aServiceAndroidTurnedOnMeanwhileSurvivesBothChanges() throws {
        // The command reads the list itself, so it neither drops `other` nor
        // needs the host's list.
        let guest = try FakeAndroid()
        try guest.set(services: [talkBack, other])
        _ = try guest.run(ScreenReader.changeCommand(turningOn: false, service: talkBack))
        #expect(guest.services == other)
        try guest.set(services: [other, switchAccess])
        _ = try guest.run(ScreenReader.changeCommand(turningOn: true, service: talkBack))
        #expect(guest.services == "\(other):\(switchAccess):\(talkBack)")
    }

    @Test func aChangeThatCouldNotWriteFails() throws {
        let guest = try FakeAndroid()
        try guest.set(services: [switchAccess])
        try guest.fail("fail-put-enabled_accessibility_services")
        let result = try guest.run(ScreenReader.changeCommand(turningOn: true, service: talkBack))
        #expect(result.exitCode != 0)
        #expect(guest.services == switchAccess)

        let second = try FakeAndroid()
        try second.set(services: [switchAccess])
        try second.fail("fail-put-accessibility_enabled")
        #expect(try second.run(ScreenReader.changeCommand(turningOn: true, service: talkBack)).exitCode != 0)
    }

    @Test func aChangeWhoseReadFailsFailsBeforeItWrites() throws {
        let guest = try FakeAndroid()
        try guest.set(services: [switchAccess])
        try guest.fail("fail-get")
        let result = try guest.run(ScreenReader.changeCommand(turningOn: true, service: talkBack))
        #expect(result.exitCode != 0)
        #expect(!guest.log.contains { $0.contains(" put ") })
    }

    @Test func aServiceNameCannotBreakOutOfTheCommand() throws {
        let guest = try FakeAndroid()
        let hostile = "x/.Y';touch \(guest.directory.path)/pwned;'"
        _ = try guest.run(ScreenReader.changeCommand(turningOn: true, service: hostile))
        #expect(guest.services == hostile)
        #expect(!FileManager.default.fileExists(atPath: guest.directory.appendingPathComponent("pwned").path))
    }

    // MARK: Android's page

    @Test func theAccessibilityPageOpensForTheForegroundUser() throws {
        let guest = try FakeAndroid(user: "0")
        #expect(try guest.run(ScreenReader.settingsCommand).exitCode == 0)
        #expect(guest.log == ["am start --user 0 -a android.settings.ACCESSIBILITY_SETTINGS"])
        try guest.fail("fail-start")
        #expect(try guest.run(ScreenReader.settingsCommand).exitCode != 0)
    }
}
