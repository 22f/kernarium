// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
import UIKit
@testable import Andarium

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

    private func host(maximumMiB: Int) -> HostDevice {
        HostDevice(
            name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [maximumMiB]),
            freeStorageBytes: 40_000_000_000)
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

    private static let buildTenIDs = [
        "snapshots", "languages", "resolution", "keyboard-mouse-controllers", "location", "send-feedback",
        "more-fixes",
    ]
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

    @Test func everyEntryHasWordsASymbolAndARequirementTheCodeKnows() throws {
        for release in WhatsNewCatalog.load() {
            #expect(!release.version.isEmpty)
            for entry in release.entries {
                #expect(!entry.title.isEmpty && (entry.text != nil || !entry.points.isEmpty), "\(entry.id)")
                #expect(UIImage(systemName: entry.symbol) != nil, "\(entry.id): \(entry.symbol)")
                // A requirement the code does not know fails to decode, so the
                // entry would not be here; the memory one is the notes' own.
                if let requirement = entry.requirement { #expect(requirement.memoryAboveMiB != nil) }
            }
        }
        let memory = try #require(WhatsNewCatalog.load().flatMap(\.entries).first { $0.id == "more-memory" })
        #expect(memory.requirement == WhatsNewRequirement(memoryAboveMiB: 4096))
    }

    @Test func theRibbonNamesTheBundledEntriesAndCountsTheRest() throws {
        let titles = try #require(WhatsNewCatalog.load().first).entries.map(\.title)
        #expect(titles == [
            "Snapshots", "Languages", "Resolution", "Keyboard, mouse and controllers", "Location", "Send feedback",
            "More fixes",
        ])
        #expect(WhatsNewRibbon.summary(titles, naming: 7) == titles.joined(separator: " \u{00B7} "))
        #expect(WhatsNewRibbon.summary(titles, naming: 2) == "Snapshots \u{00B7} Languages \u{00B7} 5 more")
        #expect(WhatsNewRibbon.summary(titles, naming: 0) == "7 new features")
    }

    // MARK: The person's launch

    @Test func anUpdateFromBuildFiveAnnouncesTheNotesAndKeepsTheRecord() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 5))
            let host = host(maximumMiB: 6144)
            let model = model(on: host)
            entry(announces: true, defaults: defaults).announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
            #expect(model.whatsNew.showsRibbon)
            // Build 10's notes first, then the build 9 the person skipped.
            #expect(model.whatsNew.pending.map(\.build) == [10, 9])
            #expect(model.whatsNew.pendingEntries.map(\.id) == Self.buildTenIDs + Self.buildNineIDs)
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 5, ribbonBuild: 10, ribbonLaunches: 1))
            #expect(model.whatsNew.releases.map(\.build) == [10, 9])
        }
    }

    @Test func anUpdateFromBuildNineAnnouncesOnlyBuildTensNotes() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 9))
            let model = model(on: host(maximumMiB: 4096))
            entry(announces: true, defaults: defaults).announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
            #expect(model.whatsNew.pending.map(\.build) == [10])
            #expect(model.whatsNew.pendingEntries.map(\.id) == Self.buildTenIDs)
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 9, ribbonBuild: 10, ribbonLaunches: 1))
            // Info lists both releases.
            #expect(model.whatsNew.releases.map(\.build) == [10, 9])
        }
    }

    @Test func anInstallWithAGuestAndNoRecordIsAnUpdateFromBuildFive() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
            #expect(model.whatsNew.showsRibbon)
        }
    }

    @Test func aDeviceThatOffersNoMoreThanFourGiBIsNotToldAboutMoreMemory() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 4096))
            entry(announces: true, defaults: defaults).announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
            #expect(model.whatsNew.pendingEntries.map(\.id)
                == Self.buildTenIDs + ["file-browser", "stability-fixes"])
            // Info lists the notes as written.
            #expect(model.whatsNew.releases.map(\.entries.count) == [7, 3])
        }
    }

    @Test func aNewInstallAnnouncesNothingAndRecordsThisBuild() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announceWhatsNew(on: model, hasGuest: false, vCPUs: 6)
            #expect(!model.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 10))
        }
    }

    @Test func buildFiveAndEarlierAnnounceNothing() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 4))
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, build: 5, defaults: defaults).announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
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
            entry(announces: true, build: 8, defaults: defaults).announceWhatsNew(on: eight, hasGuest: true, vCPUs: 6)
            #expect(!eight.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 8))
            // Info lists the notes as bundled.
            #expect(eight.whatsNew.releases.map(\.build) == [10, 9])

            let nine = model(on: host(maximumMiB: 6144))
            entry(announces: true, build: 9, defaults: defaults).announceWhatsNew(on: nine, hasGuest: true, vCPUs: 6)
            #expect(nine.whatsNew.pending.map(\.build) == [9])
            #expect(nine.whatsNew.pendingEntries.map(\.id) == Self.buildNineIDs)
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 8, ribbonBuild: 9, ribbonLaunches: 1))
            nine.whatsNew.read()
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 9))

            let ten = model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announceWhatsNew(on: ten, hasGuest: true, vCPUs: 6)
            #expect(ten.whatsNew.pending.map(\.build) == [10])
            #expect(ten.whatsNew.pendingEntries.map(\.id) == Self.buildTenIDs)
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 9, ribbonBuild: 10, ribbonLaunches: 1))
        }
    }

    @Test func aBuildWithoutANumberAnnouncesNothingAndKeepsNothing() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, build: nil, defaults: defaults).announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
            #expect(!model.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == nil)
        }
    }

    @Test func readingTheNotesAndPuttingTheRibbonAwayAreKept() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 5))
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
            model.whatsNew.read()
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 10))
            #expect(model.whatsNew.showsRibbon)
            model.whatsNew.putAway()
            #expect(!model.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 10))

            // The next launch of the same build offers nothing.
            let next = self.model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announceWhatsNew(on: next, hasGuest: true, vCPUs: 6)
            #expect(!next.whatsNew.showsRibbon)
        }
    }

    // MARK: A harness's launch

    @Test func aLaunchThatIsNotThePersonsShowsNoRibbonAndKeepsNoRecord() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 5))
            let model = model(on: host(maximumMiB: 6144))
            let launch = entry(announces: false, defaults: defaults)
            launch.announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
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
                self.entry(announces: false, defaults: fresh).announceWhatsNew(on: model, hasGuest: true, vCPUs: 6)
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
