// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What the package manager answered an install with, in plain words. The
/// install stream's output ends in `Success` or `Failure [CODE: text]`.
nonisolated enum PackageInstallMessages {
    /// The message for the codes pm prints (`INSTALL_` dropped) that mean the
    /// same to the person.
    private static let table: [(codes: [String], message: String)] = [
        (
            ["PARSE_FAILED_NOT_APK", "FAILED_INVALID_APK", "PARSE_FAILED_BAD_MANIFEST",
             "PARSE_FAILED_MANIFEST_MALFORMED", "PARSE_FAILED_UNEXPECTED_EXCEPTION"],
            String(localized: "The file isn\u{2019}t a valid app.")
        ),
        (
            ["PARSE_FAILED_NO_CERTIFICATES", "PARSE_FAILED_INCONSISTENT_CERTIFICATES",
             "PARSE_FAILED_CERTIFICATE_ENCODING"],
            String(localized: "The app isn\u{2019}t signed correctly.")
        ),
        (
            ["FAILED_UPDATE_INCOMPATIBLE", "FAILED_SHARED_USER_INCOMPATIBLE"],
            String(localized: "A version of this app signed by someone else is installed. Uninstall it in the virtual machine first.")
        ),
        (["FAILED_VERSION_DOWNGRADE"], String(localized: "A newer version of this app is already installed.")),
        (["FAILED_OLDER_SDK"], String(localized: "This app needs a newer system than this virtual machine runs.")),
        (
            ["FAILED_DEPRECATED_SDK_VERSION"],
            String(localized: "This app was made for a system version that this virtual machine no longer installs.")
        ),
        (["FAILED_NO_MATCHING_ABIS"], String(localized: "This app has no code for this device\u{2019}s processor.")),
        (
            ["FAILED_INSUFFICIENT_STORAGE"],
            String(localized: "The virtual machine doesn\u{2019}t have enough free space. Free some up in the virtual machine, or give it more storage with Resize.")
        ),
        (
            ["FAILED_MISSING_SPLIT"],
            String(localized: "This app comes in several parts, which Andarium can\u{2019}t install yet.")
        ),
        (["FAILED_MISSING_SHARED_LIBRARY"], String(localized: "The app needs a system library this virtual machine doesn\u{2019}t have.")),
        (
            ["FAILED_DUPLICATE_PERMISSION", "FAILED_CONFLICTING_PROVIDER", "FAILED_DUPLICATE_PACKAGE"],
            String(localized: "The app conflicts with an app that\u{2019}s already installed.")
        ),
        (
            ["FAILED_USER_RESTRICTED", "FAILED_VERIFICATION_FAILURE", "FAILED_ABORTED"],
            String(localized: "The virtual machine refused to install the app.")
        ),
    ]

    /// The result of an install from its output: nil for success, otherwise
    /// the message for the person. Output with neither word is a failure that
    /// names nothing.
    static func failure(inOutput output: String) -> String? {
        let lines = output.split(whereSeparator: \.isNewline).map { $0.trimmingCharacters(in: .whitespaces) }
        if lines.contains(where: { $0.hasPrefix("Success") }) { return nil }
        guard let line = lines.last(where: { $0.hasPrefix("Failure") }) else {
            return String(localized: "The virtual machine couldn\u{2019}t install the app.")
        }
        guard let code = code(inFailure: line) else { return String(localized: "The virtual machine couldn\u{2019}t install the app.") }
        return message(forCode: code)
    }

    /// `Failure [INSTALL_FAILED_INVALID_APK: ...]` is `FAILED_INVALID_APK`.
    static func code(inFailure line: String) -> String? {
        guard let open = line.firstIndex(of: "[") else { return nil }
        let inside = line[line.index(after: open)...]
        let head = inside.prefix { $0 != ":" && $0 != "]" }
        let name = head.trimmingCharacters(in: .whitespaces)
        guard !name.isEmpty else { return nil }
        return name.hasPrefix("INSTALL_") ? String(name.dropFirst("INSTALL_".count)) : name
    }

    static func message(forCode code: String) -> String {
        table.first { $0.codes.contains(code) }?.message ?? String(localized: "The virtual machine couldn\u{2019}t install the app (\(code)).")
    }

    /// Every code the table maps, for the test that checks the table.
    static var mappedCodes: [String] { table.flatMap(\.codes) }
}
