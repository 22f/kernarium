// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// The words of the notice that iOS is short of memory, over the running
/// session. They are one place so the text can change without touching the
/// logic.
enum MemoryPressureWords {
    static let lowTitle = String(localized: "Memory is low")
    static let lowDetail = String(
        localized: "iOS asked Andarium to free memory. The virtual machine may slow down for a moment.")
    static let criticalTitle = String(localized: "Memory is critically low")
    /// The memory is set on Home and read when the virtual machine starts, so
    /// the notice says to shut it down first. Edit and Virtual machine are
    /// labels on Home.
    static let criticalDetail = String(
        localized: "iOS may close Andarium. Close other apps, or shut down the virtual machine and lower its memory with Edit under Virtual machine.")

    /// The notice's one line.
    static func title(for notice: MemoryPressureNotice) -> String {
        switch notice {
        case .low: lowTitle
        case .critical: criticalTitle
        }
    }

    /// The line under it.
    static func detail(for notice: MemoryPressureNotice) -> String {
        switch notice {
        case .low: lowDetail
        case .critical: criticalDetail
        }
    }
}
