// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

// The screens other than the session on a display that folds: the decisions
// are `AndariumCore`'s (`FoldHalves`, `FoldSheet`), and each modifier here
// does nothing where the display has no fold band.

extension View {
    /// Keeps the view to the half of the inner display where the system puts
    /// its own alerts (the leading one upright, the lower one turned, bent or
    /// flat alike), so a card or a boot step never straddles the fold. The
    /// view is padded away from the other half; elsewhere there is no padding,
    /// so it lays out as it always did. Apply it to a view that fills the safe
    /// area.
    func onCardHalf() -> some View { modifier(OnCardHalf()) }

    /// A sheet's presentation on the turned inner display. Upright the system
    /// keeps a sheet to the leading half while the device is bent. Turned,
    /// bent or flat, the sheet rises to the fold and no further: it covers the
    /// lower half only, the upper half stays in sight, and nothing moves when
    /// the hinge crosses between bent and flat. `reachesBehind` lets touches
    /// reach what the sheet leaves uncovered, as the session's Settings does
    /// over the guest. Apply it outside the sheet's own presentation
    /// modifiers, and inside the model's environment.
    func foldSheet(reachesBehind: Bool = false) -> some View {
        modifier(FoldSheetPresentation(reachesBehind: reachesBehind))
    }
}

private struct OnCardHalf: ViewModifier {
    @Environment(AppModel.self) private var model

    func body(content: Content) -> some View {
        let safe = model.fold?.safeArea
        let half = model.fold.flatMap(FoldHalves.init)?.card
        // The view fills the space it is given, then keeps to the half: a
        // card in a stack centres on the half and not on the safe area. With
        // no fold band the frame proposes nothing and the padding is zero.
        let folds = half != nil
        content
            .frame(maxWidth: folds ? .infinity : nil, maxHeight: folds ? .infinity : nil)
            .padding(Self.insets(of: half, in: safe))
    }

    static func insets(of half: CGRect?, in safe: CGRect?) -> EdgeInsets {
        guard let half, let safe else { return EdgeInsets() }
        return EdgeInsets(
            top: max(0, half.minY - safe.minY), leading: max(0, half.minX - safe.minX),
            bottom: max(0, safe.maxY - half.maxY), trailing: max(0, safe.maxX - half.maxX))
    }
}

private struct FoldSheetPresentation: ViewModifier {
    @Environment(AppModel.self) private var model
    let reachesBehind: Bool

    func body(content: Content) -> some View {
        if let height = model.fold.flatMap(FoldSheet.detentHeight) {
            content
                .presentationSizing(.page)
                .presentationDetents([.height(height)])
                .presentationBackgroundInteraction(reachesBehind ? .enabled : .disabled)
        } else {
            content
        }
    }
}
