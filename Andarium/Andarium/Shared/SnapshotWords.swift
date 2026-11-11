// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// The kinds of operation the snapshot screens run, for the words that say
/// which one did not go ahead.
enum SnapshotOperation: Equatable {
    case take, restore, delete, rename
}

/// Every word of the snapshot screens that is said from a value: the driver
/// returns refusals, plans and figures, never text, and each screen says them
/// here (`Andarium/README.md#model-and-storage`). Nothing in this file reads
/// a view or a model.
enum SnapshotWords {
    static let title = "Snapshots"
    static let manage = "Manage"
    static let manageAccessibility = "Manage snapshots"
    static let take = "Take snapshot"
    static let none = "None yet"
    static let spaceLink = "How snapshots use space"
    static let needsNewerApp = "Needs a newer Andarium"
    static let cannotBeRestored = "Can\u{2019}t be restored"
    static let damaged = "Damaged"

    // MARK: Figures and dates

    /// "3 snapshots".
    static func count(_ count: Int) -> String { count == 1 ? "1 snapshot" : "\(count) snapshots" }

    /// "1.2 GiB", and "At least 1.2 GiB" where only a lower bound is known.
    static func size(_ figure: SnapshotFigure, capitalized: Bool = true) -> String {
        let text = Formats.size(bytes: figure.bytes)
        guard figure.isLowerBound else { return text }
        return capitalized ? "At least \(text)" : "at least \(text)"
    }

    /// What VoiceOver says of a row's figure.
    static func freesAccessibility(_ figure: SnapshotFigure) -> String {
        let text = Formats.size(bytes: figure.bytes)
        return figure.isLowerBound ? "Frees at least \(text) if deleted" : "Frees \(text) if deleted"
    }

    /// "Nov 11, 2026 at 7:45 AM", in the person's own format.
    static func taken(_ date: Date) -> String { date.formatted(date: .abbreviated, time: .shortened) }

    /// The date with the month in full, for a snapshot's page.
    static func takenLong(_ date: Date) -> String { date.formatted(date: .long, time: .shortened) }

    /// "r20", or a release ID that is not in the naming scheme as it is.
    static func revision(of releaseID: String) -> String { ReleaseLabel.revision(of: releaseID) }

    /// Home's row: "None yet", "1 snapshot", "5 snapshots \u{00B7} 4.6 GiB".
    /// `total` is nil while the figures are not measured.
    static func homeSubtitle(count: Int, total: SnapshotFigure?) -> String {
        guard count > 0 else { return none }
        guard let total else { return Self.count(count) }
        return "\(Self.count(count)) \u{00B7} \(size(total, capitalized: false))"
    }

    /// The sheet's subtitle: "4.6 GiB on this iPhone", "At least 4.6 GiB on this iPhone".
    static func sheetSubtitle(total: SnapshotFigure, device: String) -> String {
        "\(size(total)) on this \(device)"
    }

    // MARK: Rows and pages

    /// The name, or when it was taken where it has none.
    static func title(of item: SnapshotSummary) -> String {
        item.name.isEmpty ? taken(item.created) : item.name
    }

    /// Under a row's title: "Nov 11, 2026 at 7:45 AM \u{00B7} r20" under a name,
    /// "r20" under a date.
    static func rowDetail(of item: SnapshotSummary) -> String {
        let release = revision(of: item.releaseID)
        return item.name.isEmpty ? release : "\(taken(item.created)) \u{00B7} \(release)"
    }

    /// The orange line under a snapshot that cannot be restored by this app version.
    static func rowFlag(_ restore: SnapshotRestorePlan) -> String? {
        guard case .refused(let reason) = restore else { return nil }
        switch reason {
        case .newerAndroid, .olderDataLayout: return needsNewerApp
        case .incomplete: return damaged
        default: return cannotBeRestored
        }
    }

    /// Beneath Restore: what it does, or why this app version cannot.
    /// `appRevision` is the Android this app carries; `endsUpdate` is set
    /// while the guest has an update that did not finish, which a restore ends.
    static func restoreNote(of item: SnapshotSummary, appRevision: String, endsUpdate: Bool) -> String {
        let ending = endsUpdate ? " Andarium\u{2019}s unfinished update ends." : ""
        switch item.restore {
        case .asIs:
            return "Replaces the guest\u{2019}s current data with this snapshot." + ending
        case .updatesAtNextStart(let from, let to):
            return "Replaces the guest\u{2019}s current data with this snapshot, which is from \(revision(of: from)). "
                + "The next start updates it to \(revision(of: to))." + ending
        case .refused(let reason):
            return refusal(reason, snapshot: item, appRevision: appRevision)
        }
    }

    // MARK: Prompts

    static func restoreTitle(of item: SnapshotSummary) -> String {
        item.name.isEmpty ? "Restore this snapshot?" : "Restore \u{201C}\(item.name)\u{201D}?"
    }

    static func restoreMessage(of item: SnapshotSummary, endsUpdate: Bool) -> String {
        var text = "The guest\u{2019}s current data is replaced by this snapshot from \(taken(item.created)), "
            + "and anything not in it is deleted. Some apps may ask you to sign in again."
        if case .updatesAtNextStart(_, let to) = item.restore { text += " The next start updates it to \(revision(of: to))." }
        if endsUpdate { text += " Andarium\u{2019}s unfinished update ends." }
        return text + " This can\u{2019}t be undone."
    }

    static func deleteTitle(of item: SnapshotSummary) -> String {
        item.name.isEmpty ? "Delete the snapshot from \(taken(item.created))?" : "Delete \u{201C}\(item.name)\u{201D}?"
    }

    /// What deleting frees, when the figure is measured; otherwise only that
    /// it can't be undone.
    static func deleteMessage(frees: SnapshotFigure?) -> String {
        guard let frees else { return "This can\u{2019}t be undone." }
        return "Deleting it frees \(size(frees, capitalized: false)). This can\u{2019}t be undone."
    }

    static let takeAlertMessage = "A new snapshot takes no space. It grows as the guest changes after it."

    // MARK: Home

    /// The notice after a restore: its title and text.
    static let restoredTitle = "Snapshot restored"

    static func restoredText(_ notice: RestoredSnapshotNotice) -> String {
        var text = "The guest\u{2019}s data is back to \(describing(name: notice.name, created: notice.created))."
        if let updatesTo = notice.updatesTo { text += " The next start updates it to \(updatesTo)." }
        return text
    }

    /// "\u{201C}Before r20\u{201D} from Nov 9, 2026 at 8:55 AM", or "the snapshot from ..." where it has no name.
    private static func describing(name: String, created: Date) -> String {
        name.isEmpty ? "the snapshot from \(taken(created))" : "\u{201C}\(name)\u{201D} from \(taken(created))"
    }

    /// While a restore stands unfinished, from this attempt or an earlier session.
    static let unfinishedTitle = "Snapshot restore not finished"

    /// `item` is the snapshot being restored, when it can be named.
    static func unfinishedText(item: SnapshotSummary?, failure: SnapshotFailure?) -> String {
        var text: String
        if let item {
            text = "The guest\u{2019}s data is back to \(describing(name: item.name, created: item.created)), "
                + "but Andarium could not finish the restore. Start finishes it."
            if case .updatesAtNextStart(_, let to) = item.restore { text += " The next start updates it to \(revision(of: to))." }
        } else {
            text = "Andarium put a snapshot\u{2019}s data in place and could not finish the restore. Start finishes it."
        }
        if let failure { text += " The last attempt stopped: \(failure.description)." }
        return text
    }

    /// Erase guest's alert says what goes with the guest.
    static func eraseMessage(snapshots: Int) -> String {
        let with = switch snapshots {
        case 0: ""
        case 1: ", and its snapshot with it"
        default: ", and its \(snapshots) snapshots with it"
        }
        return "Everything the guest has stored is deleted\(with). This can\u{2019}t be undone."
    }

    /// Low space says snapshots are a way out, when there are some.
    static func lowSpaceText(hasSnapshots: Bool, startReserve: String, device: String) -> String {
        "The guest needs \(startReserve) free to start. Free up space in Settings \u{203A} General \u{203A} \(device) Storage"
            + (hasSnapshots ? ", or delete snapshots." : ".")
    }

    /// The card for an app too old for the guest's data says an older snapshot
    /// is a way back, when there is one.
    static func blockedText(_ reason: StartBlock, hasOlderSnapshot: Bool) -> String {
        guard reason == .olderAndroid, hasOlderSnapshot else { return reason.explanation }
        return "This app version is older than the one the guest last started with, and its data does not go back. "
            + "Install a newer version of Andarium, restore an older snapshot, or erase the guest to start again with this one."
    }

    /// Backup's note says snapshots are not in a backup, when there are some.
    static func backupNote(device: String, hasSnapshots: Bool) -> String {
        "Puts the guest's apps and files in iCloud and computer backups of this \(device)."
            + (hasSnapshots ? " Snapshots are not included." : "")
    }

    // MARK: What is unavailable

    /// Above the dimmed Take snapshot; nil when nothing needs saying (it is
    /// available, or an operation is under way and shows itself).
    static func takeReason(_ refusal: SnapshotRefusal) -> String? {
        switch refusal {
        case .busy: nil
        case .guestNotStopped: "Not while the guest is starting or running."
        case .noData: "Start the guest once first. Until then there is no data to keep."
        case .unreadableRecord: "The guest\u{2019}s records cannot be read, so its data cannot be snapshotted."
        case .notShutDown:
            "Android wasn\u{2019}t shut down when the guest last ran. Start the guest and shut it down from Android, then take a snapshot."
        case .unfinishedUpdate:
            "Not while Andarium\u{2019}s update is unfinished. Start tries it again, or use Restore on Home."
        case .mismatch: "Not while the guest\u{2019}s files do not match this app version."
        case .volumeCannotClone: "The guest\u{2019}s data cannot be snapshotted (it needs an APFS volume)."
        case .restoreUnfinished: "A restore did not finish. Start finishes it."
        default: nil
        }
    }

    /// Why a snapshot cannot be restored or deleted for now, shown in place of
    /// what Restore does; nil for a refusal that is about the snapshot itself.
    static func nowReason(_ refusal: SnapshotRefusal) -> String? {
        switch refusal {
        case .guestNotStopped: "Not while the guest is starting or running."
        case .busy: "Another snapshot operation is under way."
        case .restoreUnfinished: "A restore did not finish. Start finishes it."
        default: nil
        }
    }

    // MARK: Refusals and failures

    /// The title of the alert for an operation that did not go ahead.
    static func problemTitle(_ operation: SnapshotOperation) -> String {
        switch operation {
        case .take: "The snapshot could not be taken"
        case .restore: "The snapshot could not be restored"
        case .delete: "The snapshot could not be deleted"
        case .rename: "The snapshot could not be renamed"
        }
    }

    /// Every refusal in words. `snapshot` is the one the refusal is about, if
    /// any; `appRevision` the Android this app carries.
    static func refusal(
        _ refusal: SnapshotRefusal, snapshot: SnapshotSummary? = nil, appRevision: String = "", device: String = "device"
    ) -> String {
        switch refusal {
        case .guestNotStopped:
            return "The guest is starting or running. Snapshots can be taken, restored and deleted only while it is stopped."
        case .busy:
            return "Another snapshot operation is under way. Try again in a moment."
        case .noData, .unreadableRecord, .notShutDown, .unfinishedUpdate, .mismatch, .volumeCannotClone:
            return takeReason(refusal) ?? ""
        case .notEnoughSpace:
            return "There is not enough free space on this \(device). Free up space in Settings \u{203A} General \u{203A} \(device) Storage and try again."
        case .newerAndroid:
            let theirs = snapshot.map { revision(of: $0.releaseID) } ?? "a newer release"
            return "This snapshot is from \(theirs), newer than this app\u{2019}s \(appRevision). Update Andarium to restore it."
        case .olderDataLayout:
            return "This snapshot\u{2019}s data uses a newer layout than this app version can open. Update Andarium to restore it."
        case .differentSystem:
            return "This snapshot was taken with a different system image of the same release, so this app version can\u{2019}t restore it."
        case .diskSizeNotAccepted(let gib):
            return "This snapshot\u{2019}s disk is \(gib) GiB, a size this app version does not accept. It can only be deleted."
        case .incomplete:
            return "Some of this snapshot\u{2019}s files are missing, so it can\u{2019}t be restored. It can only be deleted."
        case .missing:
            return "This snapshot is not there any more."
        case .restoreUnfinished:
            return "A restore did not finish. Start finishes it."
        case .invalidName:
            return "A name has at most \(SnapshotRules.maximumNameLength) characters and no line breaks."
        }
    }

    // MARK: The space page

    static let spacePageTitle = "Snapshot space"

    /// The explanation, one short paragraph each. The ones about the lower bound
    /// and the unfinished update apply only when they do.
    static func spaceParagraphs(device: String, lowerBound: Bool, hasUpdateCopy: Bool) -> [String] {
        var paragraphs = [
            "A new snapshot takes no space. When Android later changes or deletes data, the snapshot keeps the old version, "
                + "so it grows as the guest is used, and by a lot after an update of Android.",
            "The size beside a snapshot is what deleting it frees. Snapshots can share old data that none of them frees alone, "
                + "so together they use more than their sizes add up to.",
        ]
        if lowerBound {
            paragraphs.append("This \(device) measures only part of what snapshots share, so they use at least the size shown.")
        }
        if hasUpdateCopy {
            paragraphs.append("Andarium\u{2019}s unfinished update keeps its own copy until it ends. Restoring a snapshot ends it.")
        }
        paragraphs += [
            "Deleting files in Android frees nothing a snapshot still keeps. Deleting or restoring a snapshot gives space back.",
            "Snapshots stay on this \(device). They are not in backups, and Erase guest deletes them.",
        ]
        return paragraphs
    }
}
