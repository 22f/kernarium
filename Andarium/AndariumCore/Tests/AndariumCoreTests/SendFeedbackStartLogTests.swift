// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore
@testable import AndariumFeedbackLogic

/// The start log a Send feedback sheet attaches by itself when it is opened
/// during a start, and the sheet's rules for it: what the text says, what is
/// taken out of it, and the tip and the closing question while it is there.
struct SendFeedbackStartLogTests {
    /// A key as the Android adb keeps it: base64, with `+`, `/` and `=`.
    private let key = "QAAAAGJ4Z0n2Hq+Lx0aT/m8nR4sJ0d5bYwV9cE7uKf3PzQ1yM6hS2oXkUa8tNvB5rLq=="

    private func log(output: String?, restarts: Int = 0, containers: [String] = []) -> String {
        SendFeedbackStartLog.text(
            step: "Startup", seconds: 197, kind: .normal, restarts: restarts, output: output, containers: containers)
    }

    // MARK: The text

    @Test func theLogNamesTheStepTheTimeAndTheKindOfStart() {
        let text = log(output: "[   41.2] init: starting service 'netd'...\n")
        let lines = text.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        #expect(lines.starts(with: ["Start log", "Step: Startup", "Time: 3:17", "Start: ordinary start", ""]))
        #expect(lines.contains("[   41.2] init: starting service 'netd'..."))
        #expect(text.hasSuffix("\n") && !text.hasSuffix("\n\n"))
        #expect(!lines.contains { $0.hasPrefix("Restarts") })
        #expect(SendFeedbackStartLog.fileName == "start-log.txt")
    }

    @Test func everyKindOfStartHasItsOwnWords() {
        let names = [BootKind.normal, .firstStart, .upgrade, .recovery, .restart].map(SendFeedbackStartLog.kindName)
        #expect(names == ["ordinary start", "first start", "first start after an update", "Recovery start", "restart"])
    }

    @Test func aStartThatRestartedSaysSo() {
        #expect(log(output: nil, restarts: 2).contains("Restarts in this start: 2\n"))
    }

    @Test func withoutOutputTheLogSaysNothingWasPrintedYet() {
        #expect(log(output: nil).hasSuffix("\n(nothing yet)\n"))
        #expect(log(output: "").hasSuffix("\n(nothing yet)\n"))
        #expect(log(output: "\n\n").hasSuffix("\n(nothing yet)\n"))
    }

    @Test func theOutputIsAtMostTheLastFourKiBFromAWholeLine() {
        let lines = (0..<600).map { "[\(String(format: "%7.1f", Double($0)))] line number \($0)" }
        let output = lines.joined(separator: "\n") + "\n"
        #expect(output.utf8.count > SendFeedbackStartLog.outputLimit)
        let limited = SendFeedbackStartLog.limited(output)
        #expect(limited.utf8.count <= SendFeedbackStartLog.outputLimit)
        #expect(output.hasSuffix(limited), "the end is what is kept")
        #expect(limited.hasPrefix("["), "the cut falls at a line, never inside one")
        #expect(limited.hasSuffix("line number 599\n"))
        // What fits is not touched.
        #expect(SendFeedbackStartLog.limited("[0.1] a\n[0.2] b\n") == "[0.1] a\n[0.2] b\n")
    }

    // MARK: What is taken out

    @Test func theKeyOfEveryAssignmentIsMasked() {
        let line = "[    0.000000] Kernel command line: console=ttyS0 androidboot.kernarium.adb_key=\(key) androidboot.kernarium.adb_tcp=1"
        let masked = SendFeedbackStartLog.masked(line + "\n" + line)
        #expect(!masked.contains(key) && !masked.contains("QAAAAGJ4Z0n2Hq"))
        #expect(masked.components(separatedBy: "androidboot.kernarium.adb_key=<masked>").count == 3)
        #expect(masked.contains("androidboot.kernarium.adb_tcp=1"), "what is not identifying stays")
        // The same property, as init prints it.
        let property = SendFeedbackStartLog.masked("[ro.boot.kernarium.adb_key]: [\(key)]")
        #expect(property == "[ro.boot.kernarium.adb_key]: [<masked>]")
        // And in the finished text.
        #expect(!log(output: line).contains(key))
    }

    /// The kernel's early line is cut by the ring from its start: the text may
    /// begin anywhere in the argument that carries the key, even in its value.
    @Test func aLineCutAtTheStartOfTheTextIsMaskedWhereverItWasCut() {
        let argument = "androidboot.kernarium.adb_key=\(key)"
        let rest = " androidboot.kernarium.adb_tcp=1 quiet\n[    1.0] next line"
        for cut in 0...argument.count {
            let fragment = String(argument.dropFirst(cut)) + rest
            let masked = SendFeedbackStartLog.masked(fragment)
            // No eight characters of the key survive, wherever the text begins.
            for start in 0...(key.count - 8) {
                let piece = String(key.dropFirst(start).prefix(8))
                #expect(!masked.contains(piece), "cut at \(cut) kept \(piece): \(masked)")
            }
            #expect(masked.hasSuffix(rest), "the lines after the cut one are the log's own (cut at \(cut))")
        }
    }

    @Test func aWholeLineIsNotMistakenForACutOne() {
        let ordinary = "starting service 'netd'...\n[  102.9] init: Service 'vold' (pid 312) exited with status 0"
        #expect(SendFeedbackStartLog.masked(ordinary) == ordinary)
        let timestamp = "[  102.9] init: starting service 'zygote'...\n"
        #expect(SendFeedbackStartLog.masked(timestamp) == timestamp)
        #expect(SendFeedbackStartLog.masked("pid=312 status=0") == "pid=312 status=0")
    }

    @Test func theSerialNumberTheAddressAndThePathsAreMasked() {
        let containers = ["/private/var/mobile/Containers/Data/Application/1B2C3D4E-0000-4A5B-8C9D-0123456789AB"]
        let output = """
            [    0.0] Kernel command line: androidboot.serialno=K7QX2M9T4R \
            --session-record=/var/mobile/Containers/Data/Application/1B2C3D4E-0000-4A5B-8C9D-0123456789AB/Library/x
            [    5.1] eth0: link up, address aa:bb:cc:dd:ee:ff and 0A:1B:2C:3D:4E:5F
            [    5.2] opened /Users/NAME/Library/Containers/x
            [    5.3] [ro.boot.serialno]: [K7QX2M9T4R]
            """
        let masked = SendFeedbackStartLog.masked(output, containers: containers)
        #expect(!masked.contains("K7QX2M9T4R"))
        #expect(masked.contains("androidboot.serialno=<masked>") && masked.contains("[ro.boot.serialno]: [<masked>]"))
        #expect(!masked.localizedCaseInsensitiveContains("aa:bb:cc") && !masked.contains("0A:1B:2C"))
        #expect(masked.contains("<address>"))
        #expect(!masked.contains("1B2C3D4E") && masked.contains("<container>"))
        #expect(!masked.contains("/Users/NAME") && masked.contains("/Users/<user>"))
    }

    // MARK: The sheet's rules

    @Test func theTipFollowsTheStateTheSheetWasOpenedIn() {
        var form = SendFeedbackForm(mailAvailable: true, running: false)
        #expect(form.tip == .home, "from Home: nothing runs and nothing starts")
        form.starting = true
        #expect(form.tip == .starting)
        // A bug report and a screenshot stay unavailable during a start.
        #expect(!form.canMakeBugReport && !form.canTakeScreenshot && form.canAddMedia)
        // The start finishes while the sheet is open.
        form.starting = false
        form.running = true
        #expect(form.tip == .running && form.canMakeBugReport && form.canTakeScreenshot)
    }

    @Test func theLogAloneIsNotWhatClosingAsksAbout() {
        var form = SendFeedbackForm(mailAvailable: true, running: false)
        form.starting = true
        form.attachments.add(SendFeedbackAttachment(
            kind: .startLog, url: URL(fileURLWithPath: "/staged/start-log.txt"), bytes: 900))
        #expect(!form.hasContent, "a sheet that holds only what it attached itself closes without asking")
        #expect(form.attachments.count == 1 && form.attachments.totalBytes == 900, "but the mail counts it")
        form.details = "x"
        #expect(form.hasContent)
        form.details = ""
        form.attachments.add(SendFeedbackAttachment(
            kind: .photo, url: URL(fileURLWithPath: "/staged/photo.jpg"), bytes: 10))
        #expect(form.hasContent)
    }
}
