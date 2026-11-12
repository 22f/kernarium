// SPDX-License-Identifier: GPL-2.0-or-later

#if TIP_JAR

import Foundation

// The developer the tip sheet's header shows: a name, and the GitHub and X
// profiles its two logo buttons open. The values are local to the maker's
// build (`Config/Developer.local.xcconfig`, which Git ignores) and reach the
// product only in an AppStore build that has Buy me tokens, as Info.plist
// keys; the code builds the addresses from the handles, so no address is
// ever written in a file. Plain values and rules, no UIKit or SwiftUI.

nonisolated struct TipJarDeveloper: Equatable, Sendable {
    /// Shown as it is, in every language.
    var name: String?
    var githubURL: URL?
    var xURL: URL?

    init(name: String? = nil, githubURL: URL? = nil, xURL: URL? = nil) {
        self.name = name
        self.githubURL = githubURL
        self.xURL = xURL
    }

    /// The developer a build names; every piece is nil that is absent or not well formed.
    init(name: String?, githubHandle: String?, xHandle: String?) {
        self.init(
            name: name.flatMap(Self.displayName),
            githubURL: githubHandle.flatMap(Self.githubURL),
            xURL: xHandle.flatMap(Self.xURL))
    }

    /// A name shown as a person wrote it: not empty after trimming, no control character, at most 60.
    static func displayName(_ raw: String) -> String? {
        let text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty, text.count <= 60,
              text.unicodeScalars.allSatisfy({ !CharacterSet.controlCharacters.contains($0) }) else { return nil }
        return text
    }

    /// A GitHub account: 1 to 39 letters, digits and hyphens, none at either end.
    static func githubURL(handle: String) -> URL? {
        let bytes = Array(handle.utf8)
        guard (1...39).contains(bytes.count), bytes.first != 0x2D, bytes.last != 0x2D,
              bytes.allSatisfy({ isAlphanumeric($0) || $0 == 0x2D }) else { return nil }
        return URL(string: "https://github.com/\(handle)")
    }

    /// An X account: 1 to 15 letters, digits and underscores.
    static func xURL(handle: String) -> URL? {
        let bytes = Array(handle.utf8)
        guard (1...15).contains(bytes.count), bytes.allSatisfy({ isAlphanumeric($0) || $0 == 0x5F }) else { return nil }
        return URL(string: "https://x.com/\(handle)")
    }

    private static func isAlphanumeric(_ byte: UInt8) -> Bool {
        (byte >= 0x41 && byte <= 0x5A) || (byte >= 0x61 && byte <= 0x7A) || (byte >= 0x30 && byte <= 0x39)
    }
}

/// Where the developer's values and artwork are read from.
nonisolated enum TipJarDeveloperConfiguration {
    /// The Info.plist keys the build writes the values to, each only where its
    /// value is set (`Config/Andarium.xcconfig`).
    static let nameKey = "AndariumDeveloperName"
    static let githubKey = "AndariumDeveloperGitHub"
    static let xKey = "AndariumDeveloperX"
    /// The folder of the product's resources that the artwork is copied to
    /// (`scripts/copy-developer-artwork.py`), and the name of each file in it.
    static let folder = "DeveloperArtwork"
    static let avatarFile = "avatar"
    static let githubFile = "github"
    static let xFile = "x"

    static var developer: TipJarDeveloper {
        func string(_ key: String) -> String? { Bundle.main.object(forInfoDictionaryKey: key) as? String }
        return TipJarDeveloper(name: string(nameKey), githubHandle: string(githubKey), xHandle: string(xKey))
    }

    /// The path of a file of the artwork, if the product has it.
    static func artworkPath(_ file: String) -> String? {
        Bundle.main.path(forResource: file, ofType: nil, inDirectory: folder)
    }
}

#endif
