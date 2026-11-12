// SPDX-License-Identifier: GPL-2.0-or-later

import CoreGraphics
import Foundation
import Testing

@testable import AndariumTipJarLogic

/// The developer the tip sheet's header shows, and the marks it draws. Every value here is a
/// placeholder; the real ones are local to the maker's build.
@Suite struct TipJarDeveloperTests {
    @Test func theAddressesAreBuiltFromHandlesAndNeverTakenWhole() {
        #expect(TipJarDeveloper.githubURL(handle: "example")?.absoluteString == "https://github.com/example")
        #expect(TipJarDeveloper.xURL(handle: "example")?.absoluteString == "https://x.com/example")
        let developer = TipJarDeveloper(name: "Example", githubHandle: "example-dev", xHandle: "example_dev")
        #expect(developer.name == "Example")
        #expect(developer.githubURL?.absoluteString == "https://github.com/example-dev")
        #expect(developer.xURL?.absoluteString == "https://x.com/example_dev")
    }

    @Test func onlyAHandleOfTheSitesShapeBuildsAnAddress() {
        for bad in ["", "-a", "a-", "a b", "a/b", "a?b=c", "https://github.com/example", "@example", "ex\u{e9}", String(repeating: "a", count: 40)] {
            #expect(TipJarDeveloper.githubURL(handle: bad) == nil, "\(bad.debugDescription)")
        }
        for bad in ["", "a-b", "a b", "a/b", "@example", "https://x.com/example", String(repeating: "a", count: 16)] {
            #expect(TipJarDeveloper.xURL(handle: bad) == nil, "\(bad.debugDescription)")
        }
        #expect(TipJarDeveloper.githubURL(handle: String(repeating: "a", count: 39)) != nil)
        #expect(TipJarDeveloper.xURL(handle: String(repeating: "a", count: 15)) != nil)
    }

    @Test func eachPieceIsThereOnlyWhenItsValueIsWellFormed() {
        let none = TipJarDeveloper(name: nil, githubHandle: nil, xHandle: nil)
        #expect(none == TipJarDeveloper())
        let partial = TipJarDeveloper(name: "  ", githubHandle: "bad handle", xHandle: "example")
        #expect(partial.name == nil && partial.githubURL == nil && partial.xURL != nil)
        #expect(TipJarDeveloper.displayName("  Example  ") == "Example")
        #expect(TipJarDeveloper.displayName("a\nb") == nil)
        #expect(TipJarDeveloper.displayName(String(repeating: "a", count: 61)) == nil)
    }

    @Test func theInfoPlistKeysAndTheArtworkFolderAreTheOnesTheBuildWrites() {
        #expect(TipJarDeveloperConfiguration.nameKey == "AndariumDeveloperName")
        #expect(TipJarDeveloperConfiguration.githubKey == "AndariumDeveloperGitHub")
        #expect(TipJarDeveloperConfiguration.xKey == "AndariumDeveloperX")
        #expect(TipJarDeveloperConfiguration.folder == "DeveloperArtwork")
    }
}

@Suite struct TipJarGlyphTests {
    private func svg(_ path: String, box: String = "viewBox=\"0 0 100 80\"") -> String {
        "<!-- a comment <path d=\"M0 0\"/> -->\n<svg width=\"10\" height=\"10\" \(box) xmlns=\"http://www.w3.org/2000/svg\">\n<g clip-path=\"url(#c)\">\n<path d=\"\(path)\" fill=\"black\"/>\n</g></svg>"
    }

    @Test func theViewBoxAndTheFirstPathAreRead() throws {
        let glyph = try #require(TipJarGlyph(svg: svg("M10 10H90V70H10Z")))
        #expect(glyph.viewBox == CGRect(x: 0, y: 0, width: 100, height: 80))
        #expect(glyph.path.boundingBoxOfPath == CGRect(x: 10, y: 10, width: 80, height: 60))
        // No viewBox: the width and height.
        let sized = try #require(TipJarGlyph(svg: svg("M1 1L2 2", box: "")))
        #expect(sized.viewBox == CGRect(x: 0, y: 0, width: 10, height: 10))
    }

    @Test func absoluteAndRelativeCommandsDrawTheSameShape() throws {
        let absolute = try #require(TipJarGlyph(svg: svg("M10 10L50 10L50 40L10 40Z")))
        let relative = try #require(TipJarGlyph(svg: svg("m10 10l40 0 0 30-40 0z")))
        let horizontal = try #require(TipJarGlyph(svg: svg("M10 10h40v30H10z")))
        #expect(absolute.path.boundingBoxOfPath == relative.path.boundingBoxOfPath)
        #expect(absolute.path.boundingBoxOfPath == horizontal.path.boundingBoxOfPath)
        #expect(absolute.path.contains(CGPoint(x: 30, y: 25)) && !absolute.path.contains(CGPoint(x: 60, y: 25)))
    }

    @Test func curvesAndExponentsAndTouchingNumbersParse() throws {
        // A logo export writes 6.69539e-07, -.5 and 1.5.5 (two numbers) in its data.
        let glyph = try #require(TipJarGlyph(svg: svg("M10 10C20 0 30 0 40 10S60 20 70 10Q75 0 80 10T90 10 L90 6.69539e-07 L-.5 1.5.5 2Z")))
        #expect(!glyph.path.isEmpty)
        #expect(TipJarGlyph(svg: svg("M10 10 L20"))?.path == nil)
    }

    @Test func anArcDrawsACircleThatAHoleCanBeCutFrom() throws {
        let ring = try #require(TipJarGlyph(svg: svg("M50 4A46 46 0 1 0 50 96A46 46 0 1 0 50 4ZM50 24A26 26 0 1 1 50 76A26 26 0 1 1 50 24Z")))
        let box = ring.path.boundingBoxOfPath
        #expect(abs(box.width - 92) < 0.5 && abs(box.height - 92) < 0.5)
        #expect(ring.path.contains(CGPoint(x: 50, y: 10)), "the ring")
        #expect(!ring.path.contains(CGPoint(x: 50, y: 50)), "the hole")
        // Arc flags may touch the numbers after them, as exports write them.
        #expect(TipJarGlyph(svg: svg("M10 10a5 5 0 0110 0"))?.path.isEmpty == false)
    }

    @Test func anythingThatIsNotAPathOfTheSupportedShapeIsNil() {
        for bad in ["", "<svg viewBox=\"0 0 1 1\"></svg>", "<svg><path d=\"M0 0L1 1\"/></svg>",
                    "<svg viewBox=\"0 0 0 0\"><path d=\"M0 0L1 1\"/></svg>", "<svg viewBox=\"0 0 1 1\"><path d=\"X\"/></svg>",
                    "<svg viewBox=\"0 0 1 1\"><path d=\"M0 0\"/></svg>"] {
            #expect(TipJarGlyph(svg: bad) == nil, "\(bad.debugDescription)")
        }
    }
}
