// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import AndariumCore
import SwiftUI
import UIKit

extension AppModel {
    /// The receiver, when Send feedback has one: both entrances need it.
    var feedbackAddress: String? { feedbackServices.address() }

    /// A virtual machine runs.
    var virtualMachineRuns: Bool {
        if case .running = phase { return true }
        return false
    }

    /// The basic information that ends the mail.
    func feedbackInformation() -> SendFeedbackInformation {
        let bundle = Bundle.main
        let machine = guest.map { guest in
            SendFeedbackInformation.VirtualMachine(
                vcpus: guest.cores, memoryMiB: guest.memoryMiB, display: retina ? "Retina" : "1x",
                graphicsMiB: host.memory.graphicsMemoryMiB(forMemory: guest.memoryMiB, vCPUs: guest.cores))
        }
        return SendFeedbackInformation(
            appVersion: bundle.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "?",
            build: bundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "?",
            configuration: Self.buildConfiguration,
            deviceModel: Self.machineIdentifier,
            systemVersion: UIDevice.current.systemVersion,
            language: bundle.preferredLocalizations.first ?? Locale.preferredLanguages.first ?? "en",
            virtualMachine: machine,
            androidRelease: system.release,
            runningSeconds: virtualMachineRuns ? feedbackServices.runningSeconds(self) : nil,
            freeSpaceBytes: host.freeStorageBytes,
            thermalState: Self.thermalName(ProcessInfo.processInfo.thermalState))
    }

    private static var buildConfiguration: String {
        #if APP_STORE
        "AppStore"
        #elseif DEBUG
        "Debug"
        #else
        "Release"
        #endif
    }

    /// The hardware's machine identifier ("iPhone18,1"); in the Simulator, the
    /// model it plays.
    private static var machineIdentifier: String {
        if let simulated = ProcessInfo.processInfo.environment["SIMULATOR_MODEL_IDENTIFIER"] { return simulated }
        var size = 0
        sysctlbyname("hw.machine", nil, &size, nil, 0)
        guard size > 0 else { return "unknown" }
        var name = [CChar](repeating: 0, count: size)
        sysctlbyname("hw.machine", &name, &size, nil, 0)
        return String(decoding: name.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }

    private static func thermalName(_ state: ProcessInfo.ThermalState) -> String {
        switch state {
        case .nominal: "nominal"
        case .fair: "fair"
        case .serious: "serious"
        case .critical: "critical"
        @unknown default: "unknown"
        }
    }

    /// Makes a bug report for the Send feedback sheet: the same work as the
    /// Settings row, under the same overlay, which the sheet draws over
    /// itself instead of the screens. `failed` says why it did not come.
    /// Returns false when nothing started.
    @discardableResult
    func createBugReportForFeedback(
        finished: @escaping @MainActor (URL) -> Void, failed: @escaping @MainActor (String) -> Void
    ) -> Bool {
        guard canCreateBugReport, busy == nil else { return false }
        busyOverSheet = true
        runOperation(
            Self.bugReportState, failure: Self.bugReportFailure, failed: failed, bugReportWork(), finished: finished)
        return true
    }

    /// The overlay of a task the Send feedback sheet started.
    var feedbackBusy: BusyState? { busyOverSheet ? busy : nil }
}

#endif
