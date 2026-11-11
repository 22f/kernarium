// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// A titled section of Home: rows in one glass panel under a small heading,
/// and a note beneath.
struct HomeSection<Content: View, Footer: View>: View {
    let title: String
    @ViewBuilder var content: Content
    @ViewBuilder var footer: Footer

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(.secondary)
                .padding(.leading, 18)
            VStack(spacing: 0) { content }
                .glassEffect(.regular, in: .rect(cornerRadius: 26))
            footer
        }
    }
}

extension HomeSection where Footer == EmptyView {
    init(title: String, @ViewBuilder content: () -> Content) {
        self.init(title: title, content: content, footer: { EmptyView() })
    }
}

/// A note beneath a section.
struct SectionNote: View {
    let text: String

    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text)
            .font(.footnote)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
            .padding(.horizontal, 18)
    }
}

/// One row of a section: the icon tile, the title (and a line under it) and
/// the control on the right.
struct OptionRow<Control: View>: View {
    let title: String
    let symbol: String
    var detail: String?
    /// What the line under the title does when it is touched; the line then
    /// reads as a button. None leaves it plain text.
    var detailAction: (() -> Void)?
    /// The capability is off and unreachable: the tile and the words dim.
    var dimmed = false
    @ViewBuilder var control: Control

    var body: some View {
        HStack(spacing: 12) {
            IconTile(symbol: symbol, color: dimmed ? .gray : .accentColor)
                .opacity(dimmed ? 0.55 : 1)
            VStack(alignment: .leading, spacing: 1) {
                Text(title)
                    .lineLimit(1)
                    .minimumScaleFactor(0.85)
                    .opacity(dimmed ? 0.55 : 1)
                if let detail {
                    if let detailAction {
                        Button(action: detailAction) {
                            HStack(spacing: 4) {
                                Text(detail)
                                Image(systemName: "info.circle")
                            }
                            .font(.footnote)
                            .foregroundStyle(Color.accentColor)
                            .contentShape(.rect)
                        }
                        .buttonStyle(.plain)
                        .accessibilityIdentifier("option-detail-button")
                    } else {
                        Text(detail)
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                            .lineLimit(2)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
            Spacer(minLength: 8)
            control
        }
        .padding(.horizontal, 16)
        .padding(.vertical, detail == nil ? 0 : 6)
        .frame(minHeight: 54)
    }
}

/// The rule between two rows of a section.
struct RowDivider: View {
    var body: some View { Divider().padding(.leading, 58) }
}

// MARK: System

/// The guest's system in one section, as four rows of one shape: CPU and
/// memory states the vCPUs and RAM, with Edit opening the sheet that changes
/// them; Resolution states the guest's display, with Change opening its
/// choices (`ResolutionRow`); Storage states what the guest holds of its
/// capacity under a bar, with Resize; Snapshots states how many there are and
/// the space they use, with Manage opening them.
struct SystemSection: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        if let guest = model.guest {
            HomeSection(title: "Guest system") {
                SystemRow(
                    symbol: "cpu", title: "CPU and memory", subtitle: Self.summary(of: guest),
                    button: SystemRowButton(
                        title: "Edit", identifier: "home-system-edit", accessibilityName: "Edit system",
                        action: { model.sheet = .system }
                    )
                )
                RowDivider()
                ResolutionRow()
                RowDivider()
                StorageRow(guest: guest, stops: model.capacityStopsGiB, resize: { model.sheet = .resize })
                RowDivider()
                SnapshotsRow()
            }
        }
    }

    /// "6 vCPUs · 2.5 GiB".
    static func summary(of guest: Guest) -> String {
        "\(Formats.vcpus(guest.cores)) · \(Formats.size(mib: guest.memoryMiB))"
    }
}

/// The button at the end of a Guest system row.
struct SystemRowButton {
    let title: String
    let identifier: String
    /// What VoiceOver says when the title alone is not enough.
    var accessibilityName: String?
    var isEnabled = true
    let action: () -> Void

    /// The longest title: every button is as wide as this word, at any text
    /// size, so the buttons of the rows share one width and one edge.
    static let widestTitle = "Manage"
}

extension VerticalAlignment {
    private struct HeaderCenterID: AlignmentID {
        static func defaultValue(in context: ViewDimensions) -> CGFloat { context[VerticalAlignment.center] }
    }

    /// The middle of a row's title and the line under it. The icon tile and
    /// the button centre on it, so what hangs below the text (a bar and a
    /// note) never moves them off the words they belong to.
    fileprivate static let headerCenter = VerticalAlignment(HeaderCenterID.self)
}

/// One row of the Guest system section: the icon tile, the title with one
/// line under it, anything further under that line in the same column (a bar,
/// a note), and the row's button at the end. The text column reads as one
/// accessibility element; the button is the other. At accessibility text
/// sizes the button moves under the text, which then has the whole width.
struct SystemRow<Below: View>: View {
    @Environment(\.dynamicTypeSize) private var typeSize

    let symbol: String
    let title: String
    let subtitle: String
    let button: SystemRowButton
    /// What VoiceOver reads for the title and the line under it, where
    /// the written line does not read well.
    var spoken: String?
    @ViewBuilder var below: Below

    var body: some View {
        Group {
            if typeSize.isAccessibilitySize { stacked } else { inline }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }

    private var inline: some View {
        HStack(alignment: .headerCenter, spacing: 12) {
            tile
            VStack(alignment: .leading, spacing: 8) {
                heading.alignmentGuide(.headerCenter) { $0[VerticalAlignment.center] }
                below
            }
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
            .accessibilityElement(children: .combine)
            capsule
        }
    }

    private var stacked: some View {
        HStack(alignment: .top, spacing: 12) {
            tile
            VStack(alignment: .leading, spacing: 12) {
                VStack(alignment: .leading, spacing: 8) {
                    heading
                    below
                }
                .accessibilityElement(children: .combine)
                capsule
            }
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var tile: some View {
        IconTile(symbol: symbol).accessibilityHidden(true)
    }

    private var heading: some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(title)
            Text(subtitle)
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
        .modifier(SpokenLine(text: spoken))
    }

    private var capsule: some View {
        Button(action: button.action) {
            ZStack {
                Text(SystemRowButton.widestTitle).hidden()
                Text(button.title)
            }
            .font(.subheadline.weight(.semibold))
            .foregroundStyle(button.isEnabled ? Color.accentColor : Color(.tertiaryLabel))
            .padding(.horizontal, 16)
            .frame(minHeight: 34)
            .background(
                button.isEnabled ? Color.accentColor.opacity(0.13) : Color(.quaternarySystemFill),
                in: .capsule
            )
            .contentShape(.capsule)
            .fixedSize()
        }
        .buttonStyle(.plain)
        .disabled(!button.isEnabled)
        .accessibilityIdentifier(button.identifier)
        .accessibilityLabel(button.accessibilityName ?? button.title)
    }
}

extension SystemRow where Below == EmptyView {
    init(symbol: String, title: String, subtitle: String, spoken: String? = nil, button: SystemRowButton) {
        self.init(
            symbol: symbol, title: title, subtitle: subtitle, button: button, spoken: spoken,
            below: { EmptyView() })
    }
}

/// A row's title and line as one element that VoiceOver reads as `text`;
/// without `text` the words are read as they are written.
private struct SpokenLine: ViewModifier {
    let text: String?

    func body(content: Content) -> some View {
        if let text {
            content.accessibilityElement(children: .ignore).accessibilityLabel(text)
        } else {
            content
        }
    }
}

/// The third row of Guest system: how many snapshots there are and the space
/// all of them use, with Manage opening the Snapshots sheet. Home reads them
/// when none was read, after an operation and when a run ends, not each time
/// it appears (`SnapshotsModel.reload`).
struct SnapshotsRow: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let store = model.snapshots
        SystemRow(
            symbol: "clock.arrow.circlepath", title: SnapshotWords.title,
            // Not yet read: the line holds its height and says nothing.
            subtitle: store.listing == nil
                ? "\u{00A0}" : SnapshotWords.homeSubtitle(count: store.count, total: store.total),
            button: SystemRowButton(
                title: SnapshotWords.manage, identifier: "home-snapshots-manage",
                accessibilityName: SnapshotWords.manageAccessibility,
                action: { model.sheet = .snapshots }
            )
        )
        .task { await store.reload() }
    }
}

// MARK: Capabilities

/// What the guest may use of this device: the location and the microphone,
/// which iOS asks about when they are turned on, the gyroscope, and the
/// haptics on a device that has them.
struct CapabilitiesSection: View {
    @Environment(AppModel.self) private var model

    /// The rows, top to bottom; VoiceOver reads them in this order.
    enum Row: CaseIterable {
        case location, microphone, gyroscope, haptics
    }

    /// The rows a device shows: Haptics only where there is a Taptic Engine
    /// to switch. An iPad, a Mac and the Simulator have none, and a switch
    /// that could do nothing is not shown.
    static func rows(hapticsAvailable: Bool) -> [Row] {
        Row.allCases.filter { $0 != .haptics || hapticsAvailable }
    }

    var body: some View {
        if model.guest != nil {
            HomeSection(title: "Capabilities") {
                ForEach(Array(Self.rows(hapticsAvailable: model.hapticsAvailable).enumerated()), id: \.element) { index, row in
                    if index > 0 { RowDivider() }
                    switch row {
                    case .location: LocationRow()
                    case .microphone: MicrophoneRow()
                    case .gyroscope: GyroscopeRow()
                    case .haptics: HapticsRow()
                    }
                }
            } footer: {
                if let reason = model.motionSensorsProblem ?? Self.gyroscopeUnavailable(available: model.motionSensorsAvailable) {
                    SectionNote(reason)
                }
            }
            .id("capabilities")
        }
    }

    /// Why Gyroscope cannot be turned on; nil when it can, which needs no note.
    static func gyroscopeUnavailable(available: Bool) -> String? {
        available ? nil : "This device has no gyroscope or accelerometer to share."
    }
}

/// The words under Microphone while iOS refuses access.
enum CapabilityWords {
    static let microphoneDenied = "Not allowed in Settings"
}

struct MicrophoneRow: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let denied = model.microphoneState == .denied
        OptionRow(
            title: "Microphone", symbol: "mic.fill",
            detail: denied ? CapabilityWords.microphoneDenied : nil, dimmed: denied
        ) {
            Toggle("Microphone", isOn: Binding(
                get: { model.microphoneShownOn },
                set: { model.setMicrophone($0) }
            ))
            .labelsHidden()
        }
        .accessibilityHint(denied ? "Andarium is not allowed to use the microphone. Turning this on offers to open Settings." : "")
    }
}

struct GyroscopeRow: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        OptionRow(title: "Gyroscope", symbol: "gyroscope") {
            Toggle("Gyroscope", isOn: Binding(
                get: { model.motionSensorsOn },
                set: { model.setMotionSensors($0) }
            ))
            .labelsHidden()
            .disabled(!model.motionSensorsAvailable)
        }
    }
}

/// What the guest plays on this device's Taptic Engine: Android's vibrator
/// and haptic feedback. A game controller's rumble is the controller's and
/// stays as it is.
struct HapticsRow: View {
    @Environment(AppModel.self) private var model

    /// The vibrating phone, which reads as haptics where "waveform" reads as
    /// sound.
    static let symbol = "iphone.radiowaves.left.and.right"

    var body: some View {
        OptionRow(title: "Haptics", symbol: Self.symbol) {
            Toggle("Haptics", isOn: Binding(
                get: { model.hapticsOn },
                set: { model.setHaptics($0) }
            ))
            .labelsHidden()
            .accessibilityIdentifier("home-haptics-switch")
        }
    }
}

struct LocationRow: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let access = model.locationAuthorization
        let shownOn = model.locationShownOn
        let approximate = access.isApproximate && shownOn
        OptionRow(
            title: LocationWords.title, symbol: "location.fill",
            detail: LocationWords.detail(for: access, shownOn: shownOn),
            detailAction: approximate ? { model.permissionPrompt = .location } : nil,
            dimmed: access.refuses
        ) {
            Toggle(LocationWords.title, isOn: Binding(
                get: { model.locationShownOn },
                set: { model.setLocation($0) }
            ))
            .labelsHidden()
            .accessibilityIdentifier("home-location-switch")
        }
        .accessibilityHint(LocationWords.hint(for: access, shownOn: shownOn))
    }
}

/// When iOS has refused the microphone there is nothing to ask again: say so,
/// and offer Andarium's page in Settings.
struct MicrophoneAccessAlert: ViewModifier {
    @Environment(AppModel.self) private var model

    func body(content: Content) -> some View {
        @Bindable var model = model
        content.alert(
            "Allow microphone access", isPresented: Binding(
                get: { model.permissionPrompt == .microphone },
                set: { if !$0 { model.permissionPrompt = nil } }
            )
        ) {
            Button("Open Settings") { model.openMicrophoneSettings() }
            Button("Not now", role: .cancel) {}
        } message: {
            Text("iOS is not allowing Andarium to use the microphone. Turn on Microphone for Andarium in Settings, then come back to use it.")
        }
    }
}

extension View {
    func microphoneAccessAlert() -> some View { modifier(MicrophoneAccessAlert()) }
}

/// When iOS refuses the location, or gives only an approximate one, there is
/// nothing to ask: say why, and offer Andarium's page in Settings where it has
/// the way out.
struct LocationAccessAlert: ViewModifier {
    @Environment(AppModel.self) private var model

    func body(content: Content) -> some View {
        @Bindable var model = model
        let words = LocationWords.alert(for: model.locationAuthorization)
        content.alert(
            words?.title ?? "", isPresented: Binding(
                get: { model.permissionPrompt == .location && words != nil },
                set: { if !$0, model.permissionPrompt == .location { model.permissionPrompt = nil } }
            )
        ) {
            if words?.offersSettings == true {
                Button("Open Settings") { model.openLocationSettings() }
                Button("Not now", role: .cancel) {}
            } else {
                Button("OK", role: .cancel) {}
            }
        } message: {
            Text(words?.message ?? "")
        }
    }
}

extension View {
    func locationAccessAlert() -> some View { modifier(LocationAccessAlert()) }
}

#if !APP_STORE

// MARK: JIT

/// Whether the guest runs on the JIT engine, which a debugger the person
/// attached (StikDebug) makes possible, and what the next start can do about
/// it. It applies when the guest next starts. An App Store build has no JIT
/// and no such section.
struct JitSection: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        if model.jitAvailable, model.guest != nil {
            HomeSection(title: "JIT") {
                OptionRow(title: "JIT", symbol: "bolt.fill") {
                    Toggle("JIT", isOn: Binding(
                        get: { model.jitOn },
                        set: { model.setJit($0) }
                    ))
                    .labelsHidden()
                }
            } footer: {
                SectionNote(model.jitStatus.footer)
            }
            .id("jit")
            .task { model.refreshJit() }
        }
    }
}

#endif

// MARK: Backup

/// Whether iCloud and computer backups of this device take the guest's data.
struct BackupOption: View {
    @Binding var isOn: Bool

    var body: some View {
        OptionRow(title: "Back up guest data", symbol: "icloud.fill") {
            Toggle("Back up guest data", isOn: $isOn).labelsHidden()
        }
    }
}

/// What the Back up guest data row does to iCloud and computer backups.
/// Snapshots are never in a backup, which the note says once there are some.
struct BackupNote: View {
    let host: HostDevice
    var hasSnapshots = false

    var body: some View {
        SectionNote(SnapshotWords.backupNote(device: host.name, hasSnapshots: hasSnapshots))
    }
}

struct BackupSection: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        if let guest = model.guest {
            HomeSection(title: "Backup") {
                BackupOption(isOn: Binding(
                    get: { guest.includeInBackups },
                    set: { model.guest?.includeInBackups = $0 }
                ))
            } footer: {
                BackupNote(host: model.host, hasSnapshots: model.snapshots.count > 0)
            }
            .id("backup")
        }
    }
}
