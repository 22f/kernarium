// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What the application knows of the boot under way, to tell the runtime's
/// session record of it from any other that may be on disk: the record of a
/// session that ended, of another process (a launch before this one) or of an
/// earlier boot of this one. A record is the boot's when it is running, is of
/// this process, began in this run and says the restart count the application
/// counted for this boot.
public struct BootRecordBinding: Equatable, Sendable {
    /// This process: the record's `pid`. A record of an earlier launch is of
    /// another process, and a process id that came round again is told by
    /// `runStarted`.
    public var processID: Int
    /// When the run began, before the runtime began its first record: the
    /// record's `started` (a whole second) is not before it, so a record of an
    /// earlier run of this process, whose end was never written, is not taken.
    public var runStarted: Date
    /// How many times the runtime restarted the guest in place in this run
    /// (`GuestRestartEvent.began`'s count), which is the `restarts` its record
    /// says of the boot that follows. A restart makes it one more at once; the
    /// record of the boot before says the number before, or has ended, until
    /// the runtime begins the next.
    public var restarts: Int

    public init(processID: Int, runStarted: Date, restarts: Int = 0) {
        self.processID = processID
        self.runStarted = runStarted
        self.restarts = restarts
    }

    /// Whether `record` is the running record of the boot under way. A record
    /// that does not say its restart count (a guest whose console reports no
    /// boot) is not: nothing then binds it to this boot.
    public func accepts(_ record: SessionRecord) -> Bool {
        guard record.running, record.end == "running", record.processID == processID,
            record.restarts == restarts, let started = record.started
        else { return false }
        return started.timeIntervalSince1970 >= runStarted.timeIntervalSince1970.rounded(.down)
    }

    /// What the guest printed in the boot under way, as `record` last quoted it:
    /// nil where the record is not that boot's or holds no output yet.
    public func output(of record: SessionRecord?) -> String? {
        guard let record, accepts(record), let output = record.kernelOutput, !output.isEmpty else { return nil }
        return output
    }
}
