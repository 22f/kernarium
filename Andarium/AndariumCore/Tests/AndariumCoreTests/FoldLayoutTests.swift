// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

// The layout on the folding iPhone's displays (`FoldLayout.swift`), one test per
// pose and rotation of the Spread design. The readings are what the iPhone Duo
// Simulator reports in each pose (iOS 27.2): the cover 466 x 678 with the
// strip its camera keeps, the inner display 951 x 669 held upright (the fold
// down its middle) and 669 x 951 turned (the fold across it), the system's
// fold band 40 points wide with its margins, active while the device is bent
// and inactive flat.

/// The display and its pose, as the system reports them.
private enum Pose {
    case cover
    case coverLeadingStrip
    case upright(bent: Bool)
    case turned(bent: Bool)
}

private func rect(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat) -> CGRect {
    CGRect(x: x, y: y, width: w, height: h)
}

/// A reading in a pose, with the status bar shown (Home) or hidden (the
/// session). The cover keeps its strip either way.
private func reading(_ pose: Pose, statusBarHidden: Bool = true) -> FoldReading {
    switch pose {
    case .cover:
        // The camera, and the system's block at the strip's top once the status
        // bar is hidden; shown, the block is taller.
        return FoldReading(
            window: rect(0, 0, 466, 678), safeArea: rect(0, 0, 382, 644), hinge: .closed,
            occlusions: [rect(399.7, 29.3, 37, 37), statusBarHidden ? rect(382, 0, 84, 82) : rect(382, 0, 84, 174)])
    case .coverLeadingStrip:
        // The cover turned to landscape, where the camera is on the leading side.
        return FoldReading(
            window: rect(0, 0, 678, 466), safeArea: rect(84, 0, 594, 432), hinge: .closed,
            occlusions: [rect(29.3, 29.3, 37, 37), rect(0, 0, 84, 82)])
    case .upright(let bent):
        return FoldReading(
            window: rect(0, 0, 951, 669), safeArea: statusBarHidden ? rect(0, 0, 951, 635) : rect(0, 0, 867, 635),
            hinge: bent ? .partiallyOpen : .fullyOpen,
            division: .init(frame: rect(455.5, 0, 40, 669), isActive: bent),
            occlusions: statusBarHidden ? [] : [rect(867, 0, 84, 124)])
    case .turned(let bent):
        return FoldReading(
            window: rect(0, 0, 669, 951), safeArea: statusBarHidden ? rect(0, 0, 669, 917) : rect(0, 82, 669, 835),
            hinge: bent ? .partiallyOpen : .fullyOpen,
            division: .init(frame: rect(0, 455.5, 669, 40), isActive: bent),
            occlusions: statusBarHidden ? [] : [rect(525, 0, 144, 82)])
    }
}

private let plainPhone = FoldReading.plain(window: rect(0, 0, 402, 874), safeArea: rect(0, 62, 402, 778))
private let plainPad = FoldReading.plain(window: rect(0, 0, 834, 1194), safeArea: rect(0, 24, 834, 1146))

struct FoldPoseTests {
    @Test func aDisplayThatReportsNoHingeIsPlain() {
        #expect(plainPhone.pose == .plain)
        #expect(plainPad.pose == .plain)
        #expect(!plainPhone.pose.folds)
        #expect(SessionPlan(plainPhone) == nil)
        #expect(SessionPlan(plainPad) == nil)
        #expect(FoldHalves(plainPhone) == nil)
        #expect(HomePages(plainPhone, frame: plainPhone.safeArea) == nil)
        #expect(FoldSheet.detentHeight(plainPhone) == nil)
    }

    @Test func theCoverIsAHingeWithNoFoldBand() {
        #expect(reading(.cover).pose == .cover)
        #expect(reading(.coverLeadingStrip).pose == .cover)
        #expect(!reading(.cover).pose.isInner)
    }

    @Test func theInnerDisplayIsToldByItsFoldBandWhateverTheHingeStatusSays() {
        #expect(reading(.upright(bent: true)).pose == .upright(bent: true))
        #expect(reading(.upright(bent: false)).pose == .upright(bent: false))
        #expect(reading(.turned(bent: true)).pose == .turned(bent: true))
        #expect(reading(.turned(bent: false)).pose == .turned(bent: false))
        var odd = reading(.upright(bent: true))
        odd.hinge = .closed
        #expect(odd.pose == .upright(bent: true))
        for pose in [FoldPose.upright(bent: true), .upright(bent: false), .turned(bent: true), .turned(bent: false)] {
            #expect(pose.isInner && pose.folds)
        }
    }

    @Test func aBandWiderThanTallRunsAcrossAndOneTallerThanWideDown() {
        #expect(reading(.upright(bent: true)).division!.frame.height > reading(.upright(bent: true)).division!.frame.width)
        #expect(reading(.turned(bent: true)).division!.frame.width > reading(.turned(bent: true)).division!.frame.height)
    }

    @Test func theSessionsSafeAreaKeepsTheHomeIndicatorAndLosesTheStatusBar() {
        // Home and the session read different safe areas in the same pose, and
        // the next start's display must be the session's.
        for pose in [Pose.upright(bent: true), .upright(bent: false), .turned(bent: true), .turned(bent: false)] {
            let home = reading(pose, statusBarHidden: false)
            let session = reading(pose, statusBarHidden: true)
            #expect(home.safeArea != session.safeArea)
            #expect(home.sessionSafeArea == session.safeArea)
            #expect(session.sessionSafeArea == session.safeArea)
        }
        #expect(reading(.upright(bent: true)).sessionSafeArea == rect(0, 0, 951, 635))
        #expect(reading(.turned(bent: true)).sessionSafeArea == rect(0, 0, 669, 917))
    }

    @Test func theCoversSessionSafeAreaKeepsTheCamerasStrip() {
        #expect(reading(.cover).sessionSafeArea == rect(0, 0, 382, 644))
        #expect(reading(.cover, statusBarHidden: false).sessionSafeArea == rect(0, 0, 382, 644))
    }
}

struct SessionPlanTests {
    private func plan(_ pose: Pose, statusBarHidden: Bool = true) throws -> SessionPlan {
        try #require(SessionPlan(reading(pose, statusBarHidden: statusBarHidden)))
    }

    // MARK: Cover

    @Test func theCoverKeepsTodaysPhoneAndPutsTheKeysInTheStrip() throws {
        let plan = try plan(.cover)
        #expect(plan.guest == rect(0, 0, 382, 644))
        // The strip below the system's block at its top, to the home
        // indicator's edge: 84 x 558 points, as the design draws it.
        #expect(plan.keys == .rail(rect(382, 86, 84, 558)))
        // Notices hang from the guest's top.
        #expect(plan.hud == plan.guest)
        #expect(plan.keepOut == nil)
    }

    @Test func theRailStartsBelowWhateverTheSystemKeepsAtTheStripsTop() throws {
        // With the status bar shown the system's block is 174 points tall.
        let plan = try plan(.cover, statusBarHidden: false)
        #expect(plan.keys == .rail(rect(382, 178, 84, 466)))
        #expect(plan.guest == rect(0, 0, 382, 644))
    }

    @Test func aStripOnTheLeadingSideHoldsTheRailThere() throws {
        let plan = try plan(.coverLeadingStrip)
        #expect(plan.guest == rect(84, 0, 594, 432))
        #expect(plan.keys == .rail(rect(0, 86, 84, 346)))
    }

    @Test func aCoverWithNoStripHasNoRail() throws {
        var bare = reading(.cover)
        bare.safeArea = bare.window
        bare.occlusions = []
        let plan = try #require(SessionPlan(bare))
        #expect(plan.guest == bare.window)
        #expect(plan.keys == .edge)
    }

    // MARK: Inner, upright

    @Test(arguments: [true, false])
    func heldUprightTheGuestSpansBothHalvesBentOrFlat(bent: Bool) throws {
        let plan = try plan(.upright(bent: bent))
        #expect(plan.guest == rect(0, 0, 951, 635))
        #expect(plan.keys == .edge)
        // The statistics and the notices hang from the trailing half, beside
        // the keys and clear of the fold's band and its margins.
        #expect(plan.hud == rect(495.5, 0, 455.5, 635))
        #expect(plan.keepOut == nil)
    }

    @Test func bendingTheHingeHeldUprightChangesNothing() throws {
        #expect(try plan(.upright(bent: true)) == plan(.upright(bent: false)))
        #expect(!SessionPlan.resizesGuest(from: try plan(.upright(bent: true)), to: try plan(.upright(bent: false))))
    }

    @Test func theUprightPlanIsTheSameWhenHomeReadItWithTheStatusBarShown() throws {
        #expect(try plan(.upright(bent: true), statusBarHidden: false) == plan(.upright(bent: true)))
    }

    // MARK: Inner, turned

    @Test func turnedAndFlatTheGuestFillsTheSafeAreaAndTheKeysAvoidTheFold() throws {
        let plan = try plan(.turned(bent: false))
        #expect(plan.guest == rect(0, 0, 669, 917))
        #expect(plan.keys == .edge)
        #expect(plan.hud == plan.guest)
        // The fold's band with its margins, from the guest's own top.
        #expect(plan.keepOut == 455.5...495.5)
    }

    @Test func standingTheGuestIsTheUpperHalfAndTheLowerHalfIsTheDeck() throws {
        let plan = try plan(.turned(bent: true))
        #expect(plan.guest == rect(0, 0, 669, 455.5))
        #expect(plan.keys == .deck(rect(0, 495.5, 669, 421.5)))
        #expect(plan.hud == rect(0, 495.5, 669, 421.5))
        #expect(plan.keepOut == nil)
    }

    // MARK: What stays clear of the fold

    @Test func nothingOfAndariumsOnTheInnerDisplayTouchesTheFoldBandInAnyPose() throws {
        for pose in [Pose.upright(bent: true), .upright(bent: false), .turned(bent: true), .turned(bent: false)] {
            let reading = reading(pose)
            let band = try #require(reading.division).frame
            let plan = try #require(SessionPlan(reading))
            // The statistics and the notices.
            if case .upright = reading.pose {
                #expect(!plan.hud.intersects(band), "\(pose)")
            } else if case .turned(true) = reading.pose {
                #expect(!plan.hud.intersects(band), "\(pose)")
            }
            // The deck, and the guest above it when standing.
            if case .deck(let deck) = plan.keys {
                #expect(!deck.intersects(band), "\(pose)")
                #expect(!plan.guest.intersects(band), "\(pose)")
            }
            // The keys: never over the band along their edge.
            if let span = plan.keepOut {
                #expect(span == (band.minY - plan.guest.minY)...(band.maxY - plan.guest.minY), "\(pose)")
            }
        }
    }

    @Test func theKeysSettleOnTheNearerSideOfTheBandNeverOnIt() {
        let span: ClosedRange<CGFloat> = 455.5...495.5
        // The closed keys are 124 tall, the open group 244 or so; the guest is
        // 917 tall. Every centre the drag can reach (10% to 90%) ends clear.
        for half: CGFloat in [62, 122] {
            var y: CGFloat = 917 * 0.1
            while y <= 917 * 0.9 {
                let settled = KeyPlacement.clear(y, half: half, keepOut: span, length: 917)
                #expect(settled + half <= span.lowerBound || settled - half >= span.upperBound,
                        "centre \(y), half \(half) settled at \(settled)")
                y += 7
            }
        }
        // Below the band when that is nearer, above it when that is.
        #expect(KeyPlacement.clear(480, half: 62, keepOut: span, length: 917) == 557.5)
        #expect(KeyPlacement.clear(470, half: 62, keepOut: span, length: 917) == 393.5)
        // Clear already: left where it is.
        #expect(KeyPlacement.clear(250, half: 62, keepOut: span, length: 917) == 250)
        #expect(KeyPlacement.clear(480, half: 62, keepOut: nil, length: 917) == 480)
        // A band the keys cannot clear on either side leaves them where they were.
        #expect(KeyPlacement.clear(100, half: 62, keepOut: 0...200, length: 200) == 100)
    }

    // MARK: Which changes resize Android

    @Test func theGuestIsResizedWhereItsShapeChangesAndNowhereElse() throws {
        let cover = try plan(.cover)
        let upBook = try plan(.upright(bent: true))
        let upFlat = try plan(.upright(bent: false))
        let turnedFlat = try plan(.turned(bent: false))
        let standing = try plan(.turned(bent: true))
        // The fold and the unfold.
        #expect(SessionPlan.resizesGuest(from: cover, to: upBook))
        #expect(SessionPlan.resizesGuest(from: upFlat, to: cover))
        #expect(SessionPlan.resizesGuest(from: cover, to: turnedFlat))
        #expect(SessionPlan.resizesGuest(from: standing, to: cover))
        // Held upright the hinge moves nothing.
        #expect(!SessionPlan.resizesGuest(from: upBook, to: upFlat))
        #expect(!SessionPlan.resizesGuest(from: upFlat, to: upBook))
        // The turn.
        #expect(SessionPlan.resizesGuest(from: upBook, to: standing))
        #expect(SessionPlan.resizesGuest(from: upFlat, to: turnedFlat))
        // Standing and flat, turned: the one hinge move that resizes Android.
        #expect(SessionPlan.resizesGuest(from: turnedFlat, to: standing))
        #expect(SessionPlan.resizesGuest(from: standing, to: turnedFlat))
        // The same shape again.
        #expect(!SessionPlan.resizesGuest(from: cover, to: cover))
        #expect(!SessionPlan.resizesGuest(from: standing, to: standing))
        // A display that does not fold is not part of a transition.
        #expect(!SessionPlan.resizesGuest(from: nil, to: nil))
        #expect(SessionPlan.resizesGuest(from: nil, to: cover))
    }

    @Test func theGuestsSizeInEachPoseIsWhatAndroidIsToldAt160DotsAPoint() throws {
        // Android sees sw382, sw951 (635 tall) and sw669 (917 or 455.5 tall).
        #expect(try plan(.cover).guest.size == CGSize(width: 382, height: 644))
        #expect(try plan(.upright(bent: true)).guest.size == CGSize(width: 951, height: 635))
        #expect(try plan(.turned(bent: false)).guest.size == CGSize(width: 669, height: 917))
        #expect(try plan(.turned(bent: true)).guest.size == CGSize(width: 669, height: 455.5))
    }
}

struct FoldScreensTests {
    @Test func homeHasOnePageEachSideOfTheFoldUpright() throws {
        // Home lays out in the window less the status bar's safe area.
        let reading = reading(.upright(bent: true), statusBarHidden: false)
        let pages = try #require(HomePages(reading, frame: reading.safeArea))
        #expect(pages.upright)
        #expect(pages.first == 455.5)
        #expect(pages.gap == 40)
        #expect(pages.second == 371.5)
        #expect(pages == HomePages(reading, frame: reading.safeArea))
    }

    @Test func homeHasAnUpperAndALowerPageTurned() throws {
        for bent in [true, false] {
            let reading = reading(.turned(bent: bent), statusBarHidden: false)
            let pages = try #require(HomePages(reading, frame: reading.safeArea))
            #expect(!pages.upright)
            #expect(pages.first == 373.5)
            #expect(pages.gap == 40)
            #expect(pages.second == 421.5)
        }
    }

    @Test func homeKeepsItsColumnOnTheCoverAndOnADisplayThatDoesNotFold() {
        let cover = reading(.cover, statusBarHidden: false)
        #expect(HomePages(cover, frame: cover.safeArea) == nil)
        #expect(HomePages(plainPad, frame: plainPad.safeArea) == nil)
    }

    @Test func aPageTooSmallIsNoPage() {
        let reading = reading(.upright(bent: true), statusBarHidden: false)
        #expect(HomePages(reading, frame: rect(0, 0, 560, 635)) == nil)
    }

    @Test func overlaysSitOnTheHalfWhereTheSystemPutsItsAlerts() throws {
        // Leading when upright, lower when turned, bent or flat alike.
        let upright = try #require(FoldHalves(reading(.upright(bent: true), statusBarHidden: false)))
        #expect(upright.card == rect(0, 0, 455.5, 635))
        #expect(upright.second == rect(495.5, 0, 371.5, 635))
        let uprightFlat = try #require(FoldHalves(reading(.upright(bent: false), statusBarHidden: false)))
        #expect(uprightFlat.card == upright.card)
        let turned = try #require(FoldHalves(reading(.turned(bent: true), statusBarHidden: false)))
        #expect(turned.card == rect(0, 495.5, 669, 421.5))
        #expect(turned.first == rect(0, 82, 669, 373.5))
        let turnedFlat = try #require(FoldHalves(reading(.turned(bent: false), statusBarHidden: false)))
        #expect(turnedFlat.card == turned.card)
        #expect(FoldHalves(reading(.cover)) == nil)
    }

    @Test func welcomeIsCentredAcrossTheWholeDisplayWhileFlatAndSitsOnAHalfWhileBent() throws {
        for hidden in [true, false] {
            let flatUpright = try #require(FoldHalves(reading(.upright(bent: false), statusBarHidden: hidden)))
            #expect(flatUpright.area(for: .welcome) == reading(.upright(bent: false), statusBarHidden: hidden).safeArea)
            let flatTurned = try #require(FoldHalves(reading(.turned(bent: false), statusBarHidden: hidden)))
            #expect(flatTurned.area(for: .welcome) == reading(.turned(bent: false), statusBarHidden: hidden).safeArea)
            // The whole area is across the band: the content is centred on the
            // display, not on a half.
            #expect(flatUpright.area(for: .welcome) != flatUpright.card)
            #expect(flatTurned.area(for: .welcome) != flatTurned.card)
        }
        let book = try #require(FoldHalves(reading(.upright(bent: true), statusBarHidden: false)))
        #expect(book.area(for: .welcome) == book.card)
        let standing = try #require(FoldHalves(reading(.turned(bent: true), statusBarHidden: false)))
        #expect(standing.area(for: .welcome) == standing.card)
    }

    @Test func theBootSitsOnTheUpperHalfWhenStandingAndAsACardInEveryOtherPose() throws {
        let standing = try #require(FoldHalves(reading(.turned(bent: true), statusBarHidden: false)))
        #expect(standing.area(for: .boot) == rect(0, 82, 669, 373.5))
        #expect(standing.area(for: .boot) == standing.first)
        for pose in [Pose.upright(bent: true), .upright(bent: false), .turned(bent: false)] {
            let halves = try #require(FoldHalves(reading(pose, statusBarHidden: false)))
            #expect(halves.area(for: .boot) == halves.card)
        }
    }

    @Test func aCardKeepsTheHalfWhereTheSystemPutsItsAlertsInEveryPose() throws {
        for pose in [Pose.upright(bent: true), .upright(bent: false), .turned(bent: true), .turned(bent: false)] {
            let halves = try #require(FoldHalves(reading(pose, statusBarHidden: false)))
            #expect(halves.area(for: .card) == halves.card)
        }
    }

    @Test func noPlaceOfAScreenCrossesTheFoldBandExceptWelcomeFlat() throws {
        for pose in [Pose.upright(bent: true), .upright(bent: false), .turned(bent: true), .turned(bent: false)] {
            let read = reading(pose, statusBarHidden: false)
            let band = try #require(read.division?.frame)
            let halves = try #require(FoldHalves(read))
            for screen in [FoldScreen.card, .boot] {
                #expect(!halves.area(for: screen).intersects(band), "\(screen) \(pose)")
            }
            let welcome = halves.area(for: .welcome)
            if read.division?.isActive == true {
                #expect(!welcome.intersects(band), "welcome bent \(pose)")
            } else {
                #expect(welcome.intersects(band), "welcome flat \(pose) is across the fold")
            }
        }
    }

    @Test func theFileBrowsersImportIsInTheToolbarOnTheCoverAlone() {
        #expect(FoldPose.cover.importIsInToolbar)
        for pose in [FoldPose.plain, .upright(bent: true), .upright(bent: false), .turned(bent: true), .turned(bent: false)] {
            #expect(!pose.importIsInToolbar)
        }
    }

    @Test func aSheetTurnedRisesToTwelvePointsBelowTheFoldAndStops() {
        for bent in [true, false] {
            for hidden in [true, false] {
                let reading = reading(.turned(bent: bent), statusBarHidden: hidden)
                // The safe area ends at 917 whether or not the status bar shows.
                #expect(FoldSheet.detentHeight(reading) == 917 - 495.5 - 12)
            }
        }
    }

    @Test func aSheetKeepsTheSystemsPlacementOnTheCoverAndUpright() {
        #expect(FoldSheet.detentHeight(reading(.cover)) == nil)
        #expect(FoldSheet.detentHeight(reading(.upright(bent: true))) == nil)
        #expect(FoldSheet.detentHeight(reading(.upright(bent: false))) == nil)
    }

    @Test func aSheetNeverShrinksBelowTwoHundredPoints() {
        var small = reading(.turned(bent: true))
        small.safeArea = rect(0, 0, 669, 600)
        #expect(FoldSheet.detentHeight(small) == 200)
    }
}

struct FoldRotationTests {
    @Test func theInnerDisplayTurnsAsAnIPadDoesAndTheCoverAsAnIPhoneDoes() {
        // An iPad turns.
        #expect(FoldPose.plain.interfaceRotates(idiomIsPhone: false))
        // An iPhone does not, and neither does the cover.
        #expect(!FoldPose.plain.interfaceRotates(idiomIsPhone: true))
        #expect(!FoldPose.cover.interfaceRotates(idiomIsPhone: true))
        // The inner display turns in every pose, though the device is a phone.
        for pose in [FoldPose.upright(bent: true), .upright(bent: false), .turned(bent: true), .turned(bent: false)] {
            #expect(pose.interfaceRotates(idiomIsPhone: true), "\(pose)")
        }
    }

    @Test func theHostDrivesTheGuestInASessionOnTheInnerDisplayAndNotOnTheCover() {
        let sessionPhases: [Phase] = [.booting(.normal), .running(.normal), .running(.recovery)]
        for phase in sessionPhases {
            let inner = SessionOrientationPolicy(
                phase: phase, interfaceRotates: FoldPose.upright(bent: true).interfaceRotates(idiomIsPhone: true))
            #expect(inner.hostDrivesGuestRotation, "\(phase)")
            let cover = SessionOrientationPolicy(
                phase: phase, interfaceRotates: FoldPose.cover.interfaceRotates(idiomIsPhone: true))
            #expect(!cover.hostDrivesGuestRotation, "\(phase)")
        }
        for phase in [Phase.home, .failed] {
            let inner = SessionOrientationPolicy(
                phase: phase, interfaceRotates: FoldPose.turned(bent: false).interfaceRotates(idiomIsPhone: true))
            #expect(!inner.hostDrivesGuestRotation, "\(phase)")
        }
    }

    @Test func movingBetweenTheCoverAndTheInnerDisplayIsAChangeTheSceneRootActsOn() {
        final class Token {}
        let scene = Token(), driver = Token()
        func inputs(rotates: Bool) -> SceneUpdateInputs {
            SceneUpdateInputs(
                scene: ObjectIdentifier(scene), driver: ObjectIdentifier(driver),
                sceneSize: CGSize(width: 466, height: 678), screenSize: CGSize(width: 466, height: 678),
                interfaceOrientation: 1, phase: .running(.normal), interfaceRotates: rotates)
        }
        #expect(inputs(rotates: true) != inputs(rotates: false))
        #expect(inputs(rotates: true) == inputs(rotates: true))
    }
}
