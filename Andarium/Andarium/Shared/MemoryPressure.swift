// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The words of the notice that iOS is critically short of memory, over the
/// running guest. They are one place so the text can change without touching
/// the logic.
enum MemoryPressureWords {
    static let title = String(
        localized: "Memory is critically low",
        comment: "Title of the notice shown over the running guest when iOS is critically short of memory.")
    static let detail = String(
        localized: "iOS may close Andarium.",
        comment: "Text of the notice shown when iOS is critically short of memory: iOS may end the Andarium app. Andarium is the app's name.")
    /// What VoiceOver says when the notice appears.
    static var announcement: String { title + ". " + detail }
}
