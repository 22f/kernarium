// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import AndariumCore
import Foundation

/// The log a sheet opened during a start attaches by itself: a small text file
/// with the step the start has reached, the time on the notice's clock, the
/// kind of start and what the virtual machine printed while starting, for this
/// boot only. It is in English, for whoever reads the mail, as the information
/// block is, and the person sees it as a row they can remove.
nonisolated enum SendFeedbackStartLog {
    /// A fixed ASCII name: the mail's attachments are named by it.
    static let fileName = "start-log.txt"

    /// The most of the printed output the file holds. The runtime's session
    /// record keeps 4 KiB of a boot's output (`session_kernel_output_bytes`); a
    /// longer text is cut from its start, to the last whole line.
    static let outputLimit = 4096

    static func kindName(_ kind: BootKind) -> String {
        switch kind {
        case .normal: "ordinary start"
        case .firstStart: "first start"
        case .upgrade: "first start after an update"
        case .recovery: "Recovery start"
        case .restart: "restart"
        }
    }

    /// The file's text. `output` is what the virtual machine printed in this
    /// boot, as the runtime's record last quoted it (nil where it has none yet);
    /// `containers` are the application's own folders, which the output may
    /// name.
    static func text(
        step: String, seconds: Int, kind: BootKind, restarts: Int, output: String?, containers: [String] = []
    ) -> String {
        var lines = [
            "Start log",
            "Step: \(step)",
            "Time: \(Formats.elapsed(seconds))",
            "Start: \(kindName(kind))",
        ]
        if restarts > 0 { lines.append("Restarts in this start: \(restarts)") }
        lines.append("")
        lines.append("Printed while starting, this boot only (the last \(outputLimit) bytes at most):")
        let printed = (output.map { masked(limited($0), containers: containers) } ?? "")
            .trimmingCharacters(in: .newlines)
        lines.append(printed.isEmpty ? "(nothing yet)" : printed)
        return lines.joined(separator: "\n") + "\n"
    }

    /// `output` cut from its start to at most `outputLimit` bytes, at a line.
    static func limited(_ output: String) -> String {
        guard output.utf8.count > outputLimit else { return output }
        var tail = String(decoding: Array(output.utf8.suffix(outputLimit)), as: UTF8.self)
        if let newline = tail.firstIndex(of: "\n") { tail = String(tail[tail.index(after: newline)...]) }
        return tail
    }

    /// The kernel argument whose value the first line may be the end of.
    private static let keyArgument = "androidboot.kernarium.adb_key"

    /// `text` with what could tell an installation apart taken out, as well as
    /// what the bug report takes out (`DiagnosticRedaction`):
    /// - the value of every `adb_key=` assignment (the kernel echoes its command
    ///   line, and the host's adb public key is on it), and of the same property
    ///   printed as `[...adb_key]: [value]`;
    /// - the value of `serialno=` and of the property `[...serialno]: [value]`
    ///   (the runtime masks the first in the record; this is the same rule again,
    ///   for a text that did not come from it);
    /// - any hardware address;
    /// - the application's own folders and the person's home folder;
    /// - and, where the text begins in the middle of a line, the rest of an
    ///   assignment that was cut, or of a key cut inside its value.
    static func masked(_ text: String, containers: [String] = []) -> String {
        let masked = "<masked>"
        var result = maskedFirstLineFragment(text)
        for (pattern, template) in [
            (#"(adb_key=)\S*"#, "$1\(masked)"),
            (#"(adb_key\]: \[)[^\]]*"#, "$1\(masked)"),
            (#"(serialno=)\S*"#, "$1\(masked)"),
            (#"(serialno\]: \[)[^\]]*"#, "$1\(masked)"),
            (#"\b[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}\b"#, "<address>"),
        ] {
            result = result.replacingOccurrences(of: pattern, with: template, options: .regularExpression)
        }
        return DiagnosticRedaction.redact(result, containers: containers)
    }

    /// A text cut at its start begins in the middle of a line. If the cut fell
    /// inside the argument that carries the key, what is left is either its
    /// name's tail and the value (`_key=AAAA...`), or the value's tail alone
    /// (`AAAA...==`, eight characters or more with its padding): the first word is masked in
    /// either case.
    private static func maskedFirstLineFragment(_ text: String) -> String {
        let end = text.firstIndex(where: { $0 == "\n" }) ?? text.endIndex
        let line = text[..<end]
        let wordEnd = line.firstIndex(where: { $0 == " " || $0 == "\t" }) ?? line.endIndex
        let word = String(line[..<wordEnd])
        let rest = text[wordEnd...]
        // A key's tail looks like base64 and is not a plain word: it has a
        // digit, a sign, or a capital after its first letter.
        if word.count >= 8, word.range(of: #"^[A-Za-z0-9+/]+={0,2}$"#, options: .regularExpression) != nil,
           word.contains(where: \.isNumber) || word.contains(where: { "+/=".contains($0) })
            || word.dropFirst().contains(where: \.isUppercase) {
            return "<masked>" + rest
        }
        if let equals = word.firstIndex(of: "=") {
            let name = word[..<equals]
            // A whole name is the ordinary rule's; a name's tail is a cut one.
            if keyArgument.hasSuffix(name), !name.hasSuffix("adb_key") {
                return String(word[...equals]) + "<masked>" + rest
            }
        }
        return text
    }
}

#endif
