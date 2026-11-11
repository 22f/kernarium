// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// The application ships its String Catalogs in every language. Each lookup
/// names its language, so the result does not depend on the language of the
/// device or Simulator the tests run on, and each reads the bundle the
/// application ships. `tests/tooling/andarium` checks the catalogs' contents;
/// these show the build carries them.
struct LocalizationTests {
    private static let languages = ["ja", "zh-Hans", "zh-Hant"]

    private func text(_ key: String.LocalizationValue, table: String? = nil, in language: String) -> String {
        String(localized: LocalizedStringResource(
            key, table: table, locale: Locale(identifier: language), bundle: .main))
    }

    /// The scheme's Test action pins English, so the tests that read the words
    /// the application says do not depend on the language of the device. When
    /// this fails, so will they: the pin is gone or not taken.
    @Test func theTestsRunInEnglish() {
        #expect(Bundle.main.preferredLocalizations.first == "en", "\(Bundle.main.preferredLocalizations)")
        #expect(Locale.preferredLanguages.first?.hasPrefix("en") == true, "\(Locale.preferredLanguages)")
    }

    @Test func theApplicationShipsEveryLanguage() {
        let shipped = Set(Bundle.main.localizations)
        for language in ["en"] + Self.languages {
            #expect(shipped.contains(language), "\(language) is not in \(shipped.sorted())")
        }
    }

    @Test(arguments: LocalizationTests.languages)
    func aScreenIsTranslated(language: String) {
        #expect(text("Virtual machine", in: "en") == "Virtual machine")
        #expect(text("Virtual machine", in: language) != "Virtual machine", "\(language) says it in English")
    }

    @Test func aCountIsPluralInEnglishAndInOneFormInTheOthers() {
        #expect(text("\(1) snapshots", in: "en") == "1 snapshot")
        #expect(text("\(3) snapshots", in: "en") == "3 snapshots")
        for language in Self.languages {
            let one = text("\(1) snapshots", in: language), three = text("\(3) snapshots", in: language)
            #expect(one.contains("1") && three.contains("3"), "\(language): \(one), \(three)")
            #expect(one.replacingOccurrences(of: "1", with: "3") == three, "\(language) has a form for one")
        }
    }

    @Test(arguments: LocalizationTests.languages)
    func everyNoteOfTheReleasesIsTranslated(language: String) throws {
        let releases = WhatsNewCatalog.load()
        let notes = releases.flatMap(\.entries).flatMap { entry in
            [entry.title] + [entry.text].compactMap { $0 } + entry.points
        }
        try #require(!notes.isEmpty)
        for note in notes {
            let translated = text(String.LocalizationValue(note), table: "WhatsNew", in: language)
            #expect(translated != note, "\(language) shows \"\(note)\" in English")
        }
    }

    /// Each permission text of the Info.plist reads differently in each
    /// language: the Info.plist's own value is the English text, and the
    /// language's `InfoPlist.strings` must hold another.
    @Test(arguments: LocalizationTests.languages)
    func thePermissionTextsAreTranslated(language: String) throws {
        let path = try #require(Bundle.main.path(
            forResource: "InfoPlist", ofType: "strings", inDirectory: nil, forLocalization: language))
        let strings = try #require(NSDictionary(contentsOfFile: path) as? [String: String])
        let info = try #require(Bundle.main.infoDictionary)
        let keys = info.keys.filter { $0.hasPrefix("NS") && $0.hasSuffix("UsageDescription") }.sorted()
        try #require(!keys.isEmpty)
        for key in keys {
            let english = try #require(info[key] as? String, "\(key) is no text")
            let value = try #require(strings[key], "\(language) has no \(key)")
            #expect(!value.isEmpty && value != english, "\(language) shows \(key) in English")
        }
    }
}
