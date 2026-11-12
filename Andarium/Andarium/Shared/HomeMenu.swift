// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL || TIP_JAR

import SwiftUI

// Home's toolbar menu. Info is the plain button it has always been; each
// feature that is compiled in and available adds its item, and with none the
// menu does not exist (a build without either feature has no menu code at all).

/// What the menu holds, in order.
enum HomeMenuItem: Equatable {
    case info
    #if FEEDBACK_MAIL
    case sendFeedback
    #endif
    #if TIP_JAR
    case buyMeTokens
    #endif
}

/// The glyphs of the menu and of its items; the designer's picks replace these
/// lines.
enum HomeMenuSymbols {
    /// Provisional.
    static let menu = "ellipsis"
    static let info = "info.circle"
    #if FEEDBACK_MAIL
    static let sendFeedback = "envelope"
    #endif
}

extension AppModel {
    /// The menu's items now: Info, then Send feedback when its receiver is
    /// configured, then Buy me tokens when a virtual machine is set up and the
    /// product identifiers are configured.
    var homeMenuItems: [HomeMenuItem] {
        var items: [HomeMenuItem] = [.info]
        #if FEEDBACK_MAIL
        if feedbackAddress != nil { items.append(.sendFeedback) }
        #endif
        #if TIP_JAR
        if tipJarOffered { items.append(.buyMeTokens) }
        #endif
        return items
    }

    /// What choosing an item does.
    func choose(_ item: HomeMenuItem) {
        switch item {
        case .info: sheet = .info
        #if FEEDBACK_MAIL
        case .sendFeedback: feedbackSheetOpen = true
        #endif
        #if TIP_JAR
        case .buyMeTokens: tipJarSheetOpen = true
        #endif
        }
    }
}

/// Home's trailing toolbar item: the plain Info button when Info is all there
/// is, otherwise one menu that holds it with the others.
struct HomeToolbarMenu: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let items = model.homeMenuItems
        if items == [.info] {
            HomeInfoButton()
        } else {
            Menu {
                ForEach(items, id: \.self) { item in
                    Button { model.choose(item) } label: { label(of: item) }
                        .accessibilityIdentifier(identifier(of: item))
                }
            } label: {
                Label("More", systemImage: HomeMenuSymbols.menu)
            }
            .accessibilityIdentifier("home-menu")
        }
    }

    @ViewBuilder
    private func label(of item: HomeMenuItem) -> some View {
        switch item {
        case .info: Label("Info", systemImage: HomeMenuSymbols.info)
        #if FEEDBACK_MAIL
        case .sendFeedback: Label { Text("Send feedback", tableName: "Feedback") } icon: { Image(systemName: HomeMenuSymbols.sendFeedback) }
        #endif
        #if TIP_JAR
        case .buyMeTokens: Label { Text(TipJarWords.name) } icon: { Image(systemName: TipJarSymbols.menuItem) }
        #endif
        }
    }

    private func identifier(of item: HomeMenuItem) -> String {
        switch item {
        case .info: "home-menu-info"
        #if FEEDBACK_MAIL
        case .sendFeedback: "home-menu-send-feedback"
        #endif
        #if TIP_JAR
        case .buyMeTokens: "home-menu-buy-me-tokens"
        #endif
        }
    }
}

#endif
