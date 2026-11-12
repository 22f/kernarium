// SPDX-License-Identifier: GPL-2.0-or-later

#if TIP_JAR

import SwiftUI
import UIKit

// The sender line above the developer's bubbles: a circular avatar, the
// developer's name and role, and a logo button for each profile. Every piece
// follows what the build holds: without the avatar the header keeps the app's
// own mark, without the name it reads the role alone, and a logo button exists
// only where its handle and its logo file both do. The values and the artwork
// are the maker's, local to an AppStore build (`Config/Developer.local.xcconfig`);
// nothing here names one.

/// What the header shows, read once when the sheet opens.
struct TipJarSenderContent {
    struct Link: Identifiable {
        enum Site: String { case github, x }

        let site: Site
        let url: URL
        let glyph: TipJarGlyph
        var id: String { site.rawValue }
        /// The brand's own name, which no language translates.
        var name: String { site == .github ? "GitHub" : "X" }
    }

    var name: String?
    var avatar: UIImage?
    var links: [Link]

    init(name: String? = nil, avatar: UIImage? = nil, links: [Link] = []) {
        self.name = name
        self.avatar = avatar
        self.links = links
    }

    /// The developer and the artwork files, each piece only where both parts of it exist.
    init(developer: TipJarDeveloper, avatarPath: String?, githubPath: String?, xPath: String?) {
        func glyph(_ path: String?) -> TipJarGlyph? {
            path.flatMap { try? String(contentsOfFile: $0, encoding: .utf8) }.flatMap { TipJarGlyph(svg: $0) }
        }
        var links: [Link] = []
        if let url = developer.githubURL, let glyph = glyph(githubPath) {
            links.append(Link(site: .github, url: url, glyph: glyph))
        }
        if let url = developer.xURL, let glyph = glyph(xPath) {
            links.append(Link(site: .x, url: url, glyph: glyph))
        }
        self.init(name: developer.name, avatar: avatarPath.flatMap { UIImage(contentsOfFile: $0) }, links: links)
    }

    /// The build's own: the Info.plist values and the artwork copied into the product.
    @MainActor
    static var live: TipJarSenderContent {
        typealias Configuration = TipJarDeveloperConfiguration
        return TipJarSenderContent(
            developer: Configuration.developer, avatarPath: Configuration.artworkPath(Configuration.avatarFile),
            githubPath: Configuration.artworkPath(Configuration.githubFile), xPath: Configuration.artworkPath(Configuration.xFile))
    }

    /// The sender line, as VoiceOver reads it and as the header shows it.
    var line: String {
        let role = String(localized: TipJarWords.sender)
        return name.map { "\($0) \u{00B7} \(role)" } ?? role
    }

    var spokenLine: String {
        let role = String(localized: TipJarWords.sender)
        return name.map { "\($0), \(role)" } ?? role
    }
}

/// A one-path logo drawn in the rectangle it is given, whole and unmodified.
private struct TipJarGlyphShape: Shape {
    let path: Path
    let box: CGRect

    init(_ glyph: TipJarGlyph) {
        path = Path(glyph.path)
        box = glyph.viewBox
    }

    nonisolated func path(in rect: CGRect) -> Path {
        let scale = min(rect.width / box.width, rect.height / box.height)
        let transform = CGAffineTransform(
            translationX: rect.midX - box.midX * scale, y: rect.midY - box.midY * scale
        ).scaledBy(x: scale, y: scale)
        return path.applying(transform)
    }
}

/// Who speaks. `links` is false for the second line, where the developer answers.
struct TipJarSender: View {
    let content: TipJarSenderContent
    var links = true
    @Environment(\.displayScale) private var displayScale
    @ScaledMetric(relativeTo: .caption) private var avatarMetric = 28.0
    @ScaledMetric(relativeTo: .caption) private var glyphMetric = 15.0

    /// A touch target no smaller than the system's.
    private let target = 44.0
    private let spacing = 8.0

    private var avatarSize: Double { min(avatarMetric, 56) }
    private var shownLinks: [TipJarSenderContent.Link] { links ? content.links : [] }

    var body: some View {
        ViewThatFits(in: .horizontal) {
            // One line when it fits.
            HStack(spacing: spacing) {
                avatar
                line.lineLimit(1)
                logos
            }
            // The logos move under the text when it does not.
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: spacing) {
                    avatar
                    line
                }
                logos.padding(.leading, avatarSize + spacing - logoInset)
            }
        }
    }

    /// The avatar, or the application's mark where the build has none. Decorative.
    @ViewBuilder
    private var avatar: some View {
        Group {
            if let image = content.avatar {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
                    .frame(width: avatarSize, height: avatarSize)
                    .clipShape(.circle)
                    // A hairline from the separator colour reads on light and dark.
                    .overlay(Circle().strokeBorder(Color(.separator), lineWidth: 1 / displayScale))
            } else {
                StackMark(lit: true, width: avatarSize * 0.7)
                    .frame(width: avatarSize, height: avatarSize)
                    .background(Color(.secondarySystemBackground), in: .circle)
            }
        }
        .accessibilityHidden(true)
    }

    /// The name and the role, read once as one element.
    private var line: some View {
        Text(verbatim: content.line)
            .font(.caption.weight(.semibold))
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(Text(verbatim: content.spokenLine))
            .accessibilityAddTraits(.isHeader)
    }

    private var logos: some View {
        HStack(spacing: 0) {
            ForEach(shownLinks) { link in
                SwiftUI.Link(destination: link.url) {
                    // The label colour, black or white as the appearance says, never the tint.
                    TipJarGlyphShape(link.glyph)
                        .fill(Color(.label))
                        .aspectRatio(link.glyph.viewBox.width / link.glyph.viewBox.height, contentMode: .fit)
                        .frame(height: glyphMetric)
                        // The glyph is small; the touch target is not.
                        .frame(minWidth: target, minHeight: target)
                        .contentShape(.rect)
                }
                .accessibilityLabel(Text(verbatim: link.name))
                .accessibilityIdentifier("tipjar-link-\(link.site.rawValue)")
            }
        }
    }

    /// The space between a logo's edge and its touch target's, so that the first
    /// logo under the text lines up with it.
    private var logoInset: Double {
        guard let first = shownLinks.first else { return 0 }
        let box = first.glyph.viewBox
        return max(0, (target - glyphMetric * box.width / box.height) / 2)
    }
}

#endif
