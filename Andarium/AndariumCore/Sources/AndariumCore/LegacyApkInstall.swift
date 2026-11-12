// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What the package manager is sent to install one app, and how its answer is
/// read. Settings' Install APK is the one caller that can ask for the check
/// on the app's target version to be skipped (`LegacyInstallFlow`).
public enum PackageInstallCommand {
    /// Android's `INSTALL_REASON_USER`: an install the person started.
    public static let userInstallReason = 4

    /// What lifts Android's refusal of an app built for a version older than
    /// the one it installs.
    public static let bypassLowTargetSdkBlock = "--bypass-low-target-sdk-block"

    /// The `abb_exec` service of an install of `size` bytes. `-r` replaces an
    /// installed app, as the person expects of an APK for one already there;
    /// `-t` admits test-only builds; `--install-reason` marks the install as
    /// the person's, because the launcher adds a home-screen icon only for
    /// such an install; `-S` gives the size and stays the tail, which the
    /// package manager reads the stream by. `bypassLowTargetSdkBlock` adds
    /// the one flag a second attempt of a refused legacy app carries and
    /// changes nothing else; it is false for every other install.
    public static func service(size: Int, bypassLowTargetSdkBlock: Bool = false) -> String {
        var words = ["abb_exec:package", "install", "-r", "-t", "--install-reason", String(userInstallReason)]
        if bypassLowTargetSdkBlock { words.append(Self.bypassLowTargetSdkBlock) }
        words += ["-S", String(size)]
        return words.joined(separator: "\0")
    }
}

/// The versions in the package manager's refusal of an app built for a much
/// older Android, each nil when the answer does not say it.
public struct LegacyTarget: Equatable, Sendable {
    /// The API level the app targets (`but found 7`).
    public var appLevel: Int?
    /// The lowest API level the virtual machine's Android installs an app
    /// for (`at least SDK version 24`).
    public var requiredLevel: Int?

    public init(appLevel: Int? = nil, requiredLevel: Int? = nil) {
        self.appLevel = appLevel
        self.requiredLevel = requiredLevel
    }
}

/// What the package manager answered an install with, as far as the flow
/// needs it: the stream's output ends in `Success` or `Failure [CODE: text]`.
public enum PackageInstallAnswer: Equatable, Sendable {
    case installed
    /// The refusal of an app whose target version is below the floor:
    /// `INSTALL_FAILED_DEPRECATED_SDK_VERSION`. The only answer that gets
    /// the question.
    case tooOldTarget(LegacyTarget)
    /// Any other answer, with the code it names (`INSTALL_` dropped), if any.
    case refused(code: String?)

    /// The code of the too-old refusal, `INSTALL_` dropped.
    public static let tooOldCode = "FAILED_DEPRECATED_SDK_VERSION"

    /// Reads the output. A line that starts with `Success` is a success
    /// whatever else the output says; otherwise the last line that starts
    /// with `Failure` is the answer. Case and the space around the words do
    /// not matter, and the text after the code may be missing.
    public init(output: String) {
        let lines = output.split(whereSeparator: \.isNewline).map { $0.trimmingCharacters(in: .whitespaces) }
        if lines.contains(where: { $0.lowercased().hasPrefix("success") }) {
            self = .installed
            return
        }
        guard let line = lines.last(where: { $0.lowercased().hasPrefix("failure") }),
              let open = line.firstIndex(of: "[")
        else {
            self = .refused(code: nil)
            return
        }
        let inside = line[line.index(after: open)...]
        let name = inside.prefix { $0 != ":" && $0 != "]" }.trimmingCharacters(in: .whitespaces).uppercased()
        guard !name.isEmpty else {
            self = .refused(code: nil)
            return
        }
        let code = name.hasPrefix("INSTALL_") ? String(name.dropFirst("INSTALL_".count)) : name
        // The same code answers an instant app that targets too low, which
        // the flag does not lift.
        guard code == Self.tooOldCode, !line.lowercased().contains("instant app") else {
            self = .refused(code: code)
            return
        }
        self = .tooOldTarget(Self.levels(in: String(inside)))
    }

    /// `App package must target at least SDK version 24, but found 7`: the
    /// number after "at least SDK version" and the one after "found".
    private static func levels(in text: String) -> LegacyTarget {
        let lower = text.lowercased()
        return LegacyTarget(
            appLevel: number(after: "found", in: lower), requiredLevel: number(after: "at least sdk version", in: lower))
    }

    /// The digits right after `marker` (a space, colon or equals sign may
    /// come between), nil when something else follows it.
    private static func number(after marker: String, in text: String) -> Int? {
        guard let range = text.range(of: marker) else { return nil }
        let rest = text[range.upperBound...].drop { $0 == " " || $0 == ":" || $0 == "=" }
        let digits = rest.prefix { $0.isASCII && $0.isNumber }
        return digits.isEmpty ? nil : Int(digits)
    }
}

/// Settings' Install APK as a state machine: the first attempt is the
/// ordinary install; only the too-old refusal of it asks the person, and
/// only "Install anyway" sends the same file again, with the flag, once.
/// Everything else the first attempt answers ends the install as it always
/// did, and nothing is asked twice for one file. The model drives it with the
/// real attempts and the person's answer.
public struct LegacyInstallFlow: Equatable, Sendable {
    public enum Step: Equatable, Sendable {
        /// Send the file now, with the flag when `bypassLowTargetSdkBlock`.
        case attempt(bypassLowTargetSdkBlock: Bool)
        /// The first attempt was refused for its target version: ask.
        case ask(LegacyTarget)
        case finished(Ending)
    }

    public enum Ending: Equatable, Sendable {
        case installed
        /// The person said Cancel, or the install was stopped. No words.
        case cancelled
        /// The last attempt's ordinary failure words show.
        case failed
    }

    /// What an attempt ended with.
    public enum Result: Equatable, Sendable {
        case installed
        case tooOldTarget(LegacyTarget)
        /// Any other failure, which Android or adb worded.
        case failed
    }

    public private(set) var step = Step.attempt(bypassLowTargetSdkBlock: false)
    /// The flag has been sent, or is being: the one second attempt is used.
    public private(set) var askedBefore = false

    public init() {}

    /// An attempt ended. Ignored unless one was under way.
    public mutating func attemptEnded(_ result: Result) {
        guard case let .attempt(bypass) = step else { return }
        switch result {
        case .installed:
            step = .finished(.installed)
        case let .tooOldTarget(target):
            // The flag attempt that is refused again, or a second question
            // for one file, is a failure with its ordinary words.
            if bypass || askedBefore {
                step = .finished(.failed)
            } else {
                askedBefore = true
                step = .ask(target)
            }
        case .failed:
            step = .finished(.failed)
        }
    }

    /// The person answered the question. Ignored unless one was asked.
    public mutating func answered(installAnyway: Bool) {
        guard case .ask = step else { return }
        step = installAnyway ? .attempt(bypassLowTargetSdkBlock: true) : .finished(.cancelled)
    }

    /// The install was stopped (the overlay's Cancel, or the guest ended)
    /// while an attempt or the question was open.
    public mutating func stopped() {
        if case .finished = step { return }
        step = .finished(.cancelled)
    }
}
