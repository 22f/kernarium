// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The root part of a bug report: one small text file inside Android's report,
/// so that whoever reads a report can tell a rooted virtual machine from a
/// stock one before looking at anything else. It holds no identifier, path or
/// package list.
public enum RootReport {
    /// The entry's name in the report's archive.
    public static let name = "andarium/root.txt"

    public static func entry(state: RootState, release: RootRelease?) -> BugReportArchive.Entry {
        .init(name: name, data: Data(text(state: state, release: release).utf8))
    }

    public static func text(state: RootState, release: RootRelease?) -> String {
        var lines = [
            "root switch: \(state.on ? "on" : "off")",
            "last start booted the Magisk ramdisk: \(state.rooted ? "yes" : "no")",
        ]
        switch state.attention {
        case .failedStart?: lines.append("paused: a start with root ended before Android booted")
        case .turnedOffAfterFailure?: lines.append("turned off: a start with root ended before Android booted")
        case .stoppedStart?: lines.append("a start with root was stopped before Android booted")
        case nil: break
        }
        lines.append("the release carries root: \(release.map { "Magisk \($0.versionName) (\($0.versionCode))" } ?? "no")")
        lines.append(
            "Magisk app version code the application recorded: \(state.appVersionCode.map(String.init) ?? "none")")
        return lines.joined(separator: "\n") + "\n"
    }
}
