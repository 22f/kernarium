// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import CoreGraphics
import Foundation
import ImageIO

// The screenshot of a bug report, shown as a picture. The bytes were written by the virtual machine, so nothing
// about them is trusted: a picture is shown only when its name and its first bytes both say PNG or JPEG, it is
// one image, its declared size is within the bounds, and it is decoded as a thumbnail of a bounded size and never
// at its full size.

nonisolated enum SendFeedbackPictureFormat: Equatable, Sendable {
    case png, jpeg

    var typeIdentifier: String {
        switch self {
        case .png: "public.png"
        case .jpeg: "public.jpeg"
        }
    }
}

nonisolated enum SendFeedbackPicture {
    /// The most a side may declare.
    static let maxSide = 16_384
    /// The most pixels a picture may declare in all (64 million: 8,000 by 8,000).
    static let maxPixels = 64_000_000
    /// The longest side of what is decoded.
    static let thumbnailPixels = 2_048

    enum Refusal: Error, Equatable, Sendable {
        /// Not one PNG or JPEG image, or one whose size the decoder will not even tell (what it refuses as a
        /// decompression bomb).
        case notPicture
        /// More pixels than the bounds allow.
        case tooLarge
        /// The decoder would not make it.
        case undecodable
    }

    /// A decoded picture, and the size it declared.
    struct Thumbnail: @unchecked Sendable {
        let image: CGImage
        let width: Int
        let height: Int
    }

    /// The format an entry may be shown as: its name ends in the format's extension and its first bytes are the
    /// format's signature. Anything else is not a picture to show.
    static func format(named name: String, head: Data) -> SendFeedbackPictureFormat? {
        let lower = name.lowercased()
        let bytes = [UInt8](head.prefix(8))
        if lower.hasSuffix(".png"), bytes == [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A] { return .png }
        if lower.hasSuffix(".jpg") || lower.hasSuffix(".jpeg"), bytes.count >= 3, Array(bytes[0..<3]) == [0xFF, 0xD8, 0xFF] {
            return .jpeg
        }
        return nil
    }

    /// Whether a picture of this declared size may be decoded: both sides positive and within `maxSide`, and no more
    /// than `maxPixels` in all.
    static func withinBounds(width: Int, height: Int) -> Bool {
        width > 0 && height > 0 && width <= maxSide && height <= maxSide && width <= maxPixels / height
    }

    /// The picture in `data` at no more than `thumbnailPixels` on a side. Reads the declared size first; decodes
    /// only a thumbnail; never makes an image of the full size.
    static func thumbnail(of data: Data, as format: SendFeedbackPictureFormat) throws -> Thumbnail {
        try Task.checkCancellation()
        let options = [kCGImageSourceShouldCache: false, kCGImageSourceTypeIdentifierHint: format.typeIdentifier] as CFDictionary
        guard let source = CGImageSourceCreateWithData(data as CFData, options),
              CGImageSourceGetType(source) as String? == format.typeIdentifier,
              CGImageSourceGetCount(source) == 1,
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              let width = properties[kCGImagePropertyPixelWidth] as? Int,
              let height = properties[kCGImagePropertyPixelHeight] as? Int
        else { throw Refusal.notPicture }
        guard withinBounds(width: width, height: height) else { throw Refusal.tooLarge }
        try Task.checkCancellation()
        let make = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize: thumbnailPixels,
        ] as CFDictionary
        guard let image = CGImageSourceCreateThumbnailAtIndex(source, 0, make) else { throw Refusal.undecodable }
        try Task.checkCancellation()
        return Thumbnail(image: image, width: width, height: height)
    }
}

#endif
