// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
import UIKit
@testable import Andarium

/// A build made with Send feedback: what the tests' launches have unless they
/// say otherwise, whatever the build that runs them was made with.
private let withSendFeedback: Set<WhatsNewFeature> = [.sendFeedback]

private extension AppEntry {
    /// The launch's notes for a six-vCPU guest, in a build with `features`.
    @MainActor
    func announce(on model: AppModel, hasGuest: Bool, features: Set<WhatsNewFeature> = withSendFeedback) {
        announceWhatsNew(on: model, hasGuest: hasGuest, vCPUs: 6, features: features)
    }
}

/// The bundled release notes and how a launch announces them: the person's
/// own launch gets the ribbon and keeps its record, any other shows no
/// ribbon and keeps none. Each test keeps into a defaults database of its own.
@MainActor
struct WhatsNewTests {
    private func withDefaults(_ body: (UserDefaults) throws -> Void) rethrows {
        let name = "andarium-tests-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defer { defaults.removePersistentDomain(forName: name) }
        try body(defaults)
    }

    /// An iPhone with haptics, unless said otherwise.
    private func host(maximumMiB: Int, idiom: HostDevice.Idiom = .phone, hasHaptics: Bool = true) -> HostDevice {
        HostDevice(
            name: idiom == .pad ? "iPad" : "iPhone", processorCount: 6,
            memory: MemoryPolicy(maximumsMiB: [maximumMiB]), freeStorageBytes: 40_000_000_000,
            idiom: idiom, hasHaptics: hasHaptics)
    }

    private func withDefaultsReturning<Result>(_ body: (UserDefaults) -> Result) -> Result {
        let name = "andarium-tests-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defer { defaults.removePersistentDomain(forName: name) }
        return body(defaults)
    }

    private func model(on host: HostDevice) -> AppModel {
        AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(
                version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1.0 (10)"),
            capabilities: .device, createRequirementBytes: 1)
    }

    private func entry(announces: Bool, build: Int? = 10, defaults: UserDefaults) -> AppEntry {
        var entry = AppEntry(overrides: LaunchOverrides())
        entry.announcesWhatsNew = announces
        entry.build = build
        entry.defaults = defaults
        return entry
    }

    // MARK: The bundled notes

    /// Build 10 as written, and as an iPhone with haptics and an iPad are told it.
    private static let buildTenIDs = [
        "snapshots", "resolution", "ipad-rotation", "keyboard-mouse-controllers", "location", "haptics",
        "send-feedback", "more-fixes",
    ]
    private static let buildTenIDsOnAnIPhone = buildTenIDs.filter { $0 != "ipad-rotation" }
    private static let buildTenIDsOnAnIPad = buildTenIDs.filter { $0 != "haptics" }
    private static let buildNineIDs = ["file-browser", "more-memory", "stability-fixes"]

    @Test func theBundledNotesDecodeAndStartWithBuildTen() throws {
        let url = try #require(Bundle.main.url(forResource: "WhatsNew", withExtension: "json"))
        let releases = try WhatsNewCatalog.decode(Data(contentsOf: url))
        #expect(releases.map(\.build) == [10, 9])
        #expect(releases.map(\.version) == ["1.0 (10)", "1.0 (9)"])
        #expect(WhatsNewCatalog.load() == releases)
        let ten = try #require(releases.first?.entries)
        #expect(ten.map(\.id) == Self.buildTenIDs)
        #expect(ten.first?.title == "Snapshots")
        // The fixes are six points under a title, with no sentence of their own.
        let tenFixes = try #require(ten.last)
        #expect(tenFixes.title == "More fixes" && tenFixes.text == nil && tenFixes.points.count == 6)
        #expect(tenFixes.place == nil && tenFixes.requirement == nil)
        // Build 9's notes are kept as they were.
        let nine = try #require(releases.last?.entries)
        #expect(nine.map(\.id) == Self.buildNineIDs)
        #expect(nine.first?.title == "File browser")
        let nineFixes = try #require(nine.last)
        #expect(nineFixes.title == "Stability fixes" && nineFixes.text == nil && nineFixes.points.count == 6)
        #expect(nineFixes.place == nil && nineFixes.requirement == nil)
    }

    @Test func everyEntryHasWordsASymbolAndARequirementTheCodeKnows() {
        for release in WhatsNewCatalog.load() {
            #expect(!release.version.isEmpty)
            for entry in release.entries {
                #expect(!entry.title.isEmpty && (entry.text != nil || !entry.points.isEmpty), "\(entry.id)")
                #expect(UIImage(systemName: entry.symbol) != nil, "\(entry.id): \(entry.symbol)")
            }
        }
        // A requirement the code does not know fails to decode, so the notes
        // would not be here; these four entries are the ones that have any.
        let requirements = WhatsNewCatalog.load().flatMap(\.entries).compactMap { entry in
            entry.requirement.map { (entry.id, $0) }
        }
        #expect(Dictionary(uniqueKeysWithValues: requirements) == [
            "more-memory": WhatsNewRequirement(memoryAboveMiB: 4096),
            "ipad-rotation": WhatsNewRequirement(device: .pad),
            "haptics": WhatsNewRequirement(device: .haptics),
            "send-feedback": WhatsNewRequirement(feature: .sendFeedback),
        ])
    }

    @Test func theRibbonNamesTheBundledEntriesAndCountsTheRest() throws {
        let titles = try #require(WhatsNewCatalog.load().first).entries.map(\.title)
        #expect(titles == [
            "Snapshots", "Resolution", "Rotation on iPad", "Keyboard, mouse and controllers", "Location", "Haptics",
            "Send feedback", "More fixes",
        ])
        #expect(WhatsNewRibbon.summary(titles, naming: 8) == titles.joined(separator: " \u{00B7} "))
        #expect(WhatsNewRibbon.summary(titles, naming: 2) == "Snapshots \u{00B7} Resolution \u{00B7} 6 more")
        #expect(WhatsNewRibbon.summary(titles, naming: 0) == "8 new features")
    }

    /// The ribbon lists what the device is told, in the order written: seven
    /// entries each, but not the same seven.
    @Test func theRibbonNamesWhatAnIPhoneAndAnIPadAreTold() {
        func ribbon(on host: HostDevice, naming named: Int) -> String {
            withDefaultsReturning { defaults in
                WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 9))
                let model = model(on: host)
                entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
                return WhatsNewRibbon.summary(model.whatsNew.pendingEntries.map(\.title), naming: named)
            }
        }
        let separator = " \u{00B7} "
        #expect(ribbon(on: host(maximumMiB: 4096), naming: 4) == [
            "Snapshots", "Resolution", "Keyboard, mouse and controllers", "Location", "3 more",
        ].joined(separator: separator))
        #expect(ribbon(on: host(maximumMiB: 4096, idiom: .pad, hasHaptics: false), naming: 4) == [
            "Snapshots", "Resolution", "Rotation on iPad", "Keyboard, mouse and controllers", "3 more",
        ].joined(separator: separator))
        #expect(ribbon(on: host(maximumMiB: 4096), naming: 0) == "7 new features")
        #expect(ribbon(on: host(maximumMiB: 4096, idiom: .pad, hasHaptics: false), naming: 0) == "7 new features")
    }

    @Test func eachKindOfDeviceIsToldItsOwnEntries() {
        func told(by host: HostDevice) -> [String] {
            withDefaultsReturning { defaults in
                WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 9))
                let model = model(on: host)
                entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
                // Info lists the notes as written, whatever the device.
                #expect(model.whatsNew.releases.first?.entries.map(\.id) == Self.buildTenIDs)
                return model.whatsNew.pendingEntries.map(\.id)
            }
        }
        let withoutDeviceEntries = Self.buildTenIDs.filter { $0 != "ipad-rotation" && $0 != "haptics" }
        #expect(told(by: host(maximumMiB: 4096)) == Self.buildTenIDsOnAnIPhone)
        #expect(told(by: host(maximumMiB: 4096, idiom: .pad, hasHaptics: false)) == Self.buildTenIDsOnAnIPad)
        #expect(told(by: host(maximumMiB: 4096, hasHaptics: false)) == withoutDeviceEntries)
        #expect(told(by: host(maximumMiB: 4096, idiom: .mac, hasHaptics: false)) == withoutDeviceEntries)
    }

    /// A build without Send feedback (a sideload, a Debug build, an App Store
    /// build made with no receiver) neither announces the entry nor lists it,
    /// which a device that lacks a haptic engine or an iPad's turn does not do:
    /// Info keeps those as written.
    @Test func aBuildWithoutSendFeedbackLeavesItOutOfTheRibbonTheSheetAndInfo() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 9))
            let model = model(on: host(maximumMiB: 4096))
            entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true, features: [])
            let withoutFeedback = Self.buildTenIDs.filter { $0 != "send-feedback" }
            let toldWithoutFeedback = Self.buildTenIDsOnAnIPhone.filter { $0 != "send-feedback" }
            #expect(model.whatsNew.pendingEntries.map(\.id) == toldWithoutFeedback)
            #expect(model.whatsNew.pending.first?.entries.count == 6)
            // Info: the device's entries as written (Rotation on iPad, here on an
            // iPhone), but not the feature the build lacks.
            #expect(model.whatsNew.releases.first?.entries.map(\.id) == withoutFeedback)
            #expect(model.whatsNew.releases.map(\.build) == [10, 9])
            #expect(WhatsNewRibbon.summary(model.whatsNew.pendingEntries.map(\.title), naming: 0) == "6 new features")
        }
    }

    @Test func aLaunchThatIsNotThePersonsListsOnlyWhatTheBuildHas() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 4096))
            entry(announces: false, defaults: defaults).announce(on: model, hasGuest: true, features: [])
            #expect(!model.whatsNew.showsRibbon)
            #expect(model.whatsNew.releases.first?.entries.map(\.id).contains("send-feedback") == false)
        }
    }

    /// A release whose entries are all about a feature the build lacks is in
    /// no screen.
    @Test func aReleaseThatTheMissingFeatureEmptiesIsDropped() {
        withDefaults { defaults in
            let only = WhatsNewRelease(
                build: 11, version: "1.0 (11)",
                entries: [WhatsNewEntry(
                    id: "only", title: "Only", text: "T", symbol: "star",
                    requirement: WhatsNewRequirement(feature: .sendFeedback))])
            let model = model(on: host(maximumMiB: 4096))
            let notes = WhatsNewCatalog.load() + [only]
            let launch = entry(announces: false, defaults: defaults)
            launch.announceWhatsNew(on: model, hasGuest: true, vCPUs: 6, features: [], notes: notes)
            #expect(model.whatsNew.releases.map(\.build) == [10, 9])
            launch.announceWhatsNew(on: model, hasGuest: true, vCPUs: 6, features: [.sendFeedback], notes: notes)
            #expect(model.whatsNew.releases.map(\.build) == [11, 10, 9])
        }
    }

    /// What the model says its build has is what Send feedback's own entrances
    /// use: a receiver, and `#if FEEDBACK_MAIL` is the build's own switch.
    @Test func theModelsBuildHasSendFeedbackExactlyWhenItHasAReceiver() {
        let model = model(on: host(maximumMiB: 4096))
        #if FEEDBACK_MAIL
        model.feedbackServices.address = { "feedback@example.com" }
        #expect(model.buildFeatures == [.sendFeedback])
        model.feedbackServices.address = { nil }
        #expect(model.buildFeatures.isEmpty)
        // A launch that is given none asks the model.
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 9))
            entry(announces: true, defaults: defaults).announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
            #expect(model.whatsNew.pendingEntries.map(\.id).contains("send-feedback") == false)
            model.feedbackServices.address = { "feedback@example.com" }
            entry(announces: false, defaults: defaults).announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
            #expect(model.whatsNew.releases.first?.entries.map(\.id).contains("send-feedback") == true)
        }
        #else
        #expect(model.buildFeatures.isEmpty)
        #endif
    }

    /// The host the application builds reports what the entries ask about.
    @Test func theHostReportsItsKindAndWhetherItHasHaptics() {
        let host = AppEntry.hostDevice()
        switch host.name {
        case "iPad": #expect(host.idiom == .pad)
        case "Mac": #expect(host.idiom == .mac)
        default: #expect(host.name == "iPhone" && host.idiom == .phone)
        }
        #expect(host.hasHaptics == DeviceHaptics.isAvailable)
    }

    // MARK: The person's launch

    @Test func anUpdateFromBuildFiveAnnouncesTheNotesAndKeepsTheRecord() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 5))
            let host = host(maximumMiB: 6144)
            let model = model(on: host)
            entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
            #expect(model.whatsNew.showsRibbon)
            // Build 10's notes first, then the build 9 the person skipped.
            #expect(model.whatsNew.pending.map(\.build) == [10, 9])
            #expect(model.whatsNew.pendingEntries.map(\.id) == Self.buildTenIDsOnAnIPhone + Self.buildNineIDs)
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 5, ribbonBuild: 10, ribbonLaunches: 1))
            #expect(model.whatsNew.releases.map(\.build) == [10, 9])
        }
    }

    @Test func anUpdateFromBuildNineAnnouncesOnlyBuildTensNotes() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 9))
            let model = model(on: host(maximumMiB: 4096))
            entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
            #expect(model.whatsNew.pending.map(\.build) == [10])
            #expect(model.whatsNew.pendingEntries.map(\.id) == Self.buildTenIDsOnAnIPhone)
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 9, ribbonBuild: 10, ribbonLaunches: 1))
            // Info lists both releases.
            #expect(model.whatsNew.releases.map(\.build) == [10, 9])
        }
    }

    @Test func anInstallWithAGuestAndNoRecordIsAnUpdateFromBuildFive() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
            #expect(model.whatsNew.showsRibbon)
        }
    }

    @Test func aDeviceThatOffersNoMoreThanFourGiBIsNotToldAboutMoreMemory() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 4096))
            entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
            #expect(model.whatsNew.pendingEntries.map(\.id)
                == Self.buildTenIDsOnAnIPhone + ["file-browser", "stability-fixes"])
            // Info lists the notes as written.
            #expect(model.whatsNew.releases.map(\.entries.count) == [8, 3])
        }
    }

    @Test func aNewInstallAnnouncesNothingAndRecordsThisBuild() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announce(on: model, hasGuest: false)
            #expect(!model.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 10))
        }
    }

    @Test func buildFiveAndEarlierAnnounceNothing() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 4))
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, build: 5, defaults: defaults).announce(on: model, hasGuest: true)
            #expect(!model.whatsNew.showsRibbon)
        }
    }

    /// A build numbered 8 that bundles the notes written for builds 9 and 10
    /// offers none of them and records itself; build 9 then offers only its
    /// own, and build 10 only its own after the person has read them.
    @Test func buildEightAnnouncesNothingThenBuildNineItsOwnThenBuildTenItsOwn() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 5))
            let eight = model(on: host(maximumMiB: 6144))
            entry(announces: true, build: 8, defaults: defaults).announce(on: eight, hasGuest: true)
            #expect(!eight.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 8))
            // Info lists the notes as bundled.
            #expect(eight.whatsNew.releases.map(\.build) == [10, 9])

            let nine = model(on: host(maximumMiB: 6144))
            entry(announces: true, build: 9, defaults: defaults).announce(on: nine, hasGuest: true)
            #expect(nine.whatsNew.pending.map(\.build) == [9])
            #expect(nine.whatsNew.pendingEntries.map(\.id) == Self.buildNineIDs)
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 8, ribbonBuild: 9, ribbonLaunches: 1))
            nine.whatsNew.read()
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 9))

            let ten = model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announce(on: ten, hasGuest: true)
            #expect(ten.whatsNew.pending.map(\.build) == [10])
            #expect(ten.whatsNew.pendingEntries.map(\.id) == Self.buildTenIDsOnAnIPhone)
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 9, ribbonBuild: 10, ribbonLaunches: 1))
        }
    }

    @Test func aBuildWithoutANumberAnnouncesNothingAndKeepsNothing() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, build: nil, defaults: defaults).announce(on: model, hasGuest: true)
            #expect(!model.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == nil)
        }
    }

    @Test func readingTheNotesAndPuttingTheRibbonAwayAreKept() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 5))
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
            model.whatsNew.read()
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 10))
            #expect(model.whatsNew.showsRibbon)
            model.whatsNew.putAway()
            #expect(!model.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 10))

            // The next launch of the same build offers nothing.
            let next = self.model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announce(on: next, hasGuest: true)
            #expect(!next.whatsNew.showsRibbon)
        }
    }

    // MARK: A harness's launch

    @Test func aLaunchThatIsNotThePersonsShowsNoRibbonAndKeepsNoRecord() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 5))
            let model = model(on: host(maximumMiB: 6144))
            let launch = entry(announces: false, defaults: defaults)
            launch.announce(on: model, hasGuest: true)
            #expect(!model.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 5))
            model.whatsNew.read()
            model.whatsNew.putAway()
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 5))
            // Info still lists the notes.
            #expect(model.whatsNew.releases.map(\.build) == [10, 9])

            // The record of an install that has none stays none.
            withDefaults { fresh in
                let model = self.model(on: host(maximumMiB: 6144))
                self.entry(announces: false, defaults: fresh).announce(on: model, hasGuest: true)
                #expect(WhatsNewStore(defaults: fresh).record == nil)
            }
        }
    }

    #if !APP_STORE
    @Test func onlyALaunchWithNoSettingAndOutsideATestAnnounces() {
        func announces(_ environment: [String: String]) -> Bool {
            AppEntry.isPersonsLaunch(
                overrides: LaunchOverrides(environment: environment, processorCount: 6), environment: environment)
        }
        #expect(announces([:]))
        #expect(announces(["HOME": "/"]))
        #expect(!announces(["ANDARIUM_START": "normal"]))
        #expect(!announces(["ANDARIUM_GUEST_INSTANCE": "x"]))
        #expect(!announces(["KERNARIUM_VCPU_COUNT": "2"]))
        #expect(!announces(["ANDARIUM_STATS": "0"]))
        #expect(!announces(["ANDARIUM_JIT": "0"]))
        #expect(!announces(["XCTestConfigurationFilePath": "/tmp/x"]))
    }

    /// The process's own model is the real `makeModel`'s: Info lists the notes
    /// and the test run's launch announces none. (A test does not build a
    /// model of its own here: a `LiveRuntime` that is released takes Core
    /// Motion's device down with it, which crashes a Mac.)
    @Test func theTestRunsOwnLaunchIsAHarnessLaunch() {
        #expect(!AppEntry.fromArguments().announcesWhatsNew)
        let model = AndariumProcess.shared.model
        #expect(!model.whatsNew.showsRibbon)
        #expect(model.whatsNew.releases.map(\.build) == [10, 9])
    }
    #endif
}
