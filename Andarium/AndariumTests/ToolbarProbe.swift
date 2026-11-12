// SPDX-License-Identifier: GPL-2.0-or-later

import UIKit

/// Reads the navigation bar's items the way the system holds them, so a test
/// sees what a person would be offered: SwiftUI's toolbar items become
/// `UIBarButtonItem`s of the navigation items in the controller tree.
@MainActor
enum ToolbarProbe {
    /// Every bar button item of every navigation item under `root`, once each.
    static func barItems(under root: UIViewController) -> [UIBarButtonItem] {
        var items: [UIBarButtonItem] = []
        var seenItems = Set<ObjectIdentifier>()
        var seenControllers = Set<ObjectIdentifier>()
        func add(_ item: UIBarButtonItem) {
            if seenItems.insert(ObjectIdentifier(item)).inserted { items.append(item) }
        }
        func visit(_ controller: UIViewController) {
            guard seenControllers.insert(ObjectIdentifier(controller)).inserted else { return }
            let navigation = controller.navigationItem
            let groups = navigation.leadingItemGroups + navigation.centerItemGroups + navigation.trailingItemGroups
            for item in (navigation.leftBarButtonItems ?? []) + (navigation.rightBarButtonItems ?? [])
                + groups.flatMap(\.barButtonItems) {
                add(item)
            }
            controller.children.forEach(visit)
            (controller as? UINavigationController)?.viewControllers.forEach(visit)
            if let presented = controller.presentedViewController { visit(presented) }
        }
        visit(root)
        return items
    }

    /// A line for a failure message: what the bar holds.
    static func describe(under root: UIViewController) -> String {
        barItems(under: root).map { item in
            "\(item.title ?? item.image?.description ?? "item") menu=\(item.menu.map { "\($0.children.map { type(of: $0) })" } ?? "none")"
        }.joined(separator: "; ")
    }
}
