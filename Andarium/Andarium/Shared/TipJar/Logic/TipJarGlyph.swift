// SPDX-License-Identifier: GPL-2.0-or-later

#if TIP_JAR

import CoreGraphics
import Foundation

// A one-path SVG mark (the developer's GitHub and X logos), read from the file
// the build copies into the product and drawn as a shape in the label colour.
// UIKit does not read an SVG file at run time, so the path is parsed here:
// absolute and relative lines, curves, quadratics, arcs and closes, which is
// what a logo export holds. Plain values and no UIKit or SwiftUI, so the
// AndariumCore package's tests build the same file.

nonisolated struct TipJarGlyph {
    /// The drawing's own coordinate space: the SVG's `viewBox`, or its width and height.
    let viewBox: CGRect
    let path: CGPath

    /// nil unless the text has a size and a first `<path>` with data that parses.
    init?(svg: String) {
        let text = Self.removingComments(svg)
        guard let box = Self.viewBox(in: text), box.width > 0, box.height > 0,
              let data = Self.pathData(in: text),
              let path = TipJarPathParser(data).parse() else { return nil }
        viewBox = box
        self.path = path
    }

    private static func removingComments(_ text: String) -> String {
        var result = text
        while let start = result.range(of: "<!--"), let end = result.range(of: "-->", range: start.upperBound..<result.endIndex) {
            result.removeSubrange(start.lowerBound..<end.upperBound)
        }
        return result
    }

    /// The value of an attribute of the first element named `tag`.
    private static func attribute(_ name: String, ofElement tag: String, in text: String) -> String? {
        let pattern = "<\(tag)\\b[^>]*?\\s\(name)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)')"
        guard let expression = try? NSRegularExpression(pattern: pattern, options: [.dotMatchesLineSeparators]),
              let match = expression.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) else { return nil }
        for group in 1...2 {
            if let range = Range(match.range(at: group), in: text) { return String(text[range]) }
        }
        return nil
    }

    private static func pathData(in text: String) -> String? {
        attribute("d", ofElement: "path", in: text)
    }

    private static func viewBox(in text: String) -> CGRect? {
        if let value = attribute("viewBox", ofElement: "svg", in: text) {
            let numbers = value.split(whereSeparator: { $0 == " " || $0 == "," || $0 == "\n" }).compactMap { Double($0) }
            if numbers.count == 4 { return CGRect(x: numbers[0], y: numbers[1], width: numbers[2], height: numbers[3]) }
        }
        if let width = attribute("width", ofElement: "svg", in: text).flatMap(Double.init),
           let height = attribute("height", ofElement: "svg", in: text).flatMap(Double.init) {
            return CGRect(x: 0, y: 0, width: width, height: height)
        }
        return nil
    }
}

/// The path data of an SVG `d` attribute as a path.
nonisolated struct TipJarPathParser {
    private let scalars: [UInt8]
    private var index = 0

    init(_ data: String) { scalars = Array(data.utf8) }

    func parse() -> CGPath? {
        var parser = self
        return parser.run()
    }

    private mutating func run() -> CGPath? {
        let path = CGMutablePath()
        var current = CGPoint.zero
        var start = CGPoint.zero
        var previousControl: CGPoint?
        var previousCommand: UInt8 = 0
        var command: UInt8 = 0
        var drew = false
        while true {
            skipSeparators()
            guard index < scalars.count else { break }
            let byte = scalars[index]
            if Self.isCommand(byte) {
                command = byte
                index += 1
            } else if command == 0 {
                return nil
            } else if command == UInt8(ascii: "M") {
                command = UInt8(ascii: "L")
            } else if command == UInt8(ascii: "m") {
                command = UInt8(ascii: "l")
            }
            let relative = command >= UInt8(ascii: "a")
            let origin = relative ? current : .zero
            let upper = relative ? command - 32 : command
            switch upper {
            case UInt8(ascii: "M"):
                guard let x = number(), let y = number() else { return nil }
                current = CGPoint(x: origin.x + x, y: origin.y + y)
                start = current
                path.move(to: current)
                previousControl = nil
            case UInt8(ascii: "L"):
                guard let x = number(), let y = number() else { return nil }
                current = CGPoint(x: origin.x + x, y: origin.y + y)
                path.addLine(to: current)
                drew = true
                previousControl = nil
            case UInt8(ascii: "H"):
                guard let x = number() else { return nil }
                current = CGPoint(x: origin.x + x, y: current.y)
                path.addLine(to: current)
                drew = true
                previousControl = nil
            case UInt8(ascii: "V"):
                guard let y = number() else { return nil }
                current = CGPoint(x: current.x, y: origin.y + y)
                path.addLine(to: current)
                drew = true
                previousControl = nil
            case UInt8(ascii: "C"):
                guard let x1 = number(), let y1 = number(), let x2 = number(), let y2 = number(),
                      let x = number(), let y = number() else { return nil }
                let control2 = CGPoint(x: origin.x + x2, y: origin.y + y2)
                current = CGPoint(x: origin.x + x, y: origin.y + y)
                path.addCurve(to: current, control1: CGPoint(x: origin.x + x1, y: origin.y + y1), control2: control2)
                drew = true
                previousControl = control2
            case UInt8(ascii: "S"):
                guard let x2 = number(), let y2 = number(), let x = number(), let y = number() else { return nil }
                let reflected = Self.reflect(previousControl, about: current, after: previousCommand, kinds: ["C", "S"])
                let control2 = CGPoint(x: origin.x + x2, y: origin.y + y2)
                current = CGPoint(x: origin.x + x, y: origin.y + y)
                path.addCurve(to: current, control1: reflected, control2: control2)
                drew = true
                previousControl = control2
            case UInt8(ascii: "Q"):
                guard let x1 = number(), let y1 = number(), let x = number(), let y = number() else { return nil }
                let control = CGPoint(x: origin.x + x1, y: origin.y + y1)
                current = CGPoint(x: origin.x + x, y: origin.y + y)
                path.addQuadCurve(to: current, control: control)
                drew = true
                previousControl = control
            case UInt8(ascii: "T"):
                guard let x = number(), let y = number() else { return nil }
                let control = Self.reflect(previousControl, about: current, after: previousCommand, kinds: ["Q", "T"])
                current = CGPoint(x: origin.x + x, y: origin.y + y)
                path.addQuadCurve(to: current, control: control)
                drew = true
                previousControl = control
            case UInt8(ascii: "A"):
                guard let rx = number(), let ry = number(), let rotation = number(), let large = flag(), let sweep = flag(),
                      let x = number(), let y = number() else { return nil }
                let end = CGPoint(x: origin.x + x, y: origin.y + y)
                Self.addArc(to: path, from: current, to: end, rx: rx, ry: ry, rotation: rotation, large: large, sweep: sweep)
                current = end
                drew = true
                previousControl = nil
            case UInt8(ascii: "Z"):
                path.closeSubpath()
                current = start
                previousControl = nil
            default:
                return nil
            }
            previousCommand = upper
        }
        return drew ? path : nil
    }

    private static func isCommand(_ byte: UInt8) -> Bool {
        "MmLlHhVvCcSsQqTtAaZz".utf8.contains(byte)
    }

    private mutating func skipSeparators() {
        while index < scalars.count, scalars[index] == 0x20 || scalars[index] == 0x2C || scalars[index] == 0x0A
            || scalars[index] == 0x09 || scalars[index] == 0x0D {
            index += 1
        }
    }

    /// A number: sign, digits, a fraction and an exponent; a second `.` or a sign begins the next one.
    private mutating func number() -> Double? {
        skipSeparators()
        let begin = index
        if index < scalars.count, scalars[index] == 0x2B || scalars[index] == 0x2D { index += 1 }
        var digits = 0
        while index < scalars.count, scalars[index] >= 0x30, scalars[index] <= 0x39 { index += 1; digits += 1 }
        if index < scalars.count, scalars[index] == 0x2E {
            index += 1
            while index < scalars.count, scalars[index] >= 0x30, scalars[index] <= 0x39 { index += 1; digits += 1 }
        }
        guard digits > 0 else { index = begin; return nil }
        if index < scalars.count, scalars[index] == 0x65 || scalars[index] == 0x45 {
            var probe = index + 1
            if probe < scalars.count, scalars[probe] == 0x2B || scalars[probe] == 0x2D { probe += 1 }
            if probe < scalars.count, scalars[probe] >= 0x30, scalars[probe] <= 0x39 {
                while probe < scalars.count, scalars[probe] >= 0x30, scalars[probe] <= 0x39 { probe += 1 }
                index = probe
            }
        }
        return Double(String(decoding: scalars[begin..<index], as: UTF8.self))
    }

    /// An arc flag: one digit, which may touch the next number.
    private mutating func flag() -> Bool? {
        skipSeparators()
        guard index < scalars.count, scalars[index] == 0x30 || scalars[index] == 0x31 else { return nil }
        defer { index += 1 }
        return scalars[index] == 0x31
    }

    private static func reflect(_ control: CGPoint?, about point: CGPoint, after command: UInt8, kinds: [Character]) -> CGPoint {
        guard let control, kinds.contains(Character(UnicodeScalar(command))) else { return point }
        return CGPoint(x: 2 * point.x - control.x, y: 2 * point.y - control.y)
    }

    /// An SVG elliptical arc as cubic curves (the endpoint form of the SVG specification, F.6).
    private static func addArc(
        to path: CGMutablePath, from p0: CGPoint, to p1: CGPoint,
        rx rawRx: Double, ry rawRy: Double, rotation: Double, large: Bool, sweep: Bool
    ) {
        var rx = abs(rawRx), ry = abs(rawRy)
        if p0 == p1 { return }
        if rx == 0 || ry == 0 { path.addLine(to: p1); return }
        let phi = rotation * .pi / 180
        let cosPhi = cos(phi), sinPhi = sin(phi)
        let dx = (p0.x - p1.x) / 2, dy = (p0.y - p1.y) / 2
        let x1 = cosPhi * dx + sinPhi * dy
        let y1 = -sinPhi * dx + cosPhi * dy
        let lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry)
        if lambda > 1 { rx *= lambda.squareRoot(); ry *= lambda.squareRoot() }
        let numerator = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1
        let denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1
        var factor = denominator == 0 ? 0 : (max(0, numerator / denominator)).squareRoot()
        if large == sweep { factor = -factor }
        let cxp = factor * rx * y1 / ry
        let cyp = -factor * ry * x1 / rx
        let cx = cosPhi * cxp - sinPhi * cyp + (p0.x + p1.x) / 2
        let cy = sinPhi * cxp + cosPhi * cyp + (p0.y + p1.y) / 2
        func angle(_ ux: Double, _ uy: Double, _ vx: Double, _ vy: Double) -> Double {
            let value = atan2(ux * vy - uy * vx, ux * vx + uy * vy)
            return value
        }
        let theta1 = angle(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry)
        var delta = angle((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry)
        if !sweep, delta > 0 { delta -= 2 * .pi }
        if sweep, delta < 0 { delta += 2 * .pi }
        let segments = max(1, Int((abs(delta) / (.pi / 2)).rounded(.up)))
        let step = delta / Double(segments)
        let alpha = 4.0 / 3.0 * tan(step / 4)
        var angleStart = theta1
        for _ in 0..<segments {
            let angleEnd = angleStart + step
            let c1 = (cos(angleStart) - alpha * sin(angleStart), sin(angleStart) + alpha * cos(angleStart))
            let c2 = (cos(angleEnd) + alpha * sin(angleEnd), sin(angleEnd) - alpha * cos(angleEnd))
            let e = (cos(angleEnd), sin(angleEnd))
            func map(_ point: (Double, Double)) -> CGPoint {
                let x = rx * point.0, y = ry * point.1
                return CGPoint(x: cosPhi * x - sinPhi * y + cx, y: sinPhi * x + cosPhi * y + cy)
            }
            path.addCurve(to: map(e), control1: map(c1), control2: map(c2))
            angleStart = angleEnd
        }
    }
}

#endif
