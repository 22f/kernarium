// SPDX-License-Identifier: GPL-2.0-or-later

import UIKit

/// The Simulator preview's way to show the end of a long sheet: it scrolls the
/// largest scroll view of the front presentation to its end.
@MainActor
enum PreviewScroll {
    static func toBottomOfFrontSheet() {
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        guard let window = scenes.flatMap(\.windows).first(where: \.isKeyWindow) ?? scenes.first?.windows.first else {
            return
        }
        var front = window.rootViewController
        while let presented = front?.presentedViewController { front = presented }
        guard let root = front?.view, let scroll = largestScrollView(in: root) else { return }
        let end = scroll.contentSize.height - scroll.bounds.height + scroll.adjustedContentInset.bottom
        scroll.setContentOffset(CGPoint(x: 0, y: max(-scroll.adjustedContentInset.top, end)), animated: false)
    }

    private static func largestScrollView(in view: UIView) -> UIScrollView? {
        var best: UIScrollView?
        var area = 0.0
        var pending = [view]
        while let next = pending.popLast() {
            if let scroll = next as? UIScrollView, scroll.bounds.width * scroll.bounds.height > area {
                best = scroll
                area = scroll.bounds.width * scroll.bounds.height
            }
            pending += next.subviews
        }
        return best
    }
}
