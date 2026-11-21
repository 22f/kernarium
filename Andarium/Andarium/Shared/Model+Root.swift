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
