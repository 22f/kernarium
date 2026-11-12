// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The production root with a run on a display that folds: the guest's
/// display where the plan puts it, Andarium's pieces off the fold, the guest
/// kept through every fold and turn, and the rotation rule of the inner
/// display. The readings are those the iPhone Duo Simulator gives in each pose
/// (`FoldLayoutTests` has the numbers), supplied through the reader's stub into
/// a window of the pose's size, so every pose is tested on every Simulator;
/// `TheRealDisplay` reads the Simulator's own display in the pose the
/// harness put it in (`TEST_RUNNER_DUO_EXPECT_POSE`).
@MainActor
@Suite(.serialized)
struct FoldSessionHostingTests {
    static func rect(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat) -> CGRect {
        CGRect(x: x, y: y, width: w, height: h)
    }

    /// A pose of the Spread design, with the session's safe area (the status
    /// bar hidden).
    enum Pose: CaseIterable, CustomStringConvertible, Sendable {
        case cover, uprightBook, uprightFlat, turnedBook, turnedFlat

        var description: String {
            switch self {
            case .cover: "cover"
            case .uprightBook: "upright, book"
            case .uprightFlat: "upright, flat"
            case .turnedBook: "turned, book (standing)"
            case .turnedFlat: "turned, flat"
            }
        }

        var reading: FoldReading {
            switch self {
            case .cover:
                return FoldReading(
                    window: rect(0, 0, 466, 678), safeArea: rect(0, 0, 382, 644), hinge: .closed,
                    occlusions: [rect(399.7, 29.3, 37, 37), rect(382, 0, 84, 82)])
            case .uprightBook, .uprightFlat:
                let bent = self == .uprightBook
                return FoldReading(
                    window: rect(0, 0, 951, 669), safeArea: rect(0, 0, 951, 635),
                    hinge: bent ? .partiallyOpen : .fullyOpen,
                    division: .init(frame: rect(455.5, 0, 40, 669), isActive: bent))
            case .turnedBook, .turnedFlat:
                let bent = self == .turnedBook
                return FoldReading(
                    window: rect(0, 0, 669, 951), safeArea: rect(0, 0, 669, 917),
                    hinge: bent ? .partiallyOpen : .fullyOpen,
                    division: .init(frame: rect(0, 455.5, 669, 40), isActive: bent))
            }
        }

        /// The guest's rectangle in the window, from the design's table.
        var guest: CGRect {
            switch self {
            case .cover: rect(0, 0, 382, 644)
            case .uprightBook, .uprightFlat: rect(0, 0, 951, 635)
            case .turnedBook: rect(0, 0, 669, 455.5)
            case .turnedFlat: rect(0, 0, 669, 917)
            }
        }

        var inner: Bool { self != .cover }

        var foldPose: FoldPose {
            switch self {
            case .cover: .cover
            case .uprightBook: .upright(bent: true)
            case .uprightFlat: .upright(bent: false)
            case .turnedBook: .turned(bent: true)
            case .turnedFlat: .turned(bent: false)
            }
        }
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(30))
        }
        return condition()
    }

    private func descendants(of view: UIView) -> [UIView] {
        view.subviews + view.subviews.flatMap { descendants(of: $0) }
    }

    /// The scene root with a run on screen, in a view the size of the pose that
    /// sits in the application's window. A window cannot take another size, so
    /// the root is a child of the window's controller in a view of its own,
    /// whose hit testing is asked.
    @MainActor
    private final class Rig {
        let model: AppModel
        let lease: HostWindow.Lease
        let container = UIViewController()
        let holder: UIView
        let controller: AndariumHostingController<AnyView>
        let driver = StubDriver(plan: nil)
        var pose: Pose
        private(set) var guest: GuestMetalView!

        init(_ pose: Pose, test: FoldSessionHostingTests) async throws {
            self.pose = pose
            let entry = AppEntry.fromArguments()
            let model = entry.makeModel()
            model.launchApplied = true
            self.model = model
            lease = try await HostWindow.take()
            holder = UIView(frame: pose.reading.window)
            controller = AndariumHostingController(rootView: AnyView(RootView(entry: entry).environment(model)), model: model)
            model.driver = driver
            controller.foldReader.stub = { [unowned self] in self.pose.reading }
            container.view.backgroundColor = .black
            lease.window.rootViewController = container
            container.view.addSubview(holder)
            container.addChild(controller)
            holder.addSubview(controller.view)
            controller.view.frame = holder.bounds
            controller.didMove(toParent: container)
            model.phase = .running(.normal)
            controller.view.layoutIfNeeded()
            var display: GuestMetalView?
            let found = await test.eventually {
                self.controller.view.layoutIfNeeded()
                display = test.descendants(of: self.controller.view).compactMap { $0 as? GuestMetalView }.first
                return display != nil && display!.bounds.width > 0
            }
            guard found, let display else {
                lease.give()
                throw Failure.noDisplay
            }
            guest = display
        }

        enum Failure: Error { case noDisplay }

        func end() {
            controller.willMove(toParent: nil)
            controller.view.removeFromSuperview()
            controller.removeFromParent()
            holder.removeFromSuperview()
            lease.give()
        }

        /// The scene moves to `next`: the view takes its size and the root
        /// reads the pose again, as a fold or a turn does.
        func move(to next: Pose, test: FoldSessionHostingTests) async {
            pose = next
            holder.frame = next.reading.window
            controller.view.frame = holder.bounds
            controller.view.setNeedsLayout()
            holder.layoutIfNeeded()
            _ = await test.eventually { self.model.fold == next.reading }
            controller.view.layoutIfNeeded()
            try? await Task.sleep(for: .milliseconds(250))
            controller.view.layoutIfNeeded()
        }

        var guestFrame: CGRect { guest.convert(guest.bounds, to: holder) }

        func hit(_ point: CGPoint) -> UIView? { holder.hitTest(point, with: nil) }

        /// What the screen's accessibility tree names, with each element's
        /// frame in the view's coordinates: Andarium's keys, statistics and
        /// notices are the elements.
        func labelled() -> [(label: String, frame: CGRect)] {
            var found: [(String, CGRect)] = []
            func walk(_ node: NSObject, depth: Int) {
                guard depth < 8 else { return }
                let count = node.accessibilityElementCount()
                if count != NSNotFound, count > 0 {
                    for index in 0..<count {
                        if let child = node.accessibilityElement(at: index) as? NSObject { walk(child, depth: depth + 1) }
                    }
                }
                if let label = node.accessibilityLabel, !label.isEmpty, node !== controller.view,
                    let screen = holder.window?.screen {
                    found.append((label, holder.convert(node.accessibilityFrame, from: screen.coordinateSpace)))
                }
            }
            walk(controller.view, depth: 0)
            return found
        }

        func reachesGuest(_ point: CGPoint) -> Bool {
            guard let view = hit(point) else { return false }
            return view === guest || view.isDescendant(of: guest)
        }
    }

    private func near(_ a: CGRect, _ b: CGRect, tolerance: CGFloat = 0.6) -> Bool {
        abs(a.minX - b.minX) < tolerance && abs(a.minY - b.minY) < tolerance
            && abs(a.width - b.width) < tolerance && abs(a.height - b.height) < tolerance
    }

    // MARK: Where the guest is

    @Test(arguments: Pose.allCases)
    func theGuestIsWhereThePlanPutsItInEveryPose(_ pose: Pose) async throws {
        let rig = try await Rig(pose, test: self)
        defer { rig.end() }
        #expect(await eventually { near(rig.guestFrame, pose.guest) }, "\(pose): the guest is at \(rig.guestFrame)")
        #expect(rig.model.sessionPlan?.guest == pose.guest, "\(pose)")
        let frame = rig.guestFrame
        // Inside the guest's corners a touch reaches it; beside it, it does not.
        for point in [
            CGPoint(x: frame.minX + 2, y: frame.minY + 2), CGPoint(x: frame.minX + 2, y: frame.maxY - 2),
            CGPoint(x: frame.midX - 100, y: frame.midY),
        ] {
            #expect(rig.reachesGuest(point), "\(pose): \(point)")
        }
        let window = pose.reading.window
        for point in [CGPoint(x: window.maxX - 2, y: frame.midY), CGPoint(x: frame.midX, y: window.maxY - 2)]
        where !frame.contains(point) {
            #expect(!rig.reachesGuest(point), "\(pose): \(point) is beside the guest")
        }
        // Whatever is beside the guest stays black, in the light appearance
        // too: the window and the root carry it.
        #expect(rig.controller.view.backgroundColor == .black, "\(pose)")
        #expect(rig.lease.window.backgroundColor == .black, "\(pose)")
    }

    // MARK: Nothing of Andarium's on the fold

    @Test(arguments: [Pose.uprightBook, .uprightFlat, .turnedFlat])
    func nothingOfAndariumsIsOnTheFoldWhenTheGuestSpansIt(_ pose: Pose) async throws {
        let rig = try await Rig(pose, test: self)
        defer { rig.end() }
        // The pieces that can be on screen: the statistics and the keys.
        rig.model.session.showStats = true
        rig.model.session.sideKeysOpen = false
        try await Task.sleep(for: .milliseconds(600))
        rig.controller.view.layoutIfNeeded()
        let band = try #require(pose.reading.division).frame
        for open in [false, true] {
            rig.model.session.sideKeysOpen = open
            try await Task.sleep(for: .milliseconds(700))
            rig.controller.view.layoutIfNeeded()
            // Along the fold's band, edge to edge and a little in from each
            // margin, every touch is the guest's: no key, no capsule and no
            // notice is there.
            var points: [CGPoint] = []
            if band.height > band.width {
                for y in stride(from: CGFloat(10), to: pose.guest.height - 10, by: 25) {
                    points += [CGPoint(x: band.midX, y: y), CGPoint(x: band.minX + 2, y: y), CGPoint(x: band.maxX - 2, y: y)]
                }
            } else {
                for x in stride(from: CGFloat(10), to: pose.guest.width - 10, by: 25) {
                    points += [CGPoint(x: x, y: band.midY), CGPoint(x: x, y: band.minY + 2), CGPoint(x: x, y: band.maxY - 2)]
                }
            }
            for point in points {
                #expect(rig.reachesGuest(point), "\(pose), keys open \(open): \(point) is on the fold")
            }
            // And no element of the screen's accessibility tree is there.
            for element in rig.labelled() {
                #expect(!element.frame.intersects(band), "\(pose), keys open \(open): \(element.label) at \(element.frame)")
            }
        }
    }

    @Test func standingTheLowerHalfHoldsTheKeysAndTheFoldHoldsNothing() async throws {
        let rig = try await Rig(.turnedBook, test: self)
        defer { rig.end() }
        rig.model.session.showStats = true
        try await Task.sleep(for: .milliseconds(700))
        rig.controller.view.layoutIfNeeded()
        let window = Pose.turnedBook.reading.window
        let band = try #require(Pose.turnedBook.reading.division).frame
        // The guest is the upper half alone: nothing from the fold down
        // reaches Android, and the upper half does.
        for y in stride(from: band.minY + 2, to: window.maxY - 2, by: 40) {
            #expect(!rig.reachesGuest(CGPoint(x: 330, y: y)), "y \(y) is below the guest")
        }
        #expect(rig.reachesGuest(CGPoint(x: 330, y: 400)))
        // Andarium's keys are in the lower half, none on the fold: the deck.
        let deck = CGRect(x: 0, y: band.maxY, width: window.width, height: window.height - band.maxY)
        let names = ["Back", "Home", "Settings", "Volume down", "Volume up", "Power"]
        let elements = rig.labelled()
        for name in names {
            let frames = elements.filter { $0.label == name }.map(\.frame)
            #expect(!frames.isEmpty, "no \(name) key on the deck: \(elements.map(\.label))")
            for frame in frames {
                #expect(deck.contains(frame.insetBy(dx: 1, dy: 1)), "\(name) is at \(frame)")
            }
        }
        // Nothing of Andarium's is in the band between the halves, and the
        // guest does not reach into it.
        for element in elements {
            #expect(!element.frame.intersects(band), "\(element.label) is on the fold at \(element.frame)")
        }
        for x in stride(from: CGFloat(10), to: window.width - 10, by: 25) {
            for y in [band.minY + 2, band.midY, band.maxY - 2] {
                #expect(!rig.reachesGuest(CGPoint(x: x, y: y)), "\(x), \(y)")
            }
        }
    }

    // MARK: Folding, turning and bending

    /// Every change the walk makes, with the size the guest's display takes
    /// by it: only a change of the guest's shape gives a new one.
    @Test func theGuestIsNeverReplacedAndIsResizedOnlyWhereItsShapeChanges() async throws {
        let rig = try await Rig(.cover, test: self)
        defer { rig.end() }
        let display = rig.guest!
        #expect(await eventually { near(rig.guestFrame, Pose.cover.guest) })
        var sizes: [CGSize] = [display.bounds.size]
        let observation = display.layer.observe(\.bounds, options: [.new]) { layer, _ in
            MainActor.assumeIsolated {
                if layer.bounds.size != sizes.last { sizes.append(layer.bounds.size) }
            }
        }
        defer { observation.invalidate() }
        let walk: [(Pose, resizes: Bool)] = [
            (.uprightBook, true),   // the unfold
            (.uprightFlat, false),  // the hinge moves, held upright: nothing
            (.uprightBook, false),
            (.turnedFlat, true),    // the turn
            (.turnedBook, true),    // stand the device like a laptop
            (.turnedFlat, true),
            (.uprightFlat, true),
            (.cover, true),         // the fold
            (.cover, false),
        ]
        var previous = Pose.cover
        for (next, resizes) in walk {
            let before = sizes.count
            await rig.move(to: next, test: self)
            #expect(await eventually { near(rig.guestFrame, next.guest) }, "\(previous) to \(next): at \(rig.guestFrame)")
            #expect(sizes.count - before == (resizes ? 1 : 0), "\(previous) to \(next): sizes \(sizes.suffix(sizes.count - before))")
            #expect(rig.guest === display, "\(previous) to \(next): the guest's display was replaced")
            #expect(rig.model.phase == .running(.normal), "\(previous) to \(next)")
            #expect(SessionPlan.resizesGuest(from: SessionPlan(previous.reading), to: SessionPlan(next.reading)) == resizes)
            previous = next
        }
        // The sizes the guest took, one for each change of its shape, to the
        // pixel grid (the display lays out on the screen's pixels).
        let expected = [
            CGSize(width: 382, height: 644), CGSize(width: 951, height: 635), CGSize(width: 669, height: 917),
            CGSize(width: 669, height: 455.5), CGSize(width: 669, height: 917), CGSize(width: 951, height: 635),
            CGSize(width: 382, height: 644),
        ]
        #expect(sizes.count == expected.count)
        for (got, want) in zip(sizes, expected) {
            #expect(abs(got.width - want.width) < 0.34 && abs(got.height - want.height) < 0.34, "\(got) is not \(want)")
        }
    }

    // MARK: Rotation

    @Test(arguments: Pose.allCases)
    func theInnerDisplayTurnsTheGuestAsAnIPadDoesAndTheCoverAsAnIPhoneDoes(_ pose: Pose) async throws {
        let rig = try await Rig(pose, test: self)
        defer { rig.end() }
        let phone = UIDevice.current.userInterfaceIdiom == .phone
        // An iPad's interface turns on every display; a phone's only on the
        // inner display of a folding one.
        let rotates = !phone || pose.inner
        #expect(await eventually { rig.model.hostDrivesGuestRotation == rotates }, "\(pose)")
        #expect(rig.controller.supportedInterfaceOrientations == (rotates ? .all : .portrait), "\(pose)")
        #expect(!rig.controller.prefersInterfaceOrientationLocked, "\(pose)")
        // A move to the other display changes the policy once the scene
        // root has read it, the session going on.
        let other: Pose = pose.inner ? .cover : .uprightBook
        await rig.move(to: other, test: self)
        let otherRotates = !phone || other.inner
        #expect(await eventually { rig.model.hostDrivesGuestRotation == otherRotates }, "\(pose) to \(other)")
        #expect(rig.controller.supportedInterfaceOrientations == (otherRotates ? .all : .portrait), "\(pose) to \(other)")
        #expect(rig.model.phase == .running(.normal))
    }

    @Test func aDisplayThatDoesNotFoldHasNoPoseAndKeepsItsLayout() async throws {
        let entry = AppEntry.fromArguments()
        let model = entry.makeModel()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = AndariumHostingController(rootView: Color.black, model: model)
        lease.window.rootViewController = controller
        model.phase = .running(.normal)
        try await Task.sleep(for: .milliseconds(500))
        controller.view.layoutIfNeeded()
        // Only a system that reports a hinge gives a reading. On an iPhone, an
        // iPad or a Mac there is none, and the session is laid out as ever.
        if !controller.foldReader.reportsHinge {
            #expect(model.fold == nil)
            #expect(model.sessionPlan == nil)
            #expect(model.foldPose == .plain)
        }
    }
}

/// The Simulator's own display, in the pose the harness put it in
/// (`TEST_RUNNER_DUO_EXPECT_POSE`: `cover`, `upright-book`, `upright-flat`,
/// `turned-book`, `turned-flat`). Without the variable the suite checks
/// nothing, so the ordinary runs on other devices pass.
@MainActor
@Suite(.serialized)
struct TheRealDisplay {
    private var expected: FoldSessionHostingTests.Pose? {
        switch ProcessInfo.processInfo.environment["DUO_EXPECT_POSE"] {
        case "cover": .cover
        case "upright-book": .uprightBook
        case "upright-flat": .uprightFlat
        case "turned-book": .turnedBook
        case "turned-flat": .turnedFlat
        default: nil
        }
    }

    private func descendants(of view: UIView) -> [UIView] {
        view.subviews + view.subviews.flatMap { descendants(of: $0) }
    }

    @Test func theSceneReadsAsTheExpectedPoseAndTheGuestFollowsThePlan() async throws {
        guard let pose = expected else { return }
        let entry = AppEntry.fromArguments()
        let model = entry.makeModel()
        model.launchApplied = true
        model.driver = StubDriver(plan: nil)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = AndariumHostingController(rootView: RootView(entry: entry).environment(model), model: model)
        lease.window.rootViewController = controller
        model.phase = .running(.normal)
        let deadline = ContinuousClock.now + .seconds(20)
        while model.sessionPlan == nil, ContinuousClock.now < deadline { try await Task.sleep(for: .milliseconds(50)) }
        try await Task.sleep(for: .milliseconds(600))
        controller.view.layoutIfNeeded()
        // The pose the system reports, and the plan the design gives it.
        #expect(model.foldPose == pose.foldPose, "the scene reads as \(model.foldPose)")
        let plan = try #require(model.sessionPlan)
        #expect(plan.guest == pose.guest, "the plan's guest is \(plan.guest)")
        // The guest's display is there.
        var display: GuestMetalView?
        let end = ContinuousClock.now + .seconds(10)
        while ContinuousClock.now < end {
            controller.view.layoutIfNeeded()
            display = descendants(of: controller.view).compactMap { $0 as? GuestMetalView }.first
            if let display, display.bounds.width > 0 { break }
            try await Task.sleep(for: .milliseconds(50))
        }
        let shown = try #require(display)
        let frame = shown.convert(shown.bounds, to: lease.window)
        #expect(abs(frame.minX - pose.guest.minX) < 0.6 && abs(frame.minY - pose.guest.minY) < 0.6
                && abs(frame.width - pose.guest.width) < 0.6 && abs(frame.height - pose.guest.height) < 0.6,
                "the display is at \(frame)")
        // The rotation rule.
        let phone = UIDevice.current.userInterfaceIdiom == .phone
        let rotates = !phone || pose.inner
        #expect(model.hostDrivesGuestRotation == rotates)
        #expect(controller.supportedInterfaceOrientations == (rotates ? .all : .portrait))
        model.phase = .home
    }
}
