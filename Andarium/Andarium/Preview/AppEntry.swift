// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import SwiftUI
import TipKit
import UIKit

/// The Simulator preview's entry. `-screen <name>` opens any screen or state
/// directly with mock data, so a simulator driven only by `simctl` reaches
/// every flow. The arguments are the keys `fromArguments()` reads.
struct AppEntry {
    var screen: String
    /// Boot screens animate and continue into the session instead of holding.
    var play: Bool
    var lowSpace: Bool
    var resetTips: Bool
    /// The boot step a frozen boot screen shows, from 0; the screen's own
    /// when absent.
    var bootStage: Int?
    /// The Microphone switch asks the Simulator's own permission instead of
    /// taking the access as given, so `simctl privacy` and the system's
    /// alert drive it as they drive the device's.
    var systemMicrophone: Bool
    /// The Location switch asks the Simulator's own permission, as
    /// `systemMicrophone` does for the microphone: `simctl privacy` sets it
    /// and the system's alert asks.
    var systemLocation: Bool
    /// How many CPU readings the stats overlay and Settings' Stats rows
    /// start with, and hold: 0 is a session before its first reading. Absent,
    /// the mock readings run on.
    var statsReadings: Int?
    /// Graphics memory in use, in MiB, held with `statsReadings`: a figure
    /// near the budget turns its bar orange.
    var statsVramMiB: Int?
    /// The mock device: "iphone-18-pro-max", "ipad-mini-6" or "mac-64-gib"
    /// (a ruler of about a thousand positions); absent, the idiom's own mock.
    var device: String?
    /// The guest's memory, in MiB, instead of the default a new guest gets.
    var memoryMiB: Int?
    /// "light" or "dark" draws the preview in that appearance whatever the
    /// Simulator's own, so a capture leaves the device's setting alone.
    var colorScheme: String?

    static func fromArguments() -> AppEntry {
        let defaults = UserDefaults.standard
        return AppEntry(
            screen: defaults.string(forKey: "screen") ?? "home",
            play: defaults.bool(forKey: "play"),
            lowSpace: defaults.bool(forKey: "lowSpace"),
            resetTips: defaults.bool(forKey: "resetTips"),
            bootStage: defaults.string(forKey: "bootStage").flatMap { Int($0) },
            systemMicrophone: defaults.bool(forKey: "systemMicrophone"),
            systemLocation: defaults.bool(forKey: "systemLocation"),
            statsReadings: defaults.string(forKey: "statsReadings").flatMap { Int($0) },
            statsVramMiB: defaults.string(forKey: "statsVramMiB").flatMap { Int($0) },
            device: defaults.string(forKey: "device"),
            memoryMiB: defaults.string(forKey: "memoryMiB").flatMap { Int($0) },
            colorScheme: defaults.string(forKey: "colorScheme")
        )
    }

    /// Where Home scrolls to for a screen that shows a lower section.
    private static let scrollTargets: [String: String] = [
        "home-options": "erase", "home-recovery-erase": "erase", "start-recovery": "erase", "erase": "erase",
        "home-new-recovery": "erase",
        "home-display": "display",
        "home-capabilities": "capabilities", "home-mic-on": "capabilities", "home-mic-denied": "capabilities",
        "home-haptics-off": "capabilities",
        "home-mic-alert": "capabilities", "home-gyroscope-unavailable": "capabilities",
        "home-location-on": "capabilities", "home-location-denied": "capabilities",
        "home-location-approximate": "capabilities", "home-location-restricted": "capabilities",
        "home-location-services-off": "capabilities", "home-location-alert": "capabilities",
        "home-location-approximate-alert": "capabilities", "home-location-restricted-alert": "capabilities",
        "home-location-services-off-alert": "capabilities", "home-location-ask": "capabilities",
        "home-backup": "backup",
        "home-jit": "jit", "home-jit-no-debugger": "jit", "home-jit-fallback": "jit",
    ]

    /// Where Settings scrolls to for a screen that shows a lower section.
    private static let settingsScrolls: [String: String] = [
        "settings-stats": "stats",
    ]

    /// The page of Info a screen opens on.
    private static let infoPages: [String: InfoPage] = [
        "info-acknowledgements": .acknowledgements,
        "info-license": .license("mesa"),
        "info-license-gpl": .license("linux"),
        "whatsnew-history": .whatsNew,
    ]

    var hints: ScreenHints {
        ScreenHints(
            scrollTarget: Self.scrollTargets[screen] ?? SnapshotPreview.scrollTargets[screen],
            infoPage: Self.infoPages[screen]
        )
    }

    /// The side-keys tip. A screenshot of the tip shows it whatever the
    /// datastore says; other session screenshots leave it out.
    func configureTips() {
        if resetTips { try? Tips.resetDatastore() }
        if screen == "session-tip" {
            Tips.showAllTipsForTesting()
        } else if screen.hasPrefix("session") || screen.hasPrefix("settings") || screen.hasPrefix("files")
            || screen.hasPrefix("recovery") || screen.hasPrefix("boot")
            || screen.hasPrefix("space-pause") || Self.operationScreens.contains(screen) {
            Tips.hideAllTipsForTesting()
        }
        try? Tips.configure([.displayFrequency(.immediate)])
    }

    func makeModel() -> AppModel {
        CaptureState.begin(screen: screen)
        applyColorScheme()
        let lowSpace = lowSpace || ["create-low-space", "start-low-space", "home-low-space"].contains(screen)
        let host = WhatsNewPreview.host(for: screen) ?? SnapshotPreview.host(for: screen)
            ?? PreviewData.host(lowSpace: lowSpace, device: device)
        var guest: Guest? = PreviewData.sampleGuest(on: host)
        if let memoryMiB { guest?.memoryMiB = memoryMiB }
        if ["welcome", "create", "create-low-space"].contains(screen) { guest = nil }
        let model = AppModel(
            guest: guest, host: host, system: PreviewData.system, capabilities: .preview,
            createRequirementBytes: PreviewData.createRequirementBytes
        )
        let files = MockGuestFiles()
        let browser = FileBrowserModel(backend: files)
        MockFolderProbe.browser = browser
        model.fileBrowser = browser
        let freeze = !play
        model.jitAvailable = true
        // Only an iPhone has a Taptic Engine for the guest's haptics: an iPad's
        // preview, like the device, has no Haptics row.
        model.hapticsAvailable = host.name == "iPhone"

        func boot(_ kind: BootKind, stage: Int, seconds: Int) {
            model.frozenBootStage = freeze ? (bootStage ?? stage) : nil
            model.frozenElapsed = seconds
            model.phase = .booting(kind)
        }

        switch screen {
        case "home-new", "home-new-recovery":
            model.guest = Guest.fresh(on: host)
            model.plan = .firstStart
        case "home-grow":
            model.guest?.pendingCapacityGiB = 24
        case "resize":
            model.resizePreset = 24
        case "home-max":
            model.guest?.capacityGiB = Limits.capacityRangeGiB.upperBound
        case "home-update":
            model.plan = .update(from: PreviewData.previousSystemRelease, to: PreviewData.systemRelease)
        case "home-update-failed":
            model.plan = .unfinishedUpdate(to: PreviewData.systemRelease, canRetry: true, canRestore: true)
            model.planAfterRestore = .update(from: PreviewData.previousSystemRelease, to: PreviewData.systemRelease)
        case "home-update-failed-restore":
            model.plan = .unfinishedUpdate(to: PreviewData.systemRelease, canRetry: false, canRestore: true)
            model.planAfterRestore = .update(from: PreviewData.previousSystemRelease, to: PreviewData.systemRelease)
        case "home-blocked":
            model.plan = .blocked(.olderAndroid)
        case "home-closed":
            model.notice = .closedInBackground
        case "home-network-cellular":
            model.previewNetworkNotice(.cellular)
        case "home-network-wifi":
            model.previewNetworkNotice(.wifi)
        case "home-ended":
            // The card's own words, from the runtime's record of a kernel panic.
            if let notice = SessionRecord(text: """
                uml-session 1
                state ended
                end guest-panic
                status 6
                pid 1
                message Attempted to kill init! exitcode=0x0000000b

                """)?.notice(endedOutOfForeground: false) {
                model.notice = .sessionEnded(notice)
            }
        case "home-memory-warning", "system-memory-warning":
            model.guest = PreviewData.highMemoryGuest(on: host)
        case "home-options", "home-display", "home-capabilities", "home-backup", "system-edit", "home-location-ask":
            break
        case "home-jit":
            model.jitOn = true
        case "home-jit-no-debugger":
            model.jitOn = true
            model.previewJitDebugger = false
        case "home-jit-fallback":
            model.jitOn = true
            model.previewJitDebugger = false
            model.jitLastStart = .noDebugger
        case "home-mic-on":
            model.guest?.microphone = true
            model.microphoneState = .granted
        case "home-mic-denied", "home-mic-alert":
            model.microphoneState = .denied
        case "home-gyroscope-unavailable":
            model.motionSensorsAvailable = false
        case "home-haptics-off":
            model.guest?.haptics = false
        case "home-location-on":
            model.guest?.location = true
            model.locationAuthorization = .precise
        case "home-location-approximate", "home-location-approximate-alert":
            model.guest?.location = true
            model.locationAuthorization = .approximate
        case "home-location-denied", "home-location-alert":
            model.locationAuthorization = .denied
        case "home-location-restricted", "home-location-restricted-alert":
            model.locationAuthorization = .restricted
        case "home-location-services-off", "home-location-services-off-alert":
            model.locationAuthorization = .servicesOff
        case "busy-create":
            model.busy = BusyState(title: String(localized: "Creating virtual machine"), detail: String(localized: "Setting up Andarium with \(Formats.size(gib: 16))."), progress: 0.42)
        case "busy-erase":
            model.busy = BusyState(title: String(localized: "Erasing virtual machine"), detail: String(localized: "Deleting everything the virtual machine stored."), progress: 0.65)
        case "boot":
            // The start applied the growth as it began.
            model.guest?.setCapacity(24)
            model.lifecycle.grewFromGiB = model.guest?.applyPendingGrowth()
            boot(.normal, stage: 3, seconds: 9)
        case "boot-single-vcpu":
            model.guest?.cores = 1
            boot(.normal, stage: 3, seconds: 9)
        case "boot-first":
            model.guest = model.guest?.erased
            model.plan = .firstStart
            boot(.firstStart, stage: 3, seconds: 74)
        case "boot-upgrade":
            model.bootFrom = PreviewData.previousSystemRelease
            boot(.upgrade, stage: 3, seconds: 51)
        case "boot-recovery":
            boot(.recovery, stage: 3, seconds: 7)
        case "boot-restart":
            boot(.restart, stage: 3, seconds: 8)
        case "boot-jit":
            model.jitOn = true
            model.preparingJit = true
            boot(.normal, stage: 0, seconds: 3)
        case "boot-jit-fallback":
            model.jitOn = true
            model.jitLastStart = .noDebugger
            boot(.normal, stage: 1, seconds: 4)
        case "session", "session-tip", "session-open", "session-keyboard", "session-stats",
             "session-power", "session-setup", "session-off", "session-left", "session-volume",
             "session-graphics-notice", "session-thermal-notice", "session-network-cellular", "session-network-wifi":
            model.session.showStats = screen == "session-stats"
            model.session.sideKeysOpen = ["session-open", "session-off", "session-left", "session-volume"].contains(screen)
            model.sessionPreset = SessionPreset(
                volumePanel: screen == "session-volume",
                powerMenu: screen == "session-power",
                leftEdge: ["session-left", "session-volume"].contains(screen),
                guestScreen: {
                    switch screen {
                    case "session-setup": .setup
                    case "session-off": .off
                    case "session-keyboard": .search
                    default: .home
                    }
                }()
            )
            model.phase = .running(.normal)
            if screen == "session-graphics-notice" { model.previewGraphicsNotice() }
            if screen == "session-thermal-notice" { model.previewThermalNotice() }
            if screen == "session-network-cellular" { model.previewNetworkNotice(.cellular) }
            if screen == "session-network-wifi" { model.previewNetworkNotice(.wifi) }
        case "settings", "settings-adb-off", "settings-adb-no-address", "settings-adb", "settings-stats",
             "install-apk", "install-done", "install-failed", "install-not-apk", "install-bundle", "bug-report":
            let session = model.session
            switch screen {
            case "settings-adb":
                session.adb = true
                session.adbAddress = SessionState.previewAddress
            case "settings-adb-no-address":
                session.adb = true
            case "settings-adb-off":
                session.usbDebugging = false
            default:
                break
            }
            // The guest operations play on the full-screen overlay with
            // Settings already gone.
            let operation = Self.operationScreens.contains(screen)
            model.sessionPreset = SessionPreset(
                settings: !operation,
                settingsLarge: Self.settingsScrolls.keys.contains(screen) || screen == "settings",
                settingsScroll: Self.settingsScrolls[screen]
            )
            model.phase = .running(.normal)
            // The real operation lasts a few seconds, too short for a
            // screenshot to catch: hold its overlay part way unless `-play`.
            // A result is what the real operation leaves up, so it always
            // shows as it is.
            if operation && (freeze || Self.resultScreens.contains(screen)) {
                model.busy = Self.operationOverlay(for: screen, model: model)
            }
        case "space-pause", "space-pause-short", "space-pause-shutting-down", "space-pause-pausing",
            "space-pause-running":
            model.phase = .running(.normal)
            model.spacePause = switch screen {
            case "space-pause-short": SpacePause(freeBytes: 1_200 * Limits.bytesPerMiB, stillShort: true)
            case "space-pause-shutting-down": SpacePause(freeBytes: 820 * Limits.bytesPerMiB, working: .shuttingDown)
            case "space-pause-pausing": SpacePause(freeBytes: 820 * Limits.bytesPerMiB, stage: .pausing)
            case "space-pause-running": SpacePause(freeBytes: 820 * Limits.bytesPerMiB, stage: .notPaused)
            default: SpacePause(freeBytes: 820 * Limits.bytesPerMiB)
            }
        case let name where name.hasPrefix("files"):
            MockGuestFiles.present(name, browser: browser, files: files, model: model)
        case "recovery", "recovery-open":
            model.session.sideKeysOpen = screen == "recovery-open"
            model.phase = .running(.recovery)
        case "failed":
            // The failure path a real run takes: a crash after 42 minutes.
            model.fail()
        default:
            break
        }
        WhatsNewPreview.apply(screen: screen, to: model)
        if let statsReadings {
            model.session.readout = Readout(
                previewReadings: statsReadings, memoryMiB: model.guest?.memoryMiB ?? Limits.defaultMemoryRangeMiB.lowerBound
            )
            if let statsVramMiB { model.session.readout.vramUsedBytes = UInt64(statsVramMiB) * Limits.bytesPerMiB }
        }
        if systemMicrophone { useSystemMicrophone(in: model) }
        if systemLocation { useSystemLocation(in: model) }
        SnapshotPreview.apply(screen: screen, to: model)
        return model
    }

    /// The screens that show a guest operation on the full-screen overlay.
    private static let operationScreens: Set<String> = [
        "bug-report", "install-apk", "install-done", "install-failed", "install-not-apk", "install-bundle",
    ]
    /// The ones that show how an install ended.
    private static let resultScreens: Set<String> = [
        "install-done", "install-failed", "install-not-apk", "install-bundle",
    ]

    /// A guest operation's overlay as it looks part way, or at its end: what
    /// `createBugReport` and `installPicked` show, held still.
    private static func operationOverlay(for screen: String, model: AppModel) -> BusyState {
        let cancel = BusyCancel {}
        let dismiss = BusyCancel { model.busy = nil }
        let file = PreviewData.sampleApkName
        switch screen {
        case "bug-report":
            return BusyState(
                title: String(localized: "Creating bug report"),
                detail: String(localized: "The virtual machine is collecting its logs and state. This can take several minutes."),
                progress: 0.6, cancel: cancel
            )
        case "install-done":
            return BusyState(
                title: String(localized: "App installed"), detail: String(localized: "\(file) is installed in the virtual machine."),
                result: BusyResult(failed: false, dismiss: dismiss)
            )
        case "install-failed":
            return BusyState(
                title: AppModel.installFailure,
                detail: PackageInstallMessages.message(forCode: "FAILED_NO_MATCHING_ABIS"),
                result: BusyResult(failed: true, dismiss: dismiss)
            )
        case "install-not-apk":
            return BusyState(
                title: AppModel.installFailure, detail: InstallRefusal.notApk.message,
                result: BusyResult(failed: true, dismiss: dismiss)
            )
        case "install-bundle":
            return BusyState(
                title: AppModel.installFailure, detail: InstallRefusal.bundle.message,
                result: BusyResult(failed: true, dismiss: dismiss)
            )
        default:
            return BusyState(
                title: String(localized: "Installing app"),
                detail: "\(file) \u{00B7} \(Formats.size(bytes: PreviewData.sampleApkSizeBytes))",
                progress: 0.45, cancel: cancel
            )
        }
    }

    /// Every window of the process takes `colorScheme`'s appearance as it
    /// becomes key, before its first frame.
    private func applyColorScheme() {
        let style: UIUserInterfaceStyle
        switch colorScheme {
        case "dark": style = .dark
        case "light": style = .light
        default: return
        }
        NotificationCenter.default.addObserver(
            forName: UIWindow.didBecomeKeyNotification, object: nil, queue: .main
        ) { _ in
            MainActor.assumeIsolated {
                for case let scene as UIWindowScene in UIApplication.shared.connectedScenes {
                    for window in scene.windows { window.overrideUserInterfaceStyle = style }
                }
            }
        }
    }

    /// The device's microphone conversation against the Simulator's
    /// permission, which is read again whenever the app becomes active.
    private func useSystemMicrophone(in model: AppModel) {
        model.microphone = MicrophoneController(model: model, source: SimulatorMicrophoneSource()) { _ in }
        NotificationCenter.default.addObserver(
            forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main
        ) { _ in
            Task { @MainActor in model.microphone?.refresh() }
        }
    }

    /// The device's location conversation against the Simulator's
    /// permission, which is read again whenever the app becomes active.
    private func useSystemLocation(in model: AppModel) {
        model.location = LocationController(model: model, source: SystemLocationAccess()) {}
        model.location?.refresh()
        NotificationCenter.default.addObserver(
            forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main
        ) { _ in
            Task { @MainActor in model.location?.refresh() }
        }
    }

    /// Sheets and dialogs open after the first frame so SwiftUI presents them
    /// the way a tap would.
    @MainActor
    func applyDeferred(to model: AppModel) async {
        try? await Task.sleep(for: .milliseconds(450))
        switch screen {
        case "create", "create-low-space":
            model.sheet = .create
        case "resize":
            model.sheet = .resize
        case "system-edit", "system-memory-warning":
            model.sheet = .system
        case "home-mic-alert":
            model.setMicrophone(true)
        case "home-location-alert", "home-location-restricted-alert", "home-location-services-off-alert",
             "home-location-ask":
            model.setLocation(true)
        case "home-location-approximate-alert":
            model.permissionPrompt = .location
        case "erase":
            model.confirmingErase = true
        case "info", "info-acknowledgements", "info-license", "info-license-gpl", "whatsnew-info",
             "whatsnew-history":
            model.sheet = .info
        case "whatsnew-open", "whatsnew-skipped-open":
            model.sheet = .whatsNew
        case "start-low-space":
            model.start(.normal)
        case "files-review":
            try? await Task.sleep(for: .milliseconds(1100))
            model.fileBrowser?.reviewingSelection = true
        case "files-share":
            try? await Task.sleep(for: .milliseconds(1100))
            if let browser = model.fileBrowser, let transfer = browser.transfer { browser.share(transfer) }
        case "alert-error":
            model.alert = AppAlert(
                title: String(localized: "Settings not saved"),
                message: CocoaError(.fileWriteOutOfSpace).localizedDescription
            )
        case "bug-report" where play:
            try? await Task.sleep(for: .milliseconds(450))
            model.createBugReport()
        case "install-apk" where play:
            try? await Task.sleep(for: .milliseconds(450))
            model.installPicked(PreviewData.sampleApk())
        default:
            await SnapshotPreview.applyDeferred(screen: screen, to: model)
        }
    }
}
