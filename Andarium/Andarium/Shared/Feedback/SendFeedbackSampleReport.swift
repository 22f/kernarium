// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL && !APP_STORE

import AndariumCore
import Foundation

/// A small bug report made up for the Simulator preview and the UI tests, which have no guest to make
/// one: the shape of Android's (a long log, short files, a folder-like path, a picture, an archive
/// inside) with the record Andarium appends, every line invented. It is a real zip, written the way the
/// app writes its own entries, so the viewer reads it as it reads a real report. The App Store product
/// has none of it.
nonisolated enum SendFeedbackSampleReport {
    /// Writes the report to `url`.
    static func write(to url: URL) throws {
        try Data([0x50, 0x4B, 0x05, 0x06] + [UInt8](repeating: 0, count: 18)).write(to: url)
        let inner = url.deletingLastPathComponent().appendingPathComponent(".inner-\(UUID().uuidString).zip")
        defer { try? FileManager.default.removeItem(at: inner) }
        try Data([0x50, 0x4B, 0x05, 0x06] + [UInt8](repeating: 0, count: 18)).write(to: inner)
        try BugReportArchive.append(
            [
                .init(name: "anr/example-app.txt", data: Data("\"main\" prio=5 tid=1 Sleeping\n  at java.lang.Thread.sleep(Native Method)\n".utf8)),
                .init(name: "anr/example-service.txt", data: Data("\"binder:1234_1\" prio=5 tid=12 Native\n".utf8)),
            ], to: inner)
        try BugReportArchive.append(
            [
                .init(name: "bugreport-lineage-2026-11-17-15-00-00.txt", data: Data(log().utf8)),
                .init(name: "main_entry.txt", data: Data("bugreport-lineage-2026-11-17-15-00-00.txt\n".utf8)),
                .init(name: "version.txt", data: Data("3.0\n".utf8)),
                .init(name: "dumpstate_log.txt", data: Data("1 2026-11-17 15:00:00 dumpstate: begin\n2 2026-11-17 15:00:41 dumpstate: done\n".utf8)),
                .init(name: "FS/data/misc/example.conf", data: Data("# an example file\nkey=value\n".utf8)),
                .init(name: "FS/data/anr/traces.zip", data: (try? Data(contentsOf: inner)) ?? Data()),
                .init(name: "screenshot-2026-11-17.png", data: png),
                .init(name: "andarium/session-current.txt", data: Data("uml-session 7\nstate running\nend running\n".utf8)),
            ], to: url)
    }

    /// A few hundred lines of a log, invented and numbered.
    private static func log() -> String {
        let tags = ["ActivityManager", "WindowManager", "SurfaceFlinger", "AudioFlinger", "PackageManager", "netd", "zygote"]
        let messages = [
            "Start proc com.example.game for top-activity {com.example.game/com.example.game.MainActivity}",
            "Relayout window Window{3a1f u0 com.example.game/com.example.game.MainActivity}",
            "setTransactionState: frame 2840 displayed",
            "Output stream underrun on usb:0",
            "Verified package com.example.game against its signature",
            "Interface wlan0 changed state to up",
            "Zygote forked a process for com.example.game",
        ]
        var lines = ["------ SYSTEM LOG (logcat -v threadtime -d) ------"]
        for index in 0..<420 {
            let second = 1 + index / 9
            let stamp = String(format: "11-17 15:%02d:%02d.%03d", second / 60, second % 60, (index * 137) % 1_000)
            let pid = 1_200 + (index % 7) * 31
            lines.append("\(stamp)  \(pid)  \(pid + 4) I \(tags[index % tags.count]): \(messages[index % messages.count]) [\(index)]")
        }
        return lines.joined(separator: "\n") + "\n"
    }

    /// A 1x1 picture: bytes that are not text.
    private static let png = Data([
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
        0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
        0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99, 0x3D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
        0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
    ])
}

#endif
