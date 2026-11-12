// SPDX-License-Identifier: GPL-2.0-or-later

#if TIP_JAR

import SwiftUI
import UIKit

// The sender line above the developer's bubbles: a circular avatar, the
// developer's name and role, and a logo button for each profile. Every piece
// follows what the build holds: without the avatar the header keeps the app's
// own mark, without the name it reads the role alone, and a logo button exists
// only where its handle and its logo asset both do. The values are the
// maker's, local to an AppStore build (`Config/Developer.local.xcconfig`), and
// so are the assets, which are catalogs of their own; nothing here names one.

/// What the header shows, read once when the sheet opens.
struct TipJarSenderContent {
    struct Link: Identifiable {
        enum Site: String { case github, x }

        let site: Site
        let url: URL
        /// A template image: it takes the colour of the text around it.
        let logo: UIImage
        var id: String { site.rawValue }
        /// The brand's own name, which no language translates.
        var name: String { site == .github ? "GitHub" : "X" }
        var aspectRatio: Double { logo.size.height > 0 ? logo.size.width / logo.size.height : 1 }
    }

    var name: String?
    var avatar: UIImage?
    var links: [Link]

    init(name: String? = nil, avatar: UIImage? = nil, links: [Link] = []) {
        self.name = name
        self.avatar = avatar
        self.links = links
    }

    /// The developer and the artwork, each piece only where both parts of it exist.
    init(developer: TipJarDeveloper, avatar: UIImage?, githubLogo: UIImage?, xLogo: UIImage?) {
        var links: [Link] = []
        if let url = developer.githubURL, let logo = githubLogo {
            links.append(Link(site: .github, url: url, logo: logo.withRenderingMode(.alwaysTemplate)))
        }
        #if SHEET_X
        // The profile's address is Follow developer on X's to build.
        if let url = developer.xHandle.flatMap({ FollowXProfile.url(handle: $0) }), let logo = xLogo {
            links.append(Link(site: .x, url: url, logo: logo.withRenderingMode(.alwaysTemplate)))
        }
        #endif
        self.init(name: developer.name, avatar: avatar, links: links)
    }

    /// The build's own: the Info.plist values and the assets compiled into the product.
    @MainActor
    static var live: TipJarSenderContent {
        typealias Configuration = TipJarDeveloperConfiguration
        return TipJarSenderContent(
            developer: Configuration.developer, avatar: UIImage(named: Configuration.avatarAsset),
            githubLogo: UIImage(named: Configuration.githubAsset), xLogo: UIImage(named: Configuration.xAsset))
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
    /// The logos stay smaller than the name they follow, at any text size.
    private var glyphSize: Double { min(glyphMetric, 24) }
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
        HStack(spacing: spacing) {
            ForEach(shownLinks) { link in
                SwiftUI.Link(destination: link.url) {
                    // The label colour, black or white as the appearance says, never the tint.
                    Image(uiImage: link.logo)
                        .renderingMode(.template)
                        .resizable()
                        .aspectRatio(link.aspectRatio, contentMode: .fit)
                        .foregroundStyle(Color(.label))
                        .frame(height: glyphSize)
                        // The logo is small; the touch target is not.
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
        return max(0, (target - glyphSize * first.aspectRatio) / 2)
    }
}

#endif
