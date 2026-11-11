// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Counts as the screens say them, in the person's language. A plural is a
/// variation of the one key in the catalog, so each language has its own
/// forms.
public enum CountWords {
    /// "1 vCPU", "6 vCPUs".
    public static func vcpus(_ count: Int) -> String {
        String(localized: "\(count) vCPUs", bundle: .module)
    }
}

/// Whole sentences one after another. A text that says one more thing only
/// when it applies is its sentences joined, not one string with a hole in
/// it, so each sentence is translated whole. English separates sentences with
/// a space; Chinese and Japanese write them with none.
public enum Sentences {
    public static func join(_ sentences: String...) -> String { join(sentences) }

    public static func join(_ sentences: [String]) -> String {
        sentences.reduce("") { text, sentence in
            text.isEmpty ? sentence : String(localized: "\(text) \(sentence)", bundle: .module)
        }
    }
}
