// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

// The values the update notice keeps of a version file. The file is untrusted
// input: each type is made only by an initializer that checks its text, so a
// value that exists has passed the rules, and nothing of the file is shown
// that did not come through one.

/// An iOS version as the file writes it ("26.0", "26.2.1"): one to three
/// numbers, compared as dotted numbers with the missing ones as zero.
public struct UpdateOSVersion: Comparable, Hashable, Codable, Sendable, CustomStringConvertible {
    public let major: Int
    public let minor: Int
    public let patch: Int

    public init(major: Int, minor: Int = 0, patch: Int = 0) {
        self.major = major
        self.minor = minor
        self.patch = patch
    }

    /// nil for text that is not one to three numbers of at most four digits.
    public init?(_ text: String) {
        let parts = text.split(separator: ".", omittingEmptySubsequences: false)
        guard (1...3).contains(parts.count) else { return nil }
        var numbers = [0, 0, 0]
        for (index, part) in parts.enumerated() {
            guard (1...4).contains(part.utf8.count), part.utf8.allSatisfy({ $0 >= 0x30 && $0 <= 0x39 }),
                let value = Int(part)
            else { return nil }
            numbers[index] = value
        }
        self.init(major: numbers[0], minor: numbers[1], patch: numbers[2])
    }

    public static func < (lhs: Self, rhs: Self) -> Bool {
        (lhs.major, lhs.minor, lhs.patch) < (rhs.major, rhs.minor, rhs.patch)
    }

    /// "26.0", or "26.2.1" with a patch number.
    public var description: String {
        patch == 0 ? "\(major).\(minor)" : "\(major).\(minor).\(patch)"
    }

    /// The system's version.
    public static var current: UpdateOSVersion {
        let version = ProcessInfo.processInfo.operatingSystemVersion
        return UpdateOSVersion(major: version.majorVersion, minor: version.minorVersion, patch: version.patchVersion)
    }
}

/// A release day as a calendar date, the day the file's author wrote. A
/// time and zone after it are the author's own and are not applied.
public struct UpdateDay: Hashable, Codable, Sendable {
    public let year: Int
    public let month: Int
    public let day: Int

    public init(year: Int, month: Int, day: Int) {
        self.year = year
        self.month = month
        self.day = day
    }

    /// The two forms of ISO 8601 that AltStore and SideStore read, read
    /// strictly: a full date ("2026-11-20"), or a full date with a time and a
    /// zone ("2026-11-20T09:30:00Z", "2026-11-20T09:30:00+08:00", an optional
    /// fraction of a second included). nil for anything else, a real calendar
    /// day or not.
    public init?(_ text: String) {
        let bytes = Array(text.utf8)
        func number(_ range: Range<Int>) -> Int? {
            guard range.upperBound <= bytes.count else { return nil }
            var value = 0
            for byte in bytes[range] {
                guard byte >= 0x30 && byte <= 0x39 else { return nil }
                value = value * 10 + Int(byte - 0x30)
            }
            return value
        }
        guard bytes.count >= 10, bytes[4] == 0x2D, bytes[7] == 0x2D,
            let year = number(0..<4), let month = number(5..<7), let day = number(8..<10)
        else { return nil }
        if bytes.count > 10 {
            // T, hh:mm:ss, an optional fraction, then Z or +hh:mm or -hh:mm (or +hhmm).
            guard bytes.count <= 40, bytes[10] == 0x54, bytes.count >= 20, bytes[13] == 0x3A, bytes[16] == 0x3A,
                let hour = number(11..<13), let minute = number(14..<16), let second = number(17..<19),
                hour <= 23, minute <= 59, second <= 59
            else { return nil }
            var index = 19
            if bytes[index] == 0x2E {
                index += 1
                let start = index
                while index < bytes.count, bytes[index] >= 0x30, bytes[index] <= 0x39 { index += 1 }
                guard index > start else { return nil }
            }
            guard index < bytes.count else { return nil }
            if bytes[index] == 0x5A {
                guard index == bytes.count - 1 else { return nil }
            } else if bytes[index] == 0x2B || bytes[index] == 0x2D {
                let zone = bytes.count - index - 1
                guard let offsetHour = number((index + 1)..<(index + 3)), offsetHour <= 23 else { return nil }
                if zone == 5 {
                    guard bytes[index + 3] == 0x3A, let offsetMinute = number((index + 4)..<(index + 6)), offsetMinute <= 59
                    else { return nil }
                } else if zone == 4 {
                    guard let offsetMinute = number((index + 3)..<(index + 5)), offsetMinute <= 59 else { return nil }
                } else {
                    return nil
                }
            } else {
                return nil
            }
        }
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = Self.zone
        guard (1...12).contains(month), (1...31).contains(day),
            let date = calendar.date(from: DateComponents(year: year, month: month, day: day, hour: 12)),
            calendar.dateComponents([.year, .month, .day], from: date) == DateComponents(year: year, month: month, day: day)
        else { return nil }
        self.init(year: year, month: month, day: day)
    }

    private static let zone = TimeZone(secondsFromGMT: 0)!

    /// The day in the reader's language and region, as "20 November 2026".
    public func formatted(locale: Locale = .autoupdatingCurrent) -> String {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = Self.zone
        guard let date = calendar.date(from: DateComponents(year: year, month: month, day: day, hour: 12)) else {
            return ""
        }
        return date.formatted(
            Date.FormatStyle(date: .long, time: .omitted, locale: locale, calendar: calendar, timeZone: Self.zone))
    }
}

/// One version of the application that the file lists, after every check.
/// The file's other words (names, descriptions, news) are never kept.
public struct UpdateVersion: Hashable, Codable, Sendable {
    /// What a person sees as the version: at most 16 letters, digits and dots.
    public let version: String
    /// The comparison: a positive whole number, the application's `CFBundleVersion`.
    public let build: Int
    public let released: UpdateDay
    public let sizeBytes: UInt64
    /// The lowest and the highest iOS the version runs on, where the file says.
    public let minimumOS: UpdateOSVersion?
    public let maximumOS: UpdateOSVersion?
    /// The application's file (`.ipa`): `https`, on a host the build accepts.
    public let appFile: URL
    /// The release page, where the file's extra block names one that passes.
    public let notes: URL?

    public init(
        version: String, build: Int, released: UpdateDay, sizeBytes: UInt64,
        minimumOS: UpdateOSVersion? = nil, maximumOS: UpdateOSVersion? = nil, appFile: URL, notes: URL? = nil
    ) {
        self.version = version
        self.build = build
        self.released = released
        self.sizeBytes = sizeBytes
        self.minimumOS = minimumOS
        self.maximumOS = maximumOS
        self.appFile = appFile
        self.notes = notes
    }

    /// The same version without its release page.
    public var withoutNotes: UpdateVersion {
        UpdateVersion(
            version: version, build: build, released: released, sizeBytes: sizeBytes,
            minimumOS: minimumOS, maximumOS: maximumOS, appFile: appFile)
    }

    /// "1.1 (16)": how Info writes a version.
    public var label: String { "\(version) (\(build))" }

    /// A version string the file may carry: 1 to 16 ASCII letters, digits and dots.
    public static func isWellFormed(version text: String) -> Bool {
        (1...16).contains(text.utf8.count)
            && text.utf8.allSatisfy { byte in
                (byte >= 0x30 && byte <= 0x39) || (byte >= 0x41 && byte <= 0x5A)
                    || (byte >= 0x61 && byte <= 0x7A) || byte == 0x2E
            }
    }

    /// The build number as text: one to nine ASCII digits, above zero.
    public static func build(from text: String) -> Int? {
        guard (1...9).contains(text.utf8.count), text.utf8.allSatisfy({ $0 >= 0x30 && $0 <= 0x39 }),
            let value = Int(text), value > 0
        else { return nil }
        return value
    }

    /// The largest size a file may state: one tebibyte.
    public static let largestSizeBytes: Int64 = 1 << 40
}
