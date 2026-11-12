// SPDX-License-Identifier: GPL-2.0-or-later

#if TIP_JAR

import Foundation

/// What Buy me tokens asks of the build and of the App Store, in one place a
/// test or the Simulator preview replaces. A launch of the application has the
/// live ones.
struct TipJarServices {
    /// The product identifiers, from Info.plist; nil when they are absent or
    /// are not four well formed, distinct identifiers, which hides the entrance.
    var products: @MainActor () -> TipJarProducts? = { TipJarConfiguration.products }
    var store = TipJarStoreServices.live
    /// How long the store may take to answer before the sheet says it did not.
    var timeout = Duration.seconds(20)
    #if !APP_STORE
    // The Simulator preview's hook. The App Store product has neither the
    // property nor the code that reads it.
    /// Opens the sheet once Home has settled, as a launch of the Simulator
    /// preview asks; never in the application.
    var previewOpensSheet = false
    /// Runs when the sheet appears: the Simulator preview plays a purchase with it.
    var previewPlays: @MainActor (TipJarModel) async -> Void = { _ in }
    #endif
}

extension AppModel {
    /// The Buy me tokens entrance is offered: the identifiers are configured
    /// and a virtual machine is set up.
    var tipJarOffered: Bool { guest != nil && tipJarServices.products() != nil }

    /// A model for one opening of the sheet; nil when the identifiers are not configured.
    func makeTipJarModel() -> TipJarModel? {
        tipJarServices.products().map { TipJarModel(products: $0, store: tipJarServices.store, timeout: tipJarServices.timeout) }
    }
}

#endif
