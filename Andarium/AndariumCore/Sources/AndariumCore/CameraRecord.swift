// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// How many stills of the guest's cameras did not end in a picture during a
/// session, and why for the last few, as the runtime wrote them into the
/// session record's camera slot (darwin/runtime/session_record.hpp).
///
/// Every field is a number or one of the runtime's fixed words: the record
/// holds no path, no name and no message of the system's. The slot is blank
/// until the first failure, and a blank slot is no claim: the session took
/// no still that failed, or it took none. A line this reader does not know is
/// skipped, and a field is read by its name, so a later runtime can add fields.
public struct CameraRecord: Equatable, Sendable {
    /// A size in pixels; the record writes `-` for one it does not know.
    public struct Size: Equatable, Sendable {
        public var width: Int
        public var height: Int

        public init(width: Int, height: Int) {
            self.width = width
            self.height = height
        }

        init?(_ word: Substring) {
            let parts = word.split(separator: "x", omittingEmptySubsequences: false)
            guard parts.count == 2, let width = Int(parts[0]), let height = Int(parts[1]) else { return nil }
            self.width = width
            self.height = height
        }

        var text: String { "\(width)x\(height)" }
    }

    /// One still that did not end in a picture.
    public struct Failure: Equatable, Sendable {
        public var at: Date
        /// The camera's index as the guest sees it, and where it is mounted
        /// (`back`, `front` or `external`).
        public var camera: Int
        public var position: String
        /// Where the still stopped, in the runtime's word: `refused` (before
        /// it reached the platform), `not-started`, `framework-error`,
        /// `missing-data`, `resolved-size` (the photo output made another
        /// size), `encoded-size` (the file states another size, or none),
        /// `file-size`, `guest-buffer`, `timeout` or `unknown`.
        public var stage: String
        /// The JPEG size the guest asked for, the size the photo output
        /// resolved to and the size the file's frame header states; nil where
        /// the record does not know it.
        public var asked: Size?
        public var resolved: Size?
        public var encoded: Size?
        /// Whether the photo output turns its stills to an earlier model's
        /// sensor orientation and whether that was on: `yes`, `no` or
        /// `unknown`.
        public var compensationSupported: String
        public var compensationEnabled: String
        /// The framework's error as one of its domain words (`avfoundation`,
        /// `osstatus`, `cocoa`, `posix`, `mach` or `other`; `none` for no
        /// error) and its code.
        public var errorDomain: String
        public var errorCode: Int?
    }

    /// The stills that failed during the session, and the newest of them that
    /// fit the slot, oldest first.
    public var failed: Int
    public var recent: [Failure]

    /// Reads the camera slot out of a session record's text (or the slot
    /// alone); nil when the slot is blank, as in a session that had no failure
    /// and in the records of the earlier layouts, or of a version this reader
    /// does not know.
    public init?(text: String) {
        var header = false
        var failed: Int?
        var recent: [Failure] = []
        // The kernel's quoted output is not the record's own lines.
        let own = text.components(separatedBy: "\nkernel-output\n")[0]
        for line in own.split(separator: "\n", omittingEmptySubsequences: true) {
            let words = line.split(separator: " ", omittingEmptySubsequences: true)
            guard let first = words.first else { continue }
            switch first {
            case "camera":
                header = words.count >= 2 && words[1] == "1"
            case "stills":
                // stills failed N
                if words.count >= 3, words[1] == "failed" { failed = Int(words[2]) }
            case "still":
                if let failure = Self.failure(words) { recent.append(failure) }
            default:
                break
            }
        }
        guard header, let failed else { return nil }
        self.failed = failed
        self.recent = recent
    }

    /// `still at T camera N POSITION stage STAGE asked S resolved S encoded S
    /// compensation supported X enabled Y error DOMAIN [CODE]`, read by the
    /// field names.
    private static func failure(_ words: [Substring]) -> Failure? {
        func value(after name: Substring) -> Substring? {
            guard let at = words.firstIndex(of: name), at + 1 < words.count else { return nil }
            return words[at + 1]
        }
        guard let time = value(after: "at").flatMap({ TimeInterval($0) }),
            let camera = value(after: "camera").flatMap({ Int($0) }),
            let cameraAt = words.firstIndex(of: "camera"), cameraAt + 2 < words.count,
            let stage = value(after: "stage")
        else { return nil }
        let domain = value(after: "error").map(String.init) ?? "none"
        let code: Int? = {
            guard let at = words.firstIndex(of: "error"), at + 2 < words.count else { return nil }
            return Int(words[at + 2])
        }()
        return Failure(
            at: Date(timeIntervalSince1970: time), camera: camera, position: String(words[cameraAt + 2]),
            stage: String(stage), asked: value(after: "asked").flatMap { Size($0) },
            resolved: value(after: "resolved").flatMap { Size($0) },
            encoded: value(after: "encoded").flatMap { Size($0) },
            compensationSupported: value(after: "supported").map(String.init) ?? "unknown",
            compensationEnabled: value(after: "enabled").map(String.init) ?? "unknown",
            errorDomain: domain, errorCode: code)
    }

    /// The record in words, for the bug report.
    public func report() -> String {
        var lines = [
            failed == 1
                ? "1 still of the guest's cameras did not end in a picture."
                : "\(failed) stills of the guest's cameras did not end in a picture."
        ]
        if recent.count < failed {
            lines.append("The last \(recent.count) that the record holds, oldest first:")
        }
        let formatter = ISO8601DateFormatter()
        for failure in recent {
            var line = "\(formatter.string(from: failure.at)): camera \(failure.camera) (\(failure.position)), "
            line += "stopped at \(failure.stage)"
            line += ", asked \(failure.asked?.text ?? "none")"
            line += ", resolved \(failure.resolved?.text ?? "unknown")"
            line += ", file states \(failure.encoded?.text ?? "unknown")"
            line += ", orientation compensation supported \(failure.compensationSupported)"
            line += " and enabled \(failure.compensationEnabled)"
            if failure.errorDomain != "none" {
                line += ", the framework's error \(failure.errorDomain)"
                if let code = failure.errorCode { line += " \(code)" }
            }
            lines.append(line + ".")
        }
        return lines.joined(separator: "\n")
    }
}
