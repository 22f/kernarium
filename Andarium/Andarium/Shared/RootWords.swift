// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// Every word of the root switch, in one place, so that the wording the owner
/// picks is a change here and in the catalog. The UI says "Android" or
/// "virtual machine", and names Magisk because the person grants root there.
enum RootWords {
    // MARK: Home

    static let sectionTitle: LocalizedStringKey = "Root"
    static let rowTitle = String(localized: "Root access")
    static let note = String(localized: "Apps you allow in Magisk can run as root. Takes effect at the next start.")
    /// The line under the switch while a failed start has paused root.
    static let paused = String(localized: "Paused after a failed start")

    // MARK: The warning before root is turned on

    static let warningTitle = String(localized: "Turn on root access?")
    static let warningBody = String(
        localized:
            "Apps you allow in Magisk can read and change everything in the virtual machine, including other apps’ data. Some apps don’t run on a rooted device. The official Magisk app is installed at the next start. You can turn root off whenever the virtual machine is stopped."
    )
}
