// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// The root switch on the model. The guest's root state is `Guest.root`
/// (AndariumCore); the rules the owner may choose differently are the three
/// `Root...Rule` enums there.
extension AppModel {
    /// The switch as Home shows it: on is the person's choice, and a pause that
    /// the application put on root after a failed start does not turn it off.
    var rootOn: Bool { guest?.root.on ?? false }

    /// What Home owes the person about root, if anything.
    var rootAttention: RootState.Attention? { guest?.root.attention }

    /// The Root switch. Turning root off applies at once and applies to the
    /// next start; turning it on asks first (`rootWarningShowing`), and the
    /// switch stays off until the person confirms.
    func setRoot(_ on: Bool) {
        guard guest != nil, rootAvailable else { return }
        if on {
            guard !rootOn else { return }
            rootWarningShowing = true
        } else {
            guard rootOn || rootAttention != nil else { return }
            guest?.root.setOn(false)
            persistNow()
        }
    }

    /// The warning's Turn On.
    func confirmRoot() {
        rootWarningShowing = false
        guard guest != nil, rootAvailable else { return }
        guest?.root.setOn(true)
        persistNow()
    }

    /// The warning's Cancel.
    func cancelRootWarning() {
        rootWarningShowing = false
    }
}

/// The boot screen's Magisk app step.
extension AppModel {
    /// Whether the start under way booted the Magisk ramdisk and may need the
    /// app: a normal boot (recovery has no root), the guest's `rooted` flag
    /// and a release that holds both of root's files.
    private var bootedRooted: Bool {
        guard let guest, guest.root.rooted, rootAvailable, let driver else { return false }
        return driver.rootRelease != nil && driver.rootApkURL != nil
    }

    /// After Android has booted and before the boot screen ends: with root,
    /// asks the guest whether the official Magisk app is there and puts it in
    /// when it is not (`RootAppStep`). `installing` is called once, when an
    /// install begins and not before, so that the screen adds its fifth step
    /// only for a boot that installs. A failure or the hard bound is told to the
    /// person in an alert and the boot goes on with root on and no app; the
    /// next start tries again. Without root the same point removes the app only
    /// where `RootOffRule` says so.
    func runRootAppStep(kind: BootKind, installing: @escaping @MainActor () -> Void) async {
        guard kind != .recovery, let driver, let shell = driver.guestShell else { return }
        guard bootedRooted, let release = driver.rootRelease, let apk = driver.rootApkURL,
            let state = guest?.root
        else {
            await runRootOffStep(driver: driver, shell: shell)
            return
        }
        let outcome = await RootAppStep.run(
            shell: shell, release: release, state: state,
            installing: { @MainActor [weak self] in
                self?.rootAppInstalling = true
                installing()
            },
            install: { [weak self] in
                guard let self else { throw CancellationError() }
                try await self.installRootApp(at: apk)
            })
        switch outcome {
        case let .notNeeded(next), let .installed(next):
            if next != state {
                guest?.root = next
                persistNow()
            }
        case .failed:
            alert = AppAlert(title: RootWords.appFailedTitle, message: RootWords.appFailedMessage)
        }
    }

    /// Puts the release's Magisk app in the guest over the application's adb
    /// install path, which replaces an older copy and keeps its data.
    private func installRootApp(at url: URL) async throws {
        guard let driver else { throw CancellationError() }
        _ = try await driver.installApk(at: url, bypassLowTargetSdkBlock: false, update: { _ in })
    }

    /// A start without root, when `RootOffRule` removes the app the person
    /// no longer has root for. The default rule keeps it, and this asks nothing.
    private func runRootOffStep(driver: any RuntimeDriver, shell: any GuestShell) async {
        guard let state = guest?.root, RootOffRule.removesApp(state: state) else { return }
        guard await RootOffRule.removeAppIfAny(shell: shell, limit: .seconds(15)) else { return }
        guest?.root.appEverHandled = false
        persistNow()
    }
}
