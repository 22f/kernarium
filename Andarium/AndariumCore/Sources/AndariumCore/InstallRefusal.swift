// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import UniformTypeIdentifiers

/// A file Install APK turns away before anything is sent to the guest. The
/// words are the ones the screen shows.
public enum InstallRefusal: Equatable, Sendable {
    /// The file is not an app package.
    case notApk
    /// The file is an app in several parts (.apks, .xapk, .apkm), which the
    /// install does not take.
    case bundle
    /// The file is a single .apk of `singleApkLimit` bytes or more.
    case tooLarge

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
