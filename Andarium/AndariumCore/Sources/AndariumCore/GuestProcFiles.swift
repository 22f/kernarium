// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// The guest kernel's own counters that bear on a stall, read over adb when a
/// bug report is made and added to it: the timers and interrupts it saw, the
/// CPU time it accounted and the pressure-stall time of its CPU, I/O and
/// memory (`/proc/timer_list`, `/proc/interrupts`, `/proc/stat`,
/// `/proc/pressure/*`). They cover the guest's whole run, so they show with
/// the host's stall record (`StallRecord`) whether time went missing inside
/// the guest or outside it.
///
/// One shell command reads the files and each is cut to its own limit, so the
/// entry is never longer than `maximumBytes`. A file the shell user may not
/// read is tried as root (`su 0`) and otherwise reports why it could not be
/// read; the bug report never fails for want of them.
public enum GuestProcFiles {
    public struct Source: Equatable, Sendable {
        public var path: String
        public var limit: Int
    }

    public static let sources: [Source] = [
        Source(path: "/proc/timer_list", limit: 128 << 10),
        Source(path: "/proc/interrupts", limit: 32 << 10),
        Source(path: "/proc/stat", limit: 32 << 10),
        Source(path: "/proc/pressure/cpu", limit: 4 << 10),
        Source(path: "/proc/pressure/io", limit: 4 << 10),
        Source(path: "/proc/pressure/memory", limit: 4 << 10),
    ]

    public static let entryName = "andarium/guest-proc.txt"

    /// The most the entry holds: the files at their limits and the lines that
    /// name them.
    public static let maximumBytes = sources.reduce(0) { $0 + $1.limit } + sources.count * 128

    /// The command for the guest's shell.
    public static var command: String {
        let reads = sources.map { source in
            "r \(source.path) \(source.limit)"
        }.joined(separator: "; ")
        return "r() { echo \"==> $1 <==\"; { head -c $2 \"$1\" 2>/dev/null || su 0 head -c $2 \"$1\"; } 2>&1; echo; }; \(reads)"
    }

    /// The entry for what the command printed; nil when it printed nothing.
    public static func entry(from output: String) -> BugReportArchive.Entry? {
        guard !output.isEmpty else { return nil }
        let bytes = Data(output.utf8).prefix(maximumBytes)
        return BugReportArchive.Entry(name: entryName, data: Data(bytes))
    }
}
