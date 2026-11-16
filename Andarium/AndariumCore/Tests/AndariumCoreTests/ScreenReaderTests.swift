// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

struct ScreenReaderTests {
    private let talkBack = ScreenReader.services[0]
    private let googleTalkBack = ScreenReader.services[1]
    private let switchAccess = "com.example.switch/.SwitchService"

    private func report(
        packages: [String] = [], services: String, pm: Int = 0, settings: Int = 0
    ) -> String {
        (["pm=\(pm)", "sg=\(settings)"] + packages.map { "package:\($0)" } + ["services=\(services)"])
            .joined(separator: "\n")
    }

    // MARK: Reading

    @Test func readsAGuestWithTalkBackOn() throws {
        let state = try #require(ScreenReader.parse(
            report(packages: ["com.android.talkback"], services: "\(switchAccess):\(talkBack)")))
        #expect(state.installedService == talkBack)
        #expect(state.enabledServices == [switchAccess, talkBack])
        #expect(state.isOn)
    }

    @Test func readsAGuestThatNeverHadAService() throws {
        let state = try #require(ScreenReader.parse(report(services: "null")))
        #expect(state.installedService == nil)
        #expect(state.enabledServices.isEmpty)
        #expect(!state.isOn && !state.isInstalled)
    }

    @Test func recognisesGooglesTalkBack() throws {
        let state = try #require(ScreenReader.parse(
            report(packages: ["com.google.android.marvin.talkback"], services: "")))
        #expect(state.installedService == googleTalkBack)
        #expect(!state.isOn)
    }

    @Test func prefersTheSystemBuildWhenBothAreInstalled() throws {
        let state = try #require(ScreenReader.parse(report(
            packages: ["com.google.android.marvin.talkback", "com.android.talkback"], services: googleTalkBack)))
        #expect(state.installedService == talkBack)
        // The Google build is on, not the one the switch drives.
        #expect(!state.isOn)
    }

    @Test func anotherPackageNamedLikeTalkBackIsNotTalkBack() throws {
        let state = try #require(ScreenReader.parse(report(packages: ["com.example.talkbackhelper"], services: "null")))
        #expect(!state.isInstalled)
    }

    @Test func aReadWhoseCommandFailedIsNotAState() {
        // The services line alone, or with either command's status not zero,
        // is what an old answer or a failing command prints; none is "off".
        #expect(ScreenReader.parse("") == nil)
        #expect(ScreenReader.parse("services=null\n") == nil)
        #expect(ScreenReader.parse(report(services: "", pm: 1)) == nil)
        #expect(ScreenReader.parse(report(services: "", settings: 255)) == nil)
        #expect(ScreenReader.parse("pm=0\nsg=0\npackage:com.android.talkback\n") == nil)
        let ok = Data(report(services: "null").utf8)
        #expect(ScreenReader.parse(ShellResult(stdout: ok, exitCode: 1)) == nil)
        #expect(ScreenReader.parse(ShellResult(stdout: ok, exitCode: 0)) != nil)
    }

    @Test func theListIgnoresEmptyEntries() {
        #expect(ScreenReader.serviceList(" null \n") == [])
        #expect(ScreenReader.serviceList("a/.A::b/.B:") == ["a/.A", "b/.B"])
    }

    @Test func everyCommandStartsByNamingTheForegroundUser() {
        let commands = [
            ScreenReader.stateCommand, ScreenReader.settingsCommand,
            ScreenReader.changeCommand(turningOn: true, service: talkBack),
            ScreenReader.changeCommand(turningOn: false, service: talkBack),
        ]
        for command in commands {
            #expect(command.hasPrefix("user=$(am get-current-user)"))
            // Every `pm list packages`, `settings` and `am start` carries its --user.
            for word in ["pm list packages", "settings", "am start"].map({ $0 + " " }) {
                let all = command.components(separatedBy: word).count - 1
                let named = command.components(separatedBy: "\(word)--user $user").count - 1
                #expect(all == named, "\(word) without --user in \(command)")
            }
        }
        #expect(ScreenReader.stateCommand.contains("pm list packages --user $user"))
        #expect(ScreenReader.settingsCommand.contains("am start --user $user"))
    }

    // MARK: The switch

    @Test func theSwitchOffersWhatTheGuestHas() {
        let off = ScreenReaderState(installedService: talkBack, enabledServices: [])
        let on = ScreenReaderState(installedService: talkBack, enabledServices: [talkBack])
        let none = ScreenReaderState(installedService: nil, enabledServices: [switchAccess])
        #expect(ScreenReaderOffer(state: nil) == .unknown)
        #expect(ScreenReaderOffer(state: on) == .on)
        #expect(ScreenReaderOffer(state: off) == .off)
        #expect(ScreenReaderOffer(state: none) == .unavailable)
    }

    @Test func aReadThatFailedIsNotTheStateBeforeIt() {
        let on = ScreenReaderState(installedService: talkBack, enabledServices: [talkBack])
        #expect(ScreenReaderOffer(state: on, readFailed: true) == .failed)
        #expect(ScreenReaderOffer(state: nil, readFailed: true) == .failed)
    }

    @Test func onlyAKnownStateCanBeChanged() {
        #expect(ScreenReaderOffer.on.canChange && ScreenReaderOffer.off.canChange)
        #expect(!ScreenReaderOffer.unknown.canChange && !ScreenReaderOffer.unavailable.canChange)
        #expect(!ScreenReaderOffer.failed.canChange)
        #expect(ScreenReaderOffer.on.isOn && !ScreenReaderOffer.off.isOn)
    }

    @Test func aMockGuestChangesAsTheShellWould() {
        let state = ScreenReaderState(installedService: talkBack, enabledServices: [switchAccess, talkBack])
        #expect(ScreenReader.state(afterTurningOn: false, state).enabledServices == [switchAccess])
        #expect(ScreenReader.state(afterTurningOn: true, ScreenReaderState(
            installedService: talkBack, enabledServices: [switchAccess])).enabledServices == [switchAccess, talkBack])
    }
}
