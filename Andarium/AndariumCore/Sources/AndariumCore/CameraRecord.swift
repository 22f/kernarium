// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// How the platform presented each of the guest's cameras during a session,
/// how many stills of the guest's cameras did not end in a picture, and why for
/// the last few, as the runtime wrote them into the session record's camera
/// slot (darwin/runtime/session_record.hpp).
///
/// Every field is a number or one of the runtime's fixed words: the record
/// holds no path, no name and no message of the system's. The slot is blank
/// until the first camera note or failure, and a blank slot is no claim: the
/// session presented no camera, or it took no still that failed. A line this
/// reader does not know is skipped, and a field is read by its name, so a later
/// runtime can add fields.
public struct CameraRecord: Equatable, Sendable {
    /// A size in pixels; the record writes `-` for one it does not know.
    public struct Size: Equatable, Sendable {
        public var width: Int
        public var height: Int

        public init(width: Int, height: Int) {
            self.width = width
            self.height = height
        }

        init?(_ word: Substring) {
            let parts = word.split(separator: "x", omittingEmptySubsequences: false)
            guard parts.count == 2, let width = Int(parts[0]), let height = Int(parts[1]) else { return nil }
            self.width = width
            self.height = height
        }

        var text: String { "\(width)x\(height)" }
    }

    /// One still that did not end in a picture.
    public struct Failure: Equatable, Sendable {
        public var at: Date
        /// The camera's index as the guest sees it, and where it is mounted
        /// (`back`, `front` or `external`).
        public var camera: Int
        public var position: String
        /// Where the still stopped, in the runtime's word: `refused` (before
        /// it reached the platform), `not-started`, `framework-error`,
        /// `missing-data`, `resolved-size` (the photo output made another
        /// size), `encoded-size` (the file states another size, or none),
        /// `file-size`, `guest-buffer`, `timeout` or `unknown`.
        public var stage: String
        /// The JPEG size the guest asked for, the size the photo output
        /// resolved to and the size the file's frame header states; nil where
        /// the record does not know it.
        public var asked: Size?
        public var resolved: Size?
        public var encoded: Size?
        /// Whether the photo output turns its stills to an earlier model's
        /// sensor orientation and whether that was on: `yes`, `no` or
        /// `unknown`.
        public var compensationSupported: String
        public var compensationEnabled: String
        /// The Exif orientation (1 to 8) of the photo the host's output gave for
        /// the capture; nil where it gave none.
        public var exifOrientation: Int?
        /// The framework's error as one of its domain words (`avfoundation`,
        /// `osstatus`, `cocoa`, `posix`, `mach` or `other`; `none` for no
        /// error) and its code.
        public var errorDomain: String
        public var errorCode: Int?
    }

    /// How one camera was presented to the guest, by the guest's number of it:
    /// the lines the runtime wrote (`mount`, `ratio`, `video`, `photo` and
    /// `refused`); a line the slot had no room for is nil.
    public struct Camera: Equatable, Sendable {
        /// The camera as discovered and given its anchor.
        public struct Mount: Equatable, Sendable {
            /// `back`, `front` or `external`.
            public var position: String
            /// The device type's code (`CameraDeviceType`).
            public var type: Int
            /// The rotation coordinator's fixed clockwise angle for portrait,
            /// portrait upside down, landscape right and landscape left; nil
            /// where the platform stated none.
            public var angles: [Int?]
            /// The orientation stated to the guest and the turn set on the video
            /// connection, in degrees, and whether the camera imitates an earlier
            /// front camera.
            public var stated: Int
            public var turn: Int
            public var earlier: Bool
            /// The camera's anchor size, the dynamic aspect ratios it lists (a
            /// mask) and its field of view in degrees.
            public var anchor: Size?
            public var ratios: Int
            public var fieldOfView: Int?
            /// For a front camera: how many front devices the platform listed and
            /// whether any has a square format or lists dynamic ratios.
            public var frontDevices: Int?
            public var squareFormats: Bool?
            public var dynamicRatios: Bool?
        }

        /// The device's dynamic aspect ratio: the one pinned, the one the device
        /// reports, the output dimensions it reports and the completion's error
        /// code (nil while it is pending).
        public struct Ratio: Equatable, Sendable {
            public var pinned: String
            public var now: String
            public var dimensions: Size?
            public var error: Int?
            public var pending: Bool
        }

        /// The video connection: its angle and mirroring by default and now,
        /// whether the angle asked for is supported, the size the output was asked
        /// for and the first frame's.
        public struct Video: Equatable, Sendable {
            public var defaultAngle: Int?
            public var angle: Int?
            public var angleSupported: String
            public var defaultMirrored: Int?
            public var mirrored: Int?
            public var asked: Size?
            public var first: Size?
        }

        /// The photo output: the compensation, the photo connection, the largest
        /// photo, the last photo's Exif orientation and two facts of the device.
        public struct Photo: Equatable, Sendable {
            public var compensationSupported: String
            public var compensationEnabled: String
            public var defaultAngle: Int?
            public var angle: Int?
            public var defaultMirrored: Int?
            public var mirrored: Int?
            public var maximum: Size?
            public var exifOrientation: Int?
            public var centerStage: String
            public var distortionCorrected: String
        }

        /// The refusals of the camera's admissions the platform logged and the
        /// reason's code of the last (`Refusal`).
        public struct Refusals: Equatable, Sendable {
            public var count: Int
            public var last: Int?
        }

        public var index: Int
        public var mount: Mount?
        public var ratio: Ratio?
        public var video: Video?
        public var photo: Photo?
        public var refusals: Refusals?
    }

    /// The guest's cameras the slot says something of, by number.
    public var cameras: [Camera]

    /// The stills that failed during the session, and the newest of them that
    /// fit the slot, oldest first.
    public var failed: Int
    public var recent: [Failure]

    /// Reads the camera slot out of a session record's text (or the slot
    /// alone); nil when the slot is blank, as in a session that presented no
    /// camera and had no failure and in the records of the earlier layouts, or of
    /// a version this reader does not know.
    public init?(text: String) {
        var header = false
        var failed: Int?
        var recent: [Failure] = []
        var cameras: [Int: Camera] = [:]
        // The kernel's quoted output is not the record's own lines.
        let own = text.components(separatedBy: "\nkernel-output\n")[0]
        for line in own.split(separator: "\n", omittingEmptySubsequences: true) {
            let words = line.split(separator: " ", omittingEmptySubsequences: true)
            guard let first = words.first else { continue }
            switch first {
            case "camera":
                header = words.count >= 2 && words[1] == "1"
            case "stills":
                // stills failed N
                if words.count >= 3, words[1] == "failed" { failed = Int(words[2]) }
            case "still":
                if let failure = Self.failure(words) { recent.append(failure) }
            case "mount", "ratio", "video", "photo", "refused":
                Self.note(String(first), words, into: &cameras)
            default:
                break
            }
        }
        guard header, let failed else { return nil }
        self.failed = failed
        self.recent = recent
        self.cameras = cameras.values.sorted { $0.index < $1.index }
    }

    /// A word's value as the record writes a number: `-` (or a word that is no
    /// number) is nil.
    private static func number(_ word: Substring?) -> Int? {
        word.flatMap { Int($0) }
    }

    /// `WORD camera N ...`, read by the field names; the camera's lines are
    /// gathered by its number.
    private static func note(_ kind: String, _ words: [Substring], into cameras: inout [Int: Camera]) {
        func value(after name: Substring) -> Substring? {
            guard let at = words.firstIndex(of: name), at + 1 < words.count else { return nil }
            return words[at + 1]
        }
        func values(after name: Substring, count: Int) -> [Substring]? {
            guard let at = words.firstIndex(of: name), at + count < words.count else { return nil }
            return Array(words[(at + 1)...(at + count)])
        }
        func fact(_ name: Substring) -> String { value(after: name).map(String.init) ?? "unknown" }
        guard let index = number(value(after: "camera")) else { return }
        var camera = cameras[index] ?? Camera(index: index)
        switch kind {
        case "mount":
            guard let cameraAt = words.firstIndex(of: "camera"), cameraAt + 2 < words.count,
                let type = number(value(after: "type")), let stated = number(value(after: "stated")),
                let turn = number(value(after: "turn")), let angles = values(after: "angles", count: 4)
            else { return }
            camera.mount = Camera.Mount(
                position: String(words[cameraAt + 2]), type: type, angles: angles.map { number($0) },
                stated: stated, turn: turn, earlier: value(after: "earlier") == "yes",
                anchor: value(after: "anchor").flatMap { Size($0) }, ratios: number(value(after: "ratios")) ?? 0,
                fieldOfView: number(value(after: "fov")), frontDevices: number(value(after: "fronts")),
                squareFormats: value(after: "square").map { $0 == "yes" },
                dynamicRatios: value(after: "dynamic").map { $0 == "yes" })
        case "ratio":
            guard let pinned = value(after: "pinned"), let now = value(after: "now") else { return }
            let error = value(after: "error")
            camera.ratio = Camera.Ratio(
                pinned: String(pinned), now: String(now), dimensions: value(after: "dims").flatMap { Size($0) },
                error: number(error), pending: error == "pending")
        case "video":
            let mirror = values(after: "mirror", count: 2)
            camera.video = Camera.Video(
                defaultAngle: number(value(after: "default")), angle: number(value(after: "set")),
                angleSupported: fact("supported"), defaultMirrored: number(mirror?[0]), mirrored: number(mirror?[1]),
                asked: value(after: "asked").flatMap { Size($0) }, first: value(after: "first").flatMap { Size($0) })
        case "photo":
            let mirror = values(after: "mirror", count: 2)
            camera.photo = Camera.Photo(
                compensationSupported: fact("supported"), compensationEnabled: fact("enabled"),
                defaultAngle: number(value(after: "default")), angle: number(value(after: "set")),
                defaultMirrored: number(mirror?[0]), mirrored: number(mirror?[1]),
                maximum: value(after: "max").flatMap { Size($0) }, exifOrientation: number(value(after: "exif")),
                centerStage: fact("center"), distortionCorrected: fact("distortion"))
        default:
            guard let count = number(value(after: "count")) else { return }
            camera.refusals = Camera.Refusals(count: count, last: number(value(after: "last")))
        }
        cameras[index] = camera
    }

    /// `still at T camera N POSITION stage STAGE asked S resolved S encoded S
    /// compensation supported X enabled Y exif E error DOMAIN [CODE]`, read by the
    /// field names.
    private static func failure(_ words: [Substring]) -> Failure? {
        func value(after name: Substring) -> Substring? {
            guard let at = words.firstIndex(of: name), at + 1 < words.count else { return nil }
            return words[at + 1]
        }
        guard let time = value(after: "at").flatMap({ TimeInterval($0) }),
            let camera = value(after: "camera").flatMap({ Int($0) }),
            let cameraAt = words.firstIndex(of: "camera"), cameraAt + 2 < words.count,
            let stage = value(after: "stage")
        else { return nil }
        let domain = value(after: "error").map(String.init) ?? "none"
        let code: Int? = {
            guard let at = words.firstIndex(of: "error"), at + 2 < words.count else { return nil }
            return Int(words[at + 2])
        }()
        return Failure(
            at: Date(timeIntervalSince1970: time), camera: camera, position: String(words[cameraAt + 2]),
            stage: String(stage), asked: value(after: "asked").flatMap { Size($0) },
            resolved: value(after: "resolved").flatMap { Size($0) },
            encoded: value(after: "encoded").flatMap { Size($0) },
            compensationSupported: value(after: "supported").map(String.init) ?? "unknown",
            compensationEnabled: value(after: "enabled").map(String.init) ?? "unknown",
            exifOrientation: number(value(after: "exif")),
            errorDomain: domain, errorCode: code)
    }

    /// The device types the record's codes stand for (`CameraDeviceType`).
    static let deviceTypes = [
        "other", "wide-angle", "ultra wide", "telephoto", "dual", "dual wide", "triple", "external",
        "outer ultra wide", "inner ultra wide",
    ]

    /// The reasons the platform refuses a camera's admission (`Refusal`).
    static let refusalReasons = [
        "no anchor format", "the device's formats changed", "the cameras cannot share a multi-camera session",
        "the session took no input", "the session took no output", "over the multi-camera hardware budget",
        "the anchor makes no such photo", "no Bayer RAW format", "the platform raised",
        "the device could not be put back as it was", "the device's aspect ratio was not the one pinned",
    ]

    /// The names of the dynamic aspect ratios a mask lists, lowest bit first.
    static func ratioNames(_ mask: Int) -> String {
        let names = ["1x1", "16x9", "9x16", "4x3", "3x4"]
        let listed = names.enumerated().filter { mask & (1 << $0.offset) != 0 }.map(\.element)
        return listed.isEmpty ? "none" : listed.joined(separator: " ")
    }

    private static func degrees(_ value: Int?) -> String { value.map { "\($0)" } ?? "unknown" }

    private static func yesNo(_ value: Bool?) -> String { value.map { $0 ? "yes" : "no" } ?? "unknown" }

    /// How a camera was presented, in words, a line to each part the record has.
    func report(of camera: Camera) -> [String] {
        var lines: [String] = []
        let position = camera.mount?.position ?? "unknown position"
        lines.append("Camera \(camera.index) (\(position)):")
        if let mount = camera.mount {
            let type = mount.type >= 0 && mount.type < Self.deviceTypes.count ? Self.deviceTypes[mount.type] : "type \(mount.type)"
            let angles = mount.angles.map { Self.degrees($0) }.joined(separator: ", ")
            lines.append(
                "  mounting: device type \(type); the platform's fixed angles for portrait, portrait upside down, "
                    + "landscape right and landscape left are \(angles); the guest is told \(mount.stated); "
                    + "the video connection is turned \(mount.turn); imitates an earlier front camera: "
                    + "\(mount.earlier ? "yes" : "no").")
            var anchor = "  anchor: \(mount.anchor?.text ?? "none"), aspect ratios listed: \(Self.ratioNames(mount.ratios)), "
            anchor += "field of view \(Self.degrees(mount.fieldOfView)) degrees."
            if let fronts = mount.frontDevices {
                anchor += " Front devices listed: \(fronts); any with a square format: \(Self.yesNo(mount.squareFormats))"
                anchor += "; any that lists aspect ratios: \(Self.yesNo(mount.dynamicRatios))."
            }
            lines.append(anchor)
        }
        if let ratio = camera.ratio {
            let error = ratio.pending ? "pending" : Self.degrees(ratio.error)
            lines.append(
                "  aspect ratio: pinned \(ratio.pinned), the device holds \(ratio.now), output dimensions "
                    + "\(ratio.dimensions?.text ?? "none"), the change's completion error \(error).")
        }
        if let video = camera.video {
            lines.append(
                "  video connection: angle \(Self.degrees(video.defaultAngle)) by default and \(Self.degrees(video.angle)) now "
                    + "(the angle asked for is supported: \(video.angleSupported)); mirrored \(Self.degrees(video.defaultMirrored)) "
                    + "by default and \(Self.degrees(video.mirrored)) now; the output was asked for \(video.asked?.text ?? "none"); "
                    + "the first frame was \(video.first?.text ?? "not seen").")
        }
        if let photo = camera.photo {
            lines.append(
                "  photo output: orientation compensation supported \(photo.compensationSupported) and enabled "
                    + "\(photo.compensationEnabled); photo connection angle \(Self.degrees(photo.defaultAngle)) by default and "
                    + "\(Self.degrees(photo.angle)) now, mirrored \(Self.degrees(photo.defaultMirrored)) and "
                    + "\(Self.degrees(photo.mirrored)); largest photo \(photo.maximum?.text ?? "unknown"); Exif orientation of the "
                    + "last photo \(photo.exifOrientation.map { "\($0)" } ?? "none yet"); Center Stage active "
                    + "\(photo.centerStage); distortion correction \(photo.distortionCorrected).")
        }
        if let refusals = camera.refusals {
            let reason = refusals.last.flatMap { $0 >= 0 && $0 < Self.refusalReasons.count ? Self.refusalReasons[$0] : nil }
            lines.append(
                "  refused admissions: \(refusals.count), the last for the reason: \(reason ?? "unknown").")
        }
        return lines
    }

    /// The record in words, for the bug report.
    public func report() -> String {
        var lines: [String] = []
        for camera in cameras { lines.append(contentsOf: report(of: camera)) }
        if !cameras.isEmpty { lines.append("") }
        if failed == 0 {
            lines.append("No still of the guest's cameras failed to end in a picture.")
        } else {
            lines.append(
                failed == 1
                    ? "1 still of the guest's cameras did not end in a picture."
                    : "\(failed) stills of the guest's cameras did not end in a picture.")
        }
        if recent.count < failed {
            lines.append("The last \(recent.count) that the record holds, oldest first:")
        }
        let formatter = ISO8601DateFormatter()
        for failure in recent {
            var line = "\(formatter.string(from: failure.at)): camera \(failure.camera) (\(failure.position)), "
            line += "stopped at \(failure.stage)"
            line += ", asked \(failure.asked?.text ?? "none")"
            line += ", resolved \(failure.resolved?.text ?? "unknown")"
            line += ", file states \(failure.encoded?.text ?? "unknown")"
            line += ", orientation compensation supported \(failure.compensationSupported)"
            line += " and enabled \(failure.compensationEnabled)"
            line += ", the host photo's Exif orientation \(failure.exifOrientation.map { "\($0)" } ?? "unknown")"
            if failure.errorDomain != "none" {
                line += ", the framework's error \(failure.errorDomain)"
                if let code = failure.errorCode { line += " \(code)" }
            }
            lines.append(line + ".")
        }
        return lines.joined(separator: "\n")
    }
}
