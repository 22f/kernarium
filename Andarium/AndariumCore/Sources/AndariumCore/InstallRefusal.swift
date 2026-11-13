// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import UniformTypeIdentifiers

/// A file Install APK turns away before anything is sent to the guest. The
/// words are the ones the screen shows.
public enum InstallRefusal: Error, Equatable, Sendable {
    /// The file is not an app package.
    case notApk
    /// The file is an app in several parts (.apks, .xapk, .apkm), which the
    /// install does not take.
    case bundle
    /// The file is a single .apk of `singleApkLimit` bytes or more.
    case tooLarge
    /// The file, or an entry of it, is encrypted or password-protected.
    case encrypted
    /// The file is an app bundle for publishing, which is not installable.
    case appBundle
    /// The archive holds no app.
    case noApp
    /// The archive is damaged, or its parts do not fit together.
    case damaged
    /// The app has code for no processor this virtual machine has.
    case noMatchingProcessor

    /// From this size a single .apk is a ZIP64 archive, which Android's
    /// package parser cannot read.
    public static let singleApkLimit: UInt64 = 4 * Bytes.gib

    /// nil for a file the install takes: one that ends in .apk.
    public init?(fileName: String) {
        let name = fileName.lowercased()
        if name.hasSuffix(".apk") { return nil }
        if [".apks", ".xapk", ".apkm"].contains(where: { name.hasSuffix($0) }) {
            self = .bundle
        } else {
            self = .notApk
        }
    }

    /// nil for a file whose size Android takes.
    public init?(sizeBytes: UInt64) {
        guard sizeBytes >= Self.singleApkLimit else { return nil }
        self = .tooLarge
    }

    public var message: String {
        switch self {
        case .notApk: String(localized: "This isn\u{2019}t an app. Choose a file that ends in .apk.", bundle: .module)
        case .bundle: String(localized: "Apps that come in several files can\u{2019}t be installed yet. Choose a single .apk file.", bundle: .module)
        case .tooLarge:
            String(
                localized: "Android can\u{2019}t install an app from a single .apk file of \(Formats.size(bytes: Self.singleApkLimit)) or more.",
                bundle: .module)
        case .encrypted:
            String(localized: "This file is encrypted, so it can\u{2019}t be installed. Choose an app file that isn\u{2019}t encrypted.", bundle: .module)
        case .appBundle:
            String(
                localized: "This is an app bundle for publishing apps, which can\u{2019}t be installed. Choose an .apk, .apks, .xapk or .apkm file.",
                bundle: .module)
        case .noApp:
            String(localized: "There\u{2019}s no app in this file. Choose an .apk, .apks, .xapk or .apkm file that holds an app.", bundle: .module)
        case .damaged:
            String(
                localized: "This file is damaged, or the parts of the app don\u{2019}t fit together. Download it again, or choose another file.",
                bundle: .module)
        case .noMatchingProcessor:
            String(localized: "This app has no code for this device\u{2019}s processor.", bundle: .module)
        }
    }
}

extension InstallRefusal {
    /// The one type the file picker offers: what the system calls a file
    /// that ends in .apk. No Info.plist declaration is needed. With none, the
    /// type is the dynamic one every .apk file also gets; if the system or
    /// another application declares .apk, the call returns that type and the
    /// files carry it too, so the picker and the files always agree. Other
    /// extensions (.apks, .xapk, .apkm, .zip, none) are dimmed, and
    /// `init(fileName:)` still refuses a mislabelled file.
    public static var pickerType: UTType {
        UTType(filenameExtension: "apk", conformingTo: .data) ?? .data
    }
}
