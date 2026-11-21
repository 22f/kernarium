// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import GuestHostCore

private final class Anchor {}

/// The notice's words in each language of the applications. Each lookup names
/// its language, so the result does not depend on the machine's, and reads the
/// resource bundle the package ships: a language whose catalog did not make it
/// into the package fails here.
struct UpdateWordsTests {
    private static let languages = ["ja", "zh-Hans", "zh-Hant"]

    private static let resources: URL = Bundle(for: Anchor.self).bundleURL
        .deletingLastPathComponent().appendingPathComponent("GuestHostCore_GuestHostCore.bundle")

    private func text(_ value: String.LocalizationValue, in language: String) -> String {
        String(localized: LocalizedStringResource(
            value, locale: Locale(identifier: language), bundle: .atURL(Self.resources)))
    }

    @Test func theResourceBundleCarriesEveryLanguage() throws {
        let bundle = try #require(Bundle(url: Self.resources))
        let found = Set(bundle.localizations)
        for language in Self.languages { #expect(found.contains(language), "\(language) is not in \(found.sorted())") }
    }

    @Test func englishIsTheKeyAndThePlainWords() {
        #expect(UpdateWords.cardTitle == "Update available")
        #expect(UpdateWords.cardButton == "Get Update")
        #expect(UpdateWords.cardClose == "Later")
        #expect(UpdateWords.cardLine(app: "Andarium", label: "1.1 (16)", installed: "1.0 (15)")
            == "Andarium 1.1 (16) is out. You have 1.0 (15).")
        #expect(UpdateWords.detailTitle(app: "Andarium", version: "1.1") == "Andarium 1.1")
        #expect(UpdateWords.versionLine("1.1 (16)") == "Version 1.1 (16)")
        #expect(UpdateWords.requiresValue(UpdateOSVersion(major: 26)) == "iOS 26.0 or later")
        #expect(UpdateWords.updateWith(.altStore) == "Update with AltStore")
        #expect(UpdateWords.updateWith(.sideStore) == "Update with SideStore")
        #expect(UpdateWords.downloadAppFile == "Download App File")
        #expect(UpdateWords.openReleasePage == "Open Release Page")
        #expect(UpdateWords.skip == "Skip This Version")
        #expect(UpdateWords.infoHeader == "Updates")
        #expect(UpdateWords.infoRow == "Update Available")
        #expect(UpdateWords.checkSwitch == "Check for Updates")
        #expect(UpdateWords.checkFootnote(app: "Andarium").hasPrefix("Once a day, Andarium reads one small file"))
        #expect(UpdateWords.steps.count == 3)
    }

    @Test func everyAppSentenceSaysVirtualMachineNeverGuest() {
        #expect(UpdateWords.sameAppleID.contains("virtual machine"))
        for sentence in [UpdateWords.sameAppleID, UpdateWords.checkFootnote(app: "Andarium")] {
            #expect(!sentence.lowercased().contains("guest"))
        }
    }

    @Test func theSizeIsWrittenAsEverySizeIs() {
        let version = UpdateVersion(
            version: "1.1", build: 16, released: UpdateDay(year: 2026, month: 11, day: 20), sizeBytes: 1_390_000_000,
            appFile: URL(string: "https://example.com/a.ipa")!)
        #expect(UpdateWords.sizeText(version) == "1.29 GiB")
    }

    @Test(arguments: UpdateWordsTests.languages)
    func theCardIsTranslated(language: String) {
        for word in ["Update available", "Get Update", "Later", "Skip This Version", "Done", "Updates", "Update Available", "Check for Updates"] as [String.LocalizationValue] {
            let english = text(word, in: "en")
            #expect(text(word, in: language) != english, "\(language) says \"\(english)\" in English")
        }
    }

    @Test func theCardLineKeepsItsThreeValuesInEveryLanguage() {
        let line = { (language: String) in
            self.text("\("Andarium") \("1.1 (16)") is out. You have \("1.0 (15)").", in: language)
        }
        #expect(line("en") == "Andarium 1.1 (16) is out. You have 1.0 (15).")
        #expect(line("ja") == "Andarium 1.1 (16)が公開されました。現在のバージョンは1.0 (15)です。")
        #expect(line("zh-Hans") == "Andarium 1.1 (16)已推出。你当前的版本为1.0 (15)。")
        #expect(line("zh-Hant") == "Andarium 1.1 (16)已推出。你目前的版本為1.0 (15)。")
    }

    @Test(arguments: UpdateWordsTests.languages)
    func theOtherSentencesKeepTheirValues(language: String) {
        let version = text("Version \("1.1 (16)")", in: language)
        #expect(version.contains("1.1 (16)"))
        let ios = text("iOS \("26.0") or later", in: language)
        #expect(ios.contains("iOS 26.0"))
        let tool = text("Update with \("AltStore")", in: language)
        #expect(tool.contains("AltStore"))
        let footnote = text(
            "Once a day, \("Andarium") reads one small file from where its updates are published, to see whether a newer version exists. It sends no account, identifier or usage data.",
            in: language)
        #expect(footnote.contains("Andarium"))
        #expect(!footnote.contains("%@"))
        // Chinese and Japanese write no space between their text and a Latin name or a number.
        for sentence in [version, ios, tool, footnote] {
            #expect(!sentence.contains(" Andarium"), "\(sentence)")
        }
    }
}
