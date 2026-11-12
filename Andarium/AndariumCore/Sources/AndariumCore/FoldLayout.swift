// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

// Where Andarium puts its pieces on a display that folds (the iPhone Duo).
// Every decision here is a pure function of what the system reports: the
// window, its safe area, the hinge and the regions the system reserves. The
// application only reads those and draws the answers; a display that does not
// fold reports no hinge, has no pose here, and keeps the layouts it always had.

/// What the system says about the display a scene is on. All rectangles are in
/// the window's own coordinates, in points.
public struct FoldReading: Equatable, Sendable {
    /// The hinge's status (`UIHinge.Status`). Nil where the system reports no
    /// hinge, which is every device that does not fold.
    public enum Hinge: Equatable, Sendable {
        case closed
        case partiallyOpen
        case fullyOpen
    }

    /// A region the system reserves: the fold's band, with the margins it
    /// keeps around it.
    public struct Region: Equatable, Sendable {
        public var frame: CGRect
        /// The device is bent at the fold, not flat: the region the system
        /// asks content to divide at is active only then.
        public var isActive: Bool

        public init(frame: CGRect, isActive: Bool) {
            self.frame = frame
            self.isActive = isActive
        }
    }

    /// The scene's bounds.
    public var window: CGRect
    /// The safe area as it is now.
    public var safeArea: CGRect
    public var hinge: Hinge?
    /// The fold's band (`UIViewReservedRegionKind.divisionRegionKind`, inactive
    /// ones included). The inner display reports one in every pose, the cover
    /// none.
    public var division: Region?
    /// The active occlusion regions: what the system keeps for the camera and
    /// the status bar's block.
    public var occlusions: [CGRect]

    public init(
        window: CGRect, safeArea: CGRect, hinge: Hinge?, division: Region? = nil, occlusions: [CGRect] = []
    ) {
        self.window = window
        self.safeArea = safeArea
        self.hinge = hinge
        self.division = division
        self.occlusions = occlusions
    }

    /// A display that does not fold.
    public static func plain(window: CGRect, safeArea: CGRect) -> FoldReading {
        FoldReading(window: window, safeArea: safeArea, hinge: nil)
    }

    public var pose: FoldPose {
        guard hinge != nil || division != nil else { return .plain }
        guard let division else { return .cover }
        let band = division.frame
        return band.height > band.width
            ? .upright(bent: division.isActive) : .turned(bent: division.isActive)
    }

    /// The safe area a session has. The session hides the status bar, and the
    /// inner display's status bar and the safe area it takes go with it; the
    /// home indicator stays. The cover keeps the strip beside its camera
    /// whatever the application asks. It is the safe area the guest's display
    /// is laid out in, whichever screen the reading was made on, so Home can
    /// state the display the next start gives.
    public var sessionSafeArea: CGRect {
        guard pose.isInner else { return safeArea }
        var area = window
        area.size.height = max(0, safeArea.maxY - window.minY)
        return area
    }
}

/// Where the scene is on a display that may fold.
public enum FoldPose: Equatable, Sendable {
    /// The system reports no hinge: an iPhone, an iPad or a Mac.
    case plain
    /// The outer display of a folding iPhone, shut.
    case cover
    /// The inner display held upright (the fold runs down the screen), bent
    /// at the fold (book) or flat.
    case upright(bent: Bool)
    /// The inner display turned (the fold runs across the screen), bent so the
    /// device stands on its lower half like a laptop, or flat.
    case turned(bent: Bool)

    public var folds: Bool { self != .plain }

    /// Whether the file browser's Import is a round button of its own in the
    /// toolbar, in the column the strip beside the cover's camera gives the
    /// toolbar's buttons, and not a labelled button in the bar at the foot.
    public var importIsInToolbar: Bool { self == .cover }

    public var isInner: Bool {
        switch self {
        case .upright, .turned: true
        case .plain, .cover: false
        }
    }

    /// Whether the interface of this display turns with the device. An iPad's
    /// does; an iPhone's, and the folding iPhone's cover, are portrait-only.
    /// The inner display turns whatever the application declares, so the
    /// application follows it as an iPad's does.
    public func interfaceRotates(idiomIsPhone: Bool) -> Bool {
        !idiomIsPhone || isInner
    }
}

/// The session on a display that folds: where the guest, the side keys and the
/// statistics go, each as a rectangle in window coordinates.
///
/// - Cover: the guest is today's phone, 382 points wide. The strip the camera
///   keeps beside it holds the side keys, always open in one column, and the
///   statistics above them. Notices hang from the guest's top.
/// - Inner, upright, bent or flat: the guest fills the safe area across both
///   halves, so the hinge never resizes Android. The keys stay on its edge,
///   closed until tapped. The statistics and the notices hang from the top of
///   the trailing half.
/// - Inner, turned, flat: the guest fills the safe area; the keys stay on its
///   edge and never rest on the fold.
/// - Inner, turned, bent (standing like a laptop): the guest takes the upper
///   half; the lower half, on the table, is a deck with the keys in one row
///   and the statistics and notices above them.
public struct SessionPlan: Equatable, Sendable {
    public enum Keys: Equatable, Sendable {
        /// On the guest's edge, closed until tapped, draggable.
        case edge
        /// A column in the cover's strip.
        case rail(CGRect)
        /// A row on the lower half.
        case deck(CGRect)
    }

    /// The guest's display.
    public var guest: CGRect
    public var keys: Keys
    /// What the statistics and the notices hang from the top of.
    public var hud: CGRect
    /// The fold's span along the edge the keys sit on, in the guest's own
    /// coordinates, where the keys never rest; nil where the fold does not
    /// cross that edge.
    public var keepOut: ClosedRange<CGFloat>?

    /// A strip narrower than this is the sensor housing's alone and holds
    /// nothing.
    public static let minimumRailWidth: CGFloat = 60
    /// The gap between the system's block at the top of the strip and the
    /// first key.
    public static let railTopGap: CGFloat = 4
    /// The distance a sheet keeps below the fold.
    public static let sheetClearance: CGFloat = 12

    /// Nil where the display does not fold: the session is then laid out as
    /// it always was.
    public init?(_ reading: FoldReading) {
        let pose = reading.pose
        guard pose.folds else { return nil }
        let safe = reading.sessionSafeArea
        keepOut = nil
        switch pose {
        case .plain:
            return nil
        case .cover:
            guest = safe
            hud = safe
            keys = .edge
            if let rail = Self.rail(reading, safe: safe) { keys = .rail(rail) }
        case .upright:
            guest = safe
            keys = .edge
            hud = Self.halves(of: safe, reading: reading)?.second ?? safe
        case .turned(let bent):
            if bent, let halves = Self.halves(of: safe, reading: reading) {
                guest = halves.first
                keys = .deck(halves.second)
                hud = halves.second
            } else {
                guest = safe
                keys = .edge
                hud = safe
                if let band = reading.division?.frame {
                    keepOut = (band.minY - safe.minY)...(band.maxY - safe.minY)
                }
            }
        }
    }

    /// The cover's strip beside the guest, from below the system's block.
    private static func rail(_ reading: FoldReading, safe: CGRect) -> CGRect? {
        let window = reading.window
        let trailing = window.maxX - safe.maxX
        let leading = safe.minX - window.minX
        let strip: CGRect
        if trailing >= minimumRailWidth {
            strip = CGRect(x: safe.maxX, y: window.minY, width: trailing, height: window.height)
        } else if leading >= minimumRailWidth {
            strip = CGRect(x: window.minX, y: window.minY, width: leading, height: window.height)
        } else {
            return nil
        }
        let top = reading.occlusions.filter { $0.intersects(strip) }.map(\.maxY).max() ?? window.minY
        let start = top + railTopGap
        return CGRect(x: strip.minX, y: start, width: strip.width, height: max(0, safe.maxY - start))
    }

    /// The halves of `area` on either side of the fold's band, with the band
    /// between them left out. Nil where the display has no band.
    static func halves(of area: CGRect, reading: FoldReading) -> (first: CGRect, second: CGRect)? {
        let pose = reading.pose
        guard let band = reading.division?.frame, pose.isInner else { return nil }
        var first = area
        var second = area
        if case .upright = pose {
            first.size.width = max(0, band.minX - area.minX)
            second.origin.x = band.maxX
            second.size.width = max(0, area.maxX - band.maxX)
        } else {
            first.size.height = max(0, band.minY - area.minY)
            second.origin.y = band.maxY
            second.size.height = max(0, area.maxY - band.maxY)
        }
        return (first, second)
    }

    /// Whether going from one plan to the other changes the guest's size, and
    /// so asks Android to draw again: between the cover and the inner display
    /// and between standing and the other turned poses, and in no other
    /// change, a bend of the hinge held upright included. A plan that is nil
    /// is a display that does not fold.
    public static func resizesGuest(from old: SessionPlan?, to new: SessionPlan?) -> Bool {
        guard let old, let new else { return old?.guest.size != new?.guest.size }
        return old.guest.size != new.guest.size
    }
}

/// A screen outside the session that sits on the inner display of a folding
/// iPhone, and so on one side of the fold or across it.
public enum FoldScreen: Equatable, Sendable {
    /// A card, an overlay or a step that has no rule of its own: it sits where
    /// the system puts its alerts.
    case card
    /// The screen before a virtual machine exists.
    case welcome
    /// The boot's steps.
    case boot
}

/// The halves of the safe area on either side of the fold, for the screens that
/// sit on one of them.
public struct FoldHalves: Equatable, Sendable {
    public var first: CGRect
    public var second: CGRect
    public var upright: Bool
    /// The device is bent at the fold, not flat.
    public var bent: Bool
    /// The safe area the halves divide.
    public var safe: CGRect
    /// The fold band between the halves.
    public var band: CGRect

    /// Nil where the display has no fold band.
    public init?(_ reading: FoldReading) {
        guard let halves = SessionPlan.halves(of: reading.safeArea, reading: reading) else { return nil }
        first = halves.first
        second = halves.second
        if case .upright = reading.pose { upright = true } else { upright = false }
        bent = reading.division?.isActive ?? false
        safe = reading.safeArea
        band = reading.division?.frame ?? .zero
    }

    /// Where Andarium's cards, the boot's steps and its overlays go: the half
    /// where the system puts its own alerts, the leading one upright and the
    /// lower one turned.
    public var card: CGRect { upright ? first : second }

    /// The safe area cut to the most of it that is centred on the fold along
    /// the axis the fold crosses, so that content centred in it is centred on
    /// the display and the fold runs through its middle. The status bar's
    /// strip leaves the safe area off the display's centre; this is not.
    public var centred: CGRect {
        var area = safe
        if upright {
            let half = max(0, min(band.midX - safe.minX, safe.maxX - band.midX))
            area.origin.x = band.midX - half
            area.size.width = 2 * half
        } else {
            let half = max(0, min(band.midY - safe.minY, safe.maxY - band.midY))
            area.origin.y = band.midY - half
            area.size.height = 2 * half
        }
        return area
    }

    /// Where `screen` goes. While the device is flat every screen is placed
    /// as it is on an iPad: the popups (the boot, a card, an overlay) and
    /// Welcome span the display, centred on the fold and across it (`centred`).
    /// While it is bent they keep to a half, so the fold never crosses them:
    /// - Welcome and a card: on the half where the system puts its own alerts.
    /// - Boot: on the upper half when the device stands turned, the half that
    ///   faces the person; on the alerts' half upright.
    public func area(for screen: FoldScreen) -> CGRect {
        guard bent else { return centred }
        switch screen {
        case .card, .welcome: return card
        case .boot: return upright ? card : first
        }
    }

    /// How far in from each edge of `space`, the space a view is given, the
    /// view is kept to the area of `screen`. A half is never padded
    /// outward. The area centred on the display is: a navigation bar, or the
    /// strip the status bar and the toolbar take beside the camera, leaves a
    /// screen less than the display, and content centred on the display
    /// reaches past that space (a negative inset) to centre on the fold.
    public func insets(for screen: FoldScreen, in space: CGRect) -> FoldInsets {
        let area = area(for: screen)
        let reaches = screen == .welcome && !bent
        func kept(_ inset: CGFloat) -> CGFloat { reaches ? inset : max(0, inset) }
        return FoldInsets(
            top: kept(area.minY - space.minY), leading: kept(area.minX - space.minX),
            bottom: kept(space.maxY - area.maxY), trailing: kept(space.maxX - area.maxX))
    }
}

/// Distances from the edges of a space, in points; negative reaches past it.
public struct FoldInsets: Equatable, Sendable {
    public var top: CGFloat
    public var leading: CGFloat
    public var bottom: CGFloat
    public var trailing: CGFloat

    public init(top: CGFloat = 0, leading: CGFloat = 0, bottom: CGFloat = 0, trailing: CGFloat = 0) {
        self.top = top
        self.leading = leading
        self.bottom = bottom
        self.trailing = trailing
    }
}

/// Home's two pages on the inner display: one each side of the fold, each
/// scrolling on its own, as lengths along the axis the fold crosses, in the
/// layout's own space.
public struct HomePages: Equatable, Sendable {
    public var upright: Bool
    /// Before the fold, the fold with its margins, after it.
    public var first: CGFloat
    public var gap: CGFloat
    public var second: CGFloat

    /// A page shorter or narrower than this is not worth a page of its own.
    public static let minimumPage: CGFloat = 120

    /// `frame` is the space Home lays out in, in window coordinates. Nil on a
    /// display with no fold band, and where a page would be too small.
    public init?(_ reading: FoldReading, frame: CGRect) {
        guard let band = reading.division?.frame, reading.pose.isInner, frame.width > 0, frame.height > 0 else {
            return nil
        }
        if case .upright = reading.pose {
            upright = true
            first = band.minX - frame.minX
            gap = band.width
            second = frame.maxX - band.maxX
        } else {
            upright = false
            first = band.minY - frame.minY
            gap = band.height
            second = frame.maxY - band.maxY
        }
        guard first > Self.minimumPage, second > Self.minimumPage else { return nil }
    }
}

/// A sheet on the turned inner display while the device stands bent, which
/// rises to a little below the fold band and stops, so the upper half stays in
/// sight. Flat, a sheet is placed as on an iPad, by the system.
public enum FoldSheet {
    /// The detent's height; nil where the system's own placement stands (the
    /// cover, the upright inner display, the turned one while it is flat and
    /// every other screen).
    public static func detentHeight(_ reading: FoldReading) -> CGFloat? {
        guard case .turned = reading.pose, let division = reading.division, division.isActive else { return nil }
        let band = division.frame
        // A detent leaves out the home indicator's inset and the sheet's own
        // top margin, so the height is measured from the bottom of the safe
        // area.
        return max(200, reading.safeArea.maxY - band.maxY - SessionPlan.sheetClearance)
    }
}

/// Where the side keys rest along their edge.
public enum KeyPlacement {
    /// `centre` (a distance along the edge) moved to the nearer side of
    /// `keepOut` when keys `half` long either side of it would reach into it;
    /// `length` is the edge's length. A span the keys cannot clear on either
    /// side leaves the centre where it was.
    public static func clear(_ centre: CGFloat, half: CGFloat, keepOut: ClosedRange<CGFloat>?, length: CGFloat) -> CGFloat {
        guard let keepOut, centre + half > keepOut.lowerBound, centre - half < keepOut.upperBound else {
            return centre
        }
        let above = keepOut.lowerBound - half
        let below = keepOut.upperBound + half
        let fitsAbove = above >= half
        let fitsBelow = below <= length - half
        if fitsAbove && (!fitsBelow || centre - above <= below - centre) { return above }
        return fitsBelow ? below : centre
    }
}
