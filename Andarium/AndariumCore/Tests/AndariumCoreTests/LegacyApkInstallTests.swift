// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The command Install APK sends, how the package manager's answer is read, and
/// the flow that asks before an app built for a much older Android is installed
/// anyway.
struct LegacyApkInstallTests {
    // MARK: The command line

    @Test func theInstallServiceHasItsArguments() {
        #expect(
            PackageInstallCommand.service(size: 1234)
                == "abb_exec:package\0install\0-r\0-t\0--install-reason\0" + "4\0-S\0" + "1234")
    }

    @Test func theInstallIsMarkedAsTheUsersAndTheMarkComesBeforeTheSize() throws {
        let words = PackageInstallCommand.service(size: 1234).split(separator: "\0", omittingEmptySubsequences: false)
        let reason = try #require(words.firstIndex(of: "--install-reason"))
        // The reason is Android's INSTALL_REASON_USER, and `-S size` stays the tail
        // the package manager reads the stream by.
        #expect(words[reason + 1] == "4")
        #expect(words.suffix(2) == ["-S", "1234"])
        #expect(try #require(words.firstIndex(of: "-S")) > reason + 1)
    }

    @Test func theDefaultCommandCarriesNoBypassFlag() {
        #expect(!PackageInstallCommand.service(size: 1).contains("bypass"))
        #expect(PackageInstallCommand.service(size: 77) == PackageInstallCommand.service(size: 77, bypassLowTargetSdkBlock: false))
    }

    @Test func theFlagIsTheOnlyDifferenceAndStaysBeforeTheSize() {
        let plain = PackageInstallCommand.service(size: 4096).split(separator: "\0", omittingEmptySubsequences: false)
        let flagged = PackageInstallCommand.service(size: 4096, bypassLowTargetSdkBlock: true)
            .split(separator: "\0", omittingEmptySubsequences: false)
        #expect(flagged.count == plain.count + 1)
        #expect(flagged.filter { $0 == "--bypass-low-target-sdk-block" }.count == 1)
        #expect(flagged.filter { $0 != "--bypass-low-target-sdk-block" } == plain)
        #expect(flagged.suffix(2) == ["-S", "4096"])
        #expect(
            PackageInstallCommand.service(size: 4096, bypassLowTargetSdkBlock: true)
                == "abb_exec:package\0install\0-r\0-t\0--install-reason\0" + "4\0--bypass-low-target-sdk-block\0-S\0" + "4096")
    }

    // MARK: The answer

    private let refusal =
        "Failure [INSTALL_FAILED_DEPRECATED_SDK_VERSION: App package must target at least SDK version 24, but found 7]"

    @Test func theTooOldRefusalIsReadWithItsNumbers() {
        #expect(PackageInstallAnswer(output: refusal + "\n") == .tooOldTarget(LegacyTarget(appLevel: 7, requiredLevel: 24)))
        #expect(
            PackageInstallAnswer(output: "Performing Streamed Install\n" + refusal + "\n")
                == .tooOldTarget(LegacyTarget(appLevel: 7, requiredLevel: 24)))
    }

    @Test func theTooOldRefusalIsReadWithoutNumbers() {
        for output in [
            "Failure [INSTALL_FAILED_DEPRECATED_SDK_VERSION]\n",
            "Failure [INSTALL_FAILED_DEPRECATED_SDK_VERSION: ]\n",
            "Failure [INSTALL_FAILED_DEPRECATED_SDK_VERSION: this app is too old]\n",
            "Failure [INSTALL_FAILED_DEPRECATED_SDK_VERSION: App package must target at least SDK version, but found]\n",
        ] {
            #expect(PackageInstallAnswer(output: output) == .tooOldTarget(LegacyTarget()), "\(output)")
        }
    }

    @Test func caseWhitespaceAndLineEndingsDoNotMatter() {
        let target = LegacyTarget(appLevel: 7, requiredLevel: 24)
        for output in [
            "  " + refusal + "  \r\n",
            refusal.lowercased() + "\n",
            refusal.uppercased() + "\n",
            "\n\n" + refusal,
            "Failure [ INSTALL_FAILED_DEPRECATED_SDK_VERSION : App package must target at least SDK version 24, but found 7 ]\n",
            "Failure [FAILED_DEPRECATED_SDK_VERSION: App package must target at least SDK version 24, but found 7]",
        ] {
            #expect(PackageInstallAnswer(output: output) == .tooOldTarget(target), "\(output.debugDescription)")
        }
    }

    @Test func everyOtherFailureIsNotTheTooOldRefusal() {
        let others = [
            "INSTALL_FAILED_OLDER_SDK", "INSTALL_FAILED_NO_MATCHING_ABIS", "INSTALL_FAILED_VERSION_DOWNGRADE",
            "INSTALL_PARSE_FAILED_NO_CERTIFICATES", "INSTALL_FAILED_INVALID_APK", "INSTALL_FAILED_INSUFFICIENT_STORAGE",
            "INSTALL_FAILED_UPDATE_INCOMPATIBLE", "INSTALL_FAILED_DEPRECATED_SDK_VERSIONS", "INSTALL_FAILED_DEPRECATED",
        ]
        for code in others {
            let answer = PackageInstallAnswer(output: "Failure [\(code): App package must target at least SDK version 24, but found 7]\n")
            #expect(answer == .refused(code: String(code.dropFirst("INSTALL_".count))), "\(code)")
        }
        // The same words outside a code are not the refusal.
        #expect(PackageInstallAnswer(output: "App package must target at least SDK version 24, but found 7\n") == .refused(code: nil))
        #expect(PackageInstallAnswer(output: "Failure: DEPRECATED_SDK_VERSION\n") == .refused(code: nil))
        #expect(PackageInstallAnswer(output: "") == .refused(code: nil))
        #expect(PackageInstallAnswer(output: "Failure\n") == .refused(code: nil))
        #expect(PackageInstallAnswer(output: "Failure []\n") == .refused(code: nil))
    }

    @Test func anInstantAppThatTargetsTooLowIsNotTheTooOldRefusal() {
        let output = "Failure [INSTALL_FAILED_DEPRECATED_SDK_VERSION: Instant app package must target at least O]\n"
        #expect(PackageInstallAnswer(output: output) == .refused(code: "FAILED_DEPRECATED_SDK_VERSION"))
    }

    @Test func aSuccessLineWinsWhateverElseTheOutputSays() {
        #expect(PackageInstallAnswer(output: "Success\n") == .installed)
        #expect(PackageInstallAnswer(output: "Performing Streamed Install\nSuccess\n") == .installed)
        // A warning that carries the words, then the success.
        #expect(
            PackageInstallAnswer(output: "pkg: warning INSTALL_FAILED_DEPRECATED_SDK_VERSION ignored\nSuccess\n") == .installed)
        #expect(PackageInstallAnswer(output: refusal + "\nSuccess\n") == .installed)
        #expect(PackageInstallAnswer(output: "Success [INSTALL_FAILED_DEPRECATED_SDK_VERSION]\n") == .installed)
    }

    @Test func theLastFailureLineIsTheAnswer() {
        let output = refusal + "\nFailure [INSTALL_FAILED_INVALID_APK: broken]\n"
        #expect(PackageInstallAnswer(output: output) == .refused(code: "FAILED_INVALID_APK"))
        #expect(
            PackageInstallAnswer(output: "Failure [INSTALL_FAILED_INVALID_APK: broken]\n" + refusal + "\n")
                == .tooOldTarget(LegacyTarget(appLevel: 7, requiredLevel: 24)))
    }

    // MARK: The flow

    private let target = LegacyTarget(appLevel: 7, requiredLevel: 24)

    @Test func theFirstAttemptIsTheOrdinaryInstall() {
        #expect(LegacyInstallFlow().step == .attempt(bypassLowTargetSdkBlock: false))
    }

    @Test func aTooOldRefusalAsksAndInstallAnywayRetriesWithTheFlagAndSucceeds() {
        var flow = LegacyInstallFlow()
        flow.attemptEnded(.tooOldTarget(target))
        #expect(flow.step == .ask(target))
        flow.answered(installAnyway: true)
        #expect(flow.step == .attempt(bypassLowTargetSdkBlock: true))
        flow.attemptEnded(.installed)
        #expect(flow.step == .finished(.installed))
    }

    @Test func theRetryThatFailsEndsWithOrdinaryFailureWordsAndAsksNothing() {
        var flow = LegacyInstallFlow()
        flow.attemptEnded(.tooOldTarget(target))
        flow.answered(installAnyway: true)
        flow.attemptEnded(.failed)
        #expect(flow.step == .finished(.failed))
        // Nothing reopens an ended flow.
        flow.answered(installAnyway: true)
        flow.attemptEnded(.tooOldTarget(target))
        #expect(flow.step == .finished(.failed))
    }

    @Test func aRetryRefusedAsTooOldAgainIsAFailureAndNeverAsksTwice() {
        var flow = LegacyInstallFlow()
        flow.attemptEnded(.tooOldTarget(target))
        flow.answered(installAnyway: true)
        flow.attemptEnded(.tooOldTarget(target))
        #expect(flow.step == .finished(.failed))
    }

    @Test func cancelEndsTheInstallAsCancelledWithNoRetry() {
        var flow = LegacyInstallFlow()
        flow.attemptEnded(.tooOldTarget(target))
        flow.answered(installAnyway: false)
        #expect(flow.step == .finished(.cancelled))
        flow.attemptEnded(.installed)
        flow.answered(installAnyway: true)
        #expect(flow.step == .finished(.cancelled))
    }

    @Test func aDifferentFirstFailureNeverAsks() {
        var flow = LegacyInstallFlow()
        flow.attemptEnded(.failed)
        #expect(flow.step == .finished(.failed))
        flow.answered(installAnyway: true)
        #expect(flow.step == .finished(.failed))
    }

    @Test func aFirstAttemptThatSucceedsAsksNothing() {
        var flow = LegacyInstallFlow()
        flow.attemptEnded(.installed)
        #expect(flow.step == .finished(.installed))
    }

    @Test func theAnswerBeforeTheQuestionIsIgnored() {
        var flow = LegacyInstallFlow()
        flow.answered(installAnyway: true)
        #expect(flow.step == .attempt(bypassLowTargetSdkBlock: false))
    }

    @Test func stoppingFromAnywhereCancelsAndAnEndedFlowStaysEnded() {
        var asking = LegacyInstallFlow()
        asking.attemptEnded(.tooOldTarget(target))
        asking.stopped()
        #expect(asking.step == .finished(.cancelled))

        var sending = LegacyInstallFlow()
        sending.stopped()
        #expect(sending.step == .finished(.cancelled))

        var done = LegacyInstallFlow()
        done.attemptEnded(.installed)
        done.stopped()
        #expect(done.step == .finished(.installed))
    }

    @Test func aFlowIsDrivenToTheAnswerTheBypassAttemptGives() {
        // The whole path as a model walks it, for each way the retry can end.
        for (second, ending) in [
            (LegacyInstallFlow.Result.installed, LegacyInstallFlow.Ending.installed),
            (.failed, .failed),
            (.tooOldTarget(LegacyTarget()), .failed),
        ] {
            var flow = LegacyInstallFlow()
            var sent: [Bool] = []
            var asked = 0
            var results: [LegacyInstallFlow.Result] = [.tooOldTarget(target), second]
            while true {
                switch flow.step {
                case let .attempt(bypass):
                    sent.append(bypass)
                    flow.attemptEnded(results.removeFirst())
                case .ask:
                    asked += 1
                    flow.answered(installAnyway: true)
                case .finished:
                    #expect(flow.step == .finished(ending))
                    #expect(sent == [false, true] && asked == 1)
                }
                if case .finished = flow.step { break }
            }
            #expect(flow.step == .finished(ending))
            #expect(sent == [false, true] && asked == 1)
        }
    }
}
