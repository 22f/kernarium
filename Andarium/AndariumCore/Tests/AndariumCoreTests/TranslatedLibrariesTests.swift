// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// Home's "Translated system libraries": the default, how the choice is kept,
/// what a start states to the emulator and what the session record says of it.
struct TranslatedLibrariesTests {
    private func withDefaults<Result>(_ body: (UserDefaults) -> Result) -> Result {
        let name = "andarium-translated-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defer { defaults.removePersistentDomain(forName: name) }
        return body(defaults)
    }

    // MARK: The option

    @Test func aFreshInstallAndAnUpdateStartWithTheLibrariesOn() {
        withDefaults { defaults in
            // Neither has a value stored: the key is new in this version.
            #expect(defaults.object(forKey: TranslatedLibrariesPreference.key) == nil)
            let preference = TranslatedLibrariesPreference(defaults: defaults)
            #expect(preference.isOn)
            let start = preference.launch(automated: false)
            #expect(start.isOn && start.keepsChanges)
        }
    }

    @Test func theChoiceSurvivesANewProcessUntilTurnedBackOn() {
        withDefaults { defaults in
            TranslatedLibrariesPreference(defaults: defaults).keep(false)
            let again = TranslatedLibrariesPreference(defaults: defaults)
            #expect(!again.isOn)
            let person = again.launch(automated: false)
            #expect(!person.isOn && person.keepsChanges)
            again.keep(true)
            #expect(TranslatedLibrariesPreference(defaults: defaults).isOn)
        }
    }

    @Test func aHarnessLaunchStartsOnAndLeavesWhatThePersonChoseAlone() {
        withDefaults { defaults in
            let preference = TranslatedLibrariesPreference(defaults: defaults)
            preference.keep(false)
            let harness = preference.launch(automated: true)
            #expect(harness.isOn && !harness.keepsChanges)
            #expect(!preference.isOn, "a launch changed what the person chose")
        }
    }

    // MARK: What a start states

    @Test func aStartStatesTheReleaseOnlyWhileTheOptionIsOnInABuildThatHasIt() {
        for jit in [false, true] {
            // The option is on: the release is stated, and the emulator decides
            // whether the build's artifact fits it.
            #expect(TranslatedLibrariesStart(buildCarries: true, optionOn: true, jitInUse: jit).statesRelease)
            // Off, the launch leaves it out.
            #expect(!TranslatedLibrariesStart(buildCarries: true, optionOn: false, jitInUse: jit).statesRelease)
            // A build without the framework launches as it always did, whatever
            // value an earlier build left stored.
            #expect(TranslatedLibrariesStart(buildCarries: false, optionOn: true, jitInUse: jit).statesRelease)
            #expect(TranslatedLibrariesStart(buildCarries: false, optionOn: false, jitInUse: jit).statesRelease)
        }
    }

    @Test func theRecordSaysWhatTheStartDid() {
        #expect(TranslatedLibrariesStart(buildCarries: true, optionOn: true).word == .on)
        #expect(TranslatedLibrariesStart(buildCarries: true, optionOn: false).word == .off)
        // The JIT does not use them whatever the option says.
        #expect(TranslatedLibrariesStart(buildCarries: true, optionOn: true, jitInUse: true).word == .notUsedWithJit)
        #expect(TranslatedLibrariesStart(buildCarries: true, optionOn: false, jitInUse: true).word == .notUsedWithJit)
        #expect(TranslatedLibrariesStart(buildCarries: false, optionOn: true).word == .unavailable)
        #expect(Set(TranslatedLibrariesStart.Word.allCases.map(\.rawValue))
            == ["on", "off", "not-used-with-jit", "unavailable"])
    }

    // MARK: The start record

    private func scratch() throws -> URL {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-start-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }

    @Test func theStartRecordReadsBackAndHoldsOnlyItsFields() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        let url = directory.appendingPathComponent(BugReportAttachments.startRecord)
        for word in TranslatedLibrariesStart.Word.allCases {
            #expect(StartRecord(translatedLibraries: word).write(to: url))
            #expect(StartRecord.read(at: url) == StartRecord(translatedLibraries: word))
        }
        #expect(StartRecord(translatedLibraries: .off).text == "uml-start 1\ntranslated-libraries off\n")
    }

    @Test func aFileThatIsNotAStartRecordIsNotOne() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        #expect(StartRecord(text: "") == nil)
        #expect(StartRecord(text: "uml-start 1\n") == nil)
        #expect(StartRecord(text: "uml-start 1\ntranslated-libraries maybe\n") == nil)
        #expect(StartRecord(text: "uml-start 2\ntranslated-libraries on\n") == nil)
        let long = directory.appendingPathComponent("long.txt")
        try (StartRecord(translatedLibraries: .on).text + String(repeating: "x", count: 4096))
            .write(to: long, atomically: true, encoding: .utf8)
        #expect(StartRecord.read(at: long) == nil)
        #expect(StartRecord.read(at: directory.appendingPathComponent("missing.txt")) == nil)
        // A field a later version adds does not hide the one this reads.
        #expect(StartRecord(text: "uml-start 1\nother 1\ntranslated-libraries off\n")?.translatedLibraries == .off)
    }

    @Test func aRecordThatCannotBeWrittenSaysSoAndCostsNothingElse() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        let missing = directory.appendingPathComponent("absent").appendingPathComponent("start.txt")
        #expect(!StartRecord(translatedLibraries: .on).write(to: missing))
    }

    @Test func theNextLaunchKeepsTheStartRecordWithTheSessionRecord() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        try "uml-session 1\nstate ended\nend powered-off\nstatus 0\npid 7\nstarted 1\nended 2\n".write(
            to: directory.appendingPathComponent(BugReportAttachments.sessionRecord), atomically: true, encoding: .utf8)
        StartRecord(translatedLibraries: .off).write(to: directory.appendingPathComponent(BugReportAttachments.startRecord))
        #expect(PreviousSession.consume(in: directory, endedOutOfForeground: false) != nil)
        #expect(StartRecord.read(at: directory.appendingPathComponent(BugReportAttachments.startRecord)) == nil)
        #expect(
            StartRecord.read(at: directory.appendingPathComponent(BugReportAttachments.previousStartRecord))?
                .translatedLibraries == .off)
        // A second launch with no new start says nothing more and keeps it.
        #expect(PreviousSession.consume(in: directory, endedOutOfForeground: false) == nil)
        #expect(
            StartRecord.read(at: directory.appendingPathComponent(BugReportAttachments.previousStartRecord)) != nil)
    }

    @Test func theBugReportCopiesBothStartRecordsNextToTheSessionRecords() throws {
        let directory = try scratch()
        defer { try? FileManager.default.removeItem(at: directory) }
        // No record: the report adds nothing.
        #expect(BugReportAttachments.entries(from: directory, containers: []).isEmpty)
        StartRecord(translatedLibraries: .on).write(
            to: directory.appendingPathComponent(BugReportAttachments.previousStartRecord))
        StartRecord(translatedLibraries: .notUsedWithJit).write(
            to: directory.appendingPathComponent(BugReportAttachments.startRecord))
        try "uml-session 1\nstate running\nend running\n".write(
            to: directory.appendingPathComponent(BugReportAttachments.sessionRecord), atomically: true, encoding: .utf8)
        let entries = BugReportAttachments.entries(from: directory, containers: [])
        #expect(entries.map(\.name) == [
            "andarium/session-current.txt", "andarium/start-previous.txt", "andarium/start-current.txt",
        ])
        #expect(String(decoding: entries[1].data, as: UTF8.self) == "uml-start 1\ntranslated-libraries on\n")
        #expect(String(decoding: entries[2].data, as: UTF8.self) == "uml-start 1\ntranslated-libraries not-used-with-jit\n")
        // Text that is not a start record is not copied.
        try "anything".write(
            to: directory.appendingPathComponent(BugReportAttachments.startRecord), atomically: true, encoding: .utf8)
        #expect(!BugReportAttachments.entries(from: directory, containers: []).contains {
            $0.name == "andarium/start-current.txt"
        })
    }
}
