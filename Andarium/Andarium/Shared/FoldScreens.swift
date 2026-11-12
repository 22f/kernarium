// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

// The screens other than the session on a display that folds: the decisions
// are `AndariumCore`'s (`FoldHalves`, `FoldSheet`), and each modifier here
// does nothing where the display has no fold band.

extension View {
    /// Keeps the view to the place of `screen` on the inner display (see
    /// `FoldHalves.area(for:)`): by default the half where the system puts its
    /// own alerts, the leading one upright and the lower one turned, so a card
    /// or a boot step never straddles the fold. The view is padded away from
    /// the rest of the space it has, or reaches past that space to the whole
    /// display (Welcome, flat); elsewhere there is no padding, so it lays out
    /// as it always did. It measures the space it is given and not
    /// the window's safe area, so it holds in a sheet and under a navigation
    /// bar. Apply it to a view that fills the space it is shown in.
    func onCardHalf(_ screen: FoldScreen = .card) -> some View { modifier(OnCardHalf(screen: screen)) }

    /// A sheet's presentation on the turned inner display. Upright the system
    /// keeps a sheet to the leading half while the device is bent. Turned,
    /// bent or flat, the sheet rises to the fold and no further: it covers the
    /// lower half only, the upper half stays in sight, and nothing moves when
    /// the hinge crosses between bent and flat. `reachesBehind` lets touches
    /// reach what the sheet leaves uncovered, as the session's Settings does
    /// over the guest. Apply it outside the sheet's own presentation
    /// modifiers. It is handed the model and does not read the environment:
    /// a sheet's presentation preferences are read before the sheet inherits
    /// its presenter's environment, on a Mac and in a harness that gives the
    /// presenter no model. A sheet decides once, when it is presented, whether
    /// it takes the turned presentation; turning the display with it open
    /// leaves its content, and what the person has typed in it, in place.
    func foldSheet(_ model: AppModel, reachesBehind: Bool = false) -> some View {
        modifier(FoldSheetPresentation(model: model, reachesBehind: reachesBehind))
    }
}

private struct OnCardHalf: ViewModifier {
    let screen: FoldScreen
    /// Optional: a card shown without a model, as a harness does, is laid out
    /// as it always was.
    @Environment(AppModel.self) private var model: AppModel?
    /// The space the view is given, in window coordinates, once measured.
    @State private var measured: CGRect?

    func body(content: Content) -> some View {
        let fold = model?.fold
        let halves = fold.flatMap(FoldHalves.init)
        // The view fills the space it is given, then keeps to the place of its
        // screen: a card in a stack centres on its half and not on that space.
        // With no fold band the frame proposes nothing and the padding is
        // zero. Until the space is measured the window's safe area stands for
        // it.
        let folds = halves != nil
        content
            .frame(maxWidth: folds ? .infinity : nil, maxHeight: folds ? .infinity : nil)
            .padding(Self.padding(halves?.insets(for: screen, in: (measured ?? fold?.safeArea) ?? .zero)))
            .onGeometryChange(for: CGRect.self) { $0.frame(in: .global) } action: { frame in
                if folds, measured != frame { measured = frame }
            }
    }

    private static func padding(_ insets: FoldInsets?) -> EdgeInsets {
        guard let insets else { return EdgeInsets() }
        return EdgeInsets(top: insets.top, leading: insets.leading, bottom: insets.bottom, trailing: insets.trailing)
    }
}

private struct FoldSheetPresentation: ViewModifier {
    let model: AppModel
    let reachesBehind: Bool
    /// Decided when the sheet is presented and never again: a branch on the
    /// live reading would move the content to another place in the view tree
    /// when the display turns, and SwiftUI would build it anew.
    @State private var turned: Bool
    /// The detent's height, kept up to date while the display is turned.
    @State private var height: CGFloat

    init(model: AppModel, reachesBehind: Bool) {
        self.model = model
        self.reachesBehind = reachesBehind
        let height = model.fold.flatMap(FoldSheet.detentHeight)
        _turned = State(initialValue: height != nil)
        _height = State(initialValue: height ?? 0)
    }

    func body(content: Content) -> some View {
        if turned {
            content
                .presentationSizing(.page)
                .presentationDetents([.height(height)])
                .presentationBackgroundInteraction(reachesBehind ? .enabled : .disabled)
                .onChange(of: model.fold.flatMap(FoldSheet.detentHeight)) { _, next in
                    if let next, next != height { height = next }
                }
        } else {
            content
        }
    }
}
