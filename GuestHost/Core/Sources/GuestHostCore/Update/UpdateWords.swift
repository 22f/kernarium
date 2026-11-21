// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Every sentence of the update notice, in the application's four languages.
/// They are the application's own: from the version file only the version,
/// the build, the day and the size are shown, formatted here and nowhere else.
/// The words live in this package's catalog (`Resources/Localizable.xcstrings`)
/// so Kernarium and Andarium say them the same way; the application's name is
/// the one thing each passes in.
public nonisolated enum UpdateWords {
    // MARK: The card

    public static let cardTitle = String(localized: "Update available", bundle: .module)
    public static let cardButton = String(localized: "Get Update", bundle: .module)
    /// What VoiceOver says for the card's close button.
    public static let cardClose = String(localized: "Later", bundle: .module)

    /// "Andarium 1.1 (16) is out. You have 1.0 (15)."
    public static func cardLine(app: String, label: String, installed: String) -> String {
        String(localized: "\(app) \(label) is out. You have \(installed).", bundle: .module)
    }

    // MARK: The detail

    /// The detail's title: the application and the version, no words.
    public static func detailTitle(app: String, version: String) -> String { "\(app) \(version)" }

    public static func versionLine(_ label: String) -> String {
        String(localized: "Version \(label)", bundle: .module)
    }

    public static let released = String(localized: "Released", bundle: .module)
    public static let size = String(localized: "Size", bundle: .module)
    public static let requires = String(localized: "Requires", bundle: .module)
    public static let installed = String(localized: "Installed", bundle: .module)

    public static func requiresValue(_ os: UpdateOSVersion) -> String {
        String(localized: "iOS \(os.description) or later", bundle: .module)
    }

    /// The tool's name is a proper name and stays as it is in every language.
    public static func updateWith(_ tool: UpdateTool) -> String {
        String(localized: "Update with \(tool.name)", bundle: .module)
    }

    public static let downloadAppFile = String(localized: "Download App File", bundle: .module)
    public static let openReleasePage = String(localized: "Open Release Page", bundle: .module)

    /// What the same Apple ID keeps and another one does not.
    public static let sameAppleID = String(
        localized: "Install it with the same Apple ID and the same app you installed this one with: it replaces this version and your virtual machine stays. With another Apple ID it becomes a second app, without your virtual machine.",
        bundle: .module)

    public static let stepsTitle = String(localized: "How to update", bundle: .module)
    public static let steps = [
        String(localized: "Download the app file.", bundle: .module),
        String(localized: "Install it with the app or computer you installed this one with.", bundle: .module),
        String(localized: "Sign it with the same Apple ID.", bundle: .module),
    ]

    public static let skip = String(localized: "Skip This Version", bundle: .module)
    public static let done = String(localized: "Done", bundle: .module)

    // MARK: Info

    public static let infoHeader = String(localized: "Updates", bundle: .module)
    public static let infoRow = String(localized: "Update Available", bundle: .module)
    public static let checkSwitch = String(localized: "Check for Updates", bundle: .module)

    /// What the check reads and sends, under its switch.
    public static func checkFootnote(app: String) -> String {
        String(
            localized: "Once a day, \(app) reads one small file from where its updates are published, to see whether a newer version exists. It sends no account, identifier or usage data.",
            bundle: .module)
    }

    // MARK: Formats

    /// The size as every size in the applications is written: "1.29 GiB".
    public static func sizeText(_ version: UpdateVersion) -> String { Formats.size(bytes: version.sizeBytes) }
}
