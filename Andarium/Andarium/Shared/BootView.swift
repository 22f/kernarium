// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The boot, from Andarium's start to the guest's `sys.boot_completed`: a
/// status line with a live dot, what is starting and the elapsed time; the
/// title; and the steps in one panel, with no bar or percentage of the whole
/// (only the disk write, which knows its length, says one). First start,
/// update, recovery and restart are variants of the same screen. A launch
/// has one run, so there is no Cancel.
///
/// A start that has run for three minutes (`SlowStart`; eight for a first
/// start and the first after an update) without reaching Android puts a notice
/// above the steps: what the screen is waiting for, the step and the time, Stop
/// (which asks first) and, where Send feedback is compiled in, Send feedback.
/// The boot goes on behind it, and it goes away when the boot completes. There
/// is no way to dismiss it. It sits above the steps and pushes them down;
/// where the screen is short, the screen scrolls.
///
/// A normal boot lays the guest's display out from the first frame and shows
/// it dimmed behind this screen, so the guest is seen starting. The dim is a
/// plain translucent fill and the panels are flat fills too, not materials or
/// glass: the display costs the presentation pass it costs when it is
/// visible, and the screen asks the render server for no blur of it.
/// Recovery keeps an opaque screen.
struct BootView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.colorScheme) private var colorScheme
    let kind: BootKind

    @State private var stage = 0
    @State private var startedAt = Date()

    struct Step {
        let title: String
        let detail: String
        let seconds: Double
    }

    var body: some View {
        let guest = model.guest ?? Guest.fresh(on: model.host)
        let steps = shownSteps(guest: guest)
        let frozen = model.frozenBootStage.map { min($0, steps.count - 1) }
        let current = frozen ?? stage
        VStack(spacing: 0) {
            statusLine(frozen: frozen != nil)
            // Centred while it fits; taller than the screen (the notice, at the
            // largest text size), it scrolls from its top.
            GeometryReader { proxy in
                ScrollView {
                    content(guest: guest, system: model.system, steps: steps, current: current)
                        .frame(minHeight: proxy.size.height, alignment: .center)
                }
                .scrollBounceBehavior(.basedOnSize)
                #if !APP_STORE
                .defaultScrollAnchor(model.previewBootScrollsToBottom ? .bottom : nil)
                #endif
            }
        }
        .onCardHalf(.boot)
        .background { backdrop }
        .environment(\.colorScheme, dimmed ? .dark : colorScheme)
        .statusBarHidden(true)
        .task {
            // The notice comes from the clock, not from the console: a boot
            // whose line never comes is exactly the one it is for.
            while !Task.isCancelled {
                model.updateSlowStart()
                try? await Task.sleep(for: .seconds(1))
            }
        }
        .onChange(of: model.slowStart.showing) { _, showing in
            guard showing else { return }
            Self.announce(Self.slowTitle(longByNature: model.slowStart.longByNature))
        }
        .onChange(of: model.slowStart.stopping) { _, stopping in
            guard stopping else { return }
            Self.announce(String(localized: "Stopping\u{2026}"))
        }
        .task(id: kind) {
            guard frozen == nil else { return }
            if model.driver != nil {
                await followRuntime(stepCount: steps.count)
                return
            }
            for (index, step) in steps.enumerated() {
                withAnimation(.snappy) { stage = index }
                try? await Task.sleep(for: .seconds(step.seconds))
                if Task.isCancelled { return }
            }
            withAnimation(.snappy) { stage = steps.count }
            try? await Task.sleep(for: .seconds(0.4))
            model.finishBoot(kind)
        }
    }

    /// The guest's display shows through, dimmed, on every boot but Recovery's.
    private var dimmed: Bool { kind != .recovery }

    @ViewBuilder
    private var backdrop: some View {
        if dimmed {
            Color.black.opacity(0.64).ignoresSafeArea()
        } else {
            Color(.systemGroupedBackground).ignoresSafeArea()
        }
    }

    /// The steps, with the first saying what the disk write is doing while
    /// one is under way.
    private func shownSteps(guest: Guest) -> [Step] {
        var steps = Self.steps(kind, guest: guest, system: model.system, grewFromGiB: model.lifecycle.grewFromGiB)
        if model.preparingDisk {
            let detail = model.diskProgress.map { String(localized: "Preparing \u{00B7} \(Int(($0 * 100).rounded()))%") }
                ?? String(localized: "Preparing")
            steps[0] = Step(title: steps[0].title, detail: detail, seconds: 0)
        }
        #if !APP_STORE
        if model.preparingJit {
            steps[0] = Step(title: steps[0].title, detail: JitStatus.preparing, seconds: 0)
        }
        #endif
        return steps
    }

    /// The device application: the boot is as far as the guest's console
    /// says, and ends when the guest has something to show.
    private func followRuntime(stepCount: Int) async {
        while !Task.isCancelled && !model.bootTracker.isReady {
            let live = min(model.liveBootStage(), stepCount - 1)
            if live != stage { withAnimation(.snappy) { stage = live } }
            try? await Task.sleep(for: .milliseconds(200))
        }
        if Task.isCancelled { return }
        withAnimation(.snappy) { stage = stepCount }
        try? await Task.sleep(for: .seconds(0.4))
        if Task.isCancelled { return }
        model.finishBoot(kind)
    }

    // MARK: Parts

    private func statusLine(frozen: Bool) -> some View {
        HStack(spacing: 8) {
            PulsingDot()
            // Stop was chosen: the line says what is happening now.
            Text(model.slowStart.stopping ? String(localized: "Stopping\u{2026}") : Self.status(kind))
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(Color.accentColor)
            Spacer()
            TimelineView(.periodic(from: startedAt, by: 1)) { timeline in
                // The start's own time, which a restart of the guest in place
                // goes on counting; this screen's, where none is kept.
                let seconds = frozen ? model.frozenElapsed
                    : model.slowStart.began != nil ? model.bootElapsedSeconds()
                    : Int(timeline.date.timeIntervalSince(startedAt))
                Text(Formats.elapsed(seconds))
                    .font(.system(.subheadline, design: .rounded).weight(.medium))
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
                    .contentTransition(.numericText())
            }
        }
        .padding(.horizontal, 24)
        .frame(height: 44)
        .frame(maxWidth: 640)
        .frame(maxWidth: .infinity)
    }

    private func content(guest: Guest, system: SystemInfo, steps: [Step], current: Int) -> some View {
        VStack(alignment: .leading, spacing: 18) {
            Text(kind == .recovery ? "Recovery" : "Andarium")
                .font(.largeTitle.weight(.bold))
                .padding(.horizontal, 4)

            if model.slowStart.showing {
                slowNotice(steps: steps, current: current)
            }

            VStack(spacing: 0) {
                ForEach(Array(steps.enumerated()), id: \.offset) { index, step in
                    if index > 0 { Divider().padding(.leading, 54) }
                    stepRow(step, state: index < current ? .done : (index == current ? .active : .pending))
                }
            }
            .padding(.vertical, 4)
            .modifier(BootPanel(dimmed: dimmed, radius: 26))

            if let note = Self.note(kind, from: model.bootFrom, system: system) {
                Label {
                    Text(note.keepingTogether)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                } icon: {
                    Image(systemName: Self.noteSymbol(kind))
                        .foregroundStyle(Color.accentColor)
                }
                .padding(16)
                .frame(maxWidth: .infinity, alignment: .leading)
                .modifier(BootPanel(dimmed: dimmed, radius: 22))
            }

            #if !APP_STORE
            if let sentence = model.jitLastStart?.fallbackSentence {
                Label {
                    Text(sentence)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                } icon: {
                    Image(systemName: "bolt.slash.fill")
                        .foregroundStyle(Color.accentColor)
                }
                .padding(16)
                .frame(maxWidth: .infinity, alignment: .leading)
                .modifier(BootPanel(dimmed: dimmed, radius: 22))
            }
            #endif
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 20)
        .frame(maxWidth: 520)
        .frame(maxWidth: .infinity)
    }

    // MARK: Slow start

    /// The notice's title. A first start, and the first start after an update,
    /// have no usual to be slower than.
    static func slowTitle(longByNature: Bool) -> String {
        longByNature
            ? String(localized: "Starting is taking a long time")
            : String(localized: "Starting is taking longer than usual")
    }

    /// Spoken at once, not after VoiceOver's own speech.
    private static func announce(_ text: String) {
        var announcement = AttributedString(text)
        announcement.accessibilitySpeechAnnouncementPriority = .high
        AccessibilityNotification.Announcement(announcement).post()
    }

    /// "3 minutes, 12 seconds" in the person's language, for VoiceOver.
    static func spoken(_ seconds: Int) -> String {
        Duration.seconds(seconds).formatted(.units(allowed: [.minutes, .seconds], width: .wide))
    }

    private func slowNotice(steps: [Step], current: Int) -> some View {
        let stepTitle = steps[min(max(current, 0), steps.count - 1)].title
        return VStack(alignment: .leading, spacing: 12) {
            Label {
                Text(Self.slowTitle(longByNature: model.slowStart.longByNature))
                    .font(.headline)
                    .fixedSize(horizontal: false, vertical: true)
            } icon: {
                Image(systemName: "hourglass")
                    .foregroundStyle(Color.accentColor)
            }
            .accessibilityAddTraits(.isHeader)
            Text("The virtual machine is still starting. You can keep waiting, or stop it. To start it again after stopping, close Andarium in the App Switcher and open it again.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            // The time moves with the clock, as the status line's does.
            TimelineView(.periodic(from: startedAt, by: 1)) { _ in
                let seconds = model.frozenBootStage != nil ? model.frozenElapsed : model.bootElapsedSeconds()
                Text(verbatim: "\(stepTitle) \u{00B7} \(Formats.elapsed(seconds))")
                    .font(.subheadline.weight(.semibold))
                    .monospacedDigit()
                    .foregroundStyle(Color.accentColor)
                    .accessibilityLabel(Text(verbatim: "\(stepTitle), \(Self.spoken(seconds))"))
            }
            ViewThatFits(in: .horizontal) {
                slowButtons(lineLimit: 1, axis: .horizontal)
                slowButtons(lineLimit: nil, axis: .vertical)
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .modifier(BootPanel(dimmed: dimmed, radius: 22))
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("boot-slow-notice")
    }

    /// Side by side, at one width (`EqualWidthRow`), only where both labels fit
    /// at their own; stacked, a label wraps. Stop's label is as wide as its
    /// longer state, so that choosing it moves nothing.
    @ViewBuilder
    private func slowButtons(lineLimit: Int?, axis: Axis) -> some View {
        let layout = axis == .horizontal ? AnyLayout(EqualWidthRow(spacing: 12)) : AnyLayout(VStackLayout(spacing: 10))
        layout {
            Button(role: .destructive) {
                model.askToStop()
            } label: {
                ZStack {
                    stopLabel(stopping: true, lineLimit: lineLimit, axis: axis).hidden()
                    stopLabel(stopping: model.slowStart.stopping, lineLimit: lineLimit, axis: axis)
                }
                .frame(maxWidth: .infinity, minHeight: 32)
            }
            .buttonStyle(.bordered)
            .tint(.red)
            .controlSize(.large)
            .disabled(model.slowStart.stopping)
            .accessibilityIdentifier("boot-stop")
            .confirmationDialog(
                "Stop the virtual machine?",
                isPresented: Binding(get: { model.confirmingStop }, set: { model.confirmingStop = $0 }),
                titleVisibility: .visible
            ) {
                Button("Stop", role: .destructive) { model.stopStart() }
                Button("Cancel", role: .cancel) {}
            } message: {
                Text("To start it again, close Andarium in the App Switcher and open it again.")
            }
            #if FEEDBACK_MAIL
            if model.feedbackAddress != nil {
                Button {
                    model.feedbackFromBoot = true
                } label: {
                    Text("Send feedback", tableName: "Feedback")
                        .fontWeight(.semibold)
                        .lineLimit(lineLimit)
                        .multilineTextAlignment(.center)
                        .fixedSize(horizontal: axis == .horizontal, vertical: true)
                        .frame(maxWidth: .infinity, minHeight: 32)
                }
                .buttonStyle(.bordered)
                .controlSize(.large)
                .accessibilityIdentifier("boot-send-feedback")
            }
            #endif
        }
    }

    /// Stop's label in one of its two states: "Stopping\u{2026}" has a spinner.
    @ViewBuilder
    private func stopLabel(stopping: Bool, lineLimit: Int?, axis: Axis) -> some View {
        HStack(spacing: 8) {
            if stopping {
                ProgressView()
                    .controlSize(.small)
                    .accessibilityHidden(true)
            }
            Text(stopping ? "Stopping\u{2026}" : "Stop")
                .fontWeight(.semibold)
                .lineLimit(lineLimit)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: axis == .horizontal, vertical: true)
        }
    }

    private enum StepState { case done, active, pending }

    private func stepRow(_ step: Step, state: StepState) -> some View {
        HStack(alignment: .top, spacing: 14) {
            ZStack {
                switch state {
                case .done:
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(Color.accentColor)
                case .active:
                    ProgressView()
                        .controlSize(.small)
                case .pending:
                    Image(systemName: "circle")
                        .foregroundStyle(.tertiary)
                }
            }
            .font(.title3)
            .frame(width: 24, height: 24)
            VStack(alignment: .leading, spacing: 2) {
                Text(step.title)
                    .font(.body.weight(.semibold))
                    .foregroundStyle(state == .pending ? AnyShapeStyle(.tertiary) : AnyShapeStyle(.primary))
                Text(step.detail.keepingTogether)
                    .font(.subheadline)
                    .monospacedDigit()
                    .foregroundStyle(state == .active ? AnyShapeStyle(Color.accentColor) : (state == .done ? AnyShapeStyle(.secondary) : AnyShapeStyle(.tertiary)))
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }

    // MARK: Content per kind

    static func status(_ kind: BootKind) -> String {
        switch kind {
        case .normal: String(localized: "Starting")
        case .firstStart: String(localized: "First start")
        case .upgrade: String(localized: "Updating")
        case .recovery: String(localized: "Starting recovery")
        case .restart: String(localized: "Restarting")
        }
    }

    /// The symbol of the note a first start or an update shows; no other
    /// boot has one.
    static func noteSymbol(_ kind: BootKind) -> String {
        kind == .firstStart ? "sparkles" : "arrow.triangle.2.circlepath"
    }

    static func note(_ kind: BootKind, from: String?, system: SystemInfo) -> String? {
        switch kind {
        case .firstStart:
            String(localized: "The first-run setup may take a few minutes.")
        case .upgrade:
            if let from {
                String(localized: "Updating from \(ReleaseLabel.revision(of: from)) to \(system.releaseRevision). Apps and files are kept.")
            } else {
                String(localized: "Updating to \(system.releaseRevision). Apps and files are kept.")
            }
        case .normal, .restart, .recovery:
            nil
        }
    }

    /// `grewFromGiB` is the capacity this start grew the storage from
    /// (`Lifecycle.grewFromGiB`): the runtime grows the disk in every kind of
    /// start but the first, which creates it, and the guest's capacity is
    /// already the new one.
    static func steps(_ kind: BootKind, guest: Guest, system info: SystemInfo, grewFromGiB: Int? = nil) -> [Step] {
        let system = Step(
            title: String(localized: "System image"), detail: String(localized: "\(info.releaseRevision) · read-only"), seconds: 0.7)
        let kernelVersion = info.kernel.replacingOccurrences(of: "Linux ", with: "")
        let kernel = Step(
            title: String(localized: "Linux kernel"),
            detail: String(localized: "\(kernelVersion) · \(CountWords.vcpus(guest.cores)) · \(Formats.size(mib: guest.memoryMiB)) RAM"),
            seconds: 0.9)
        let used = MeterWords.storage(Formats.size(bytes: guest.usedBytes), of: Formats.size(gib: guest.capacityGiB))
        let growing = grewFromGiB.map {
            String(localized: "Grows from \(Formats.size(gib: $0)) to \(Formats.size(gib: guest.capacityGiB))")
        }
        let storage = String(localized: "Storage")
        let startup = String(localized: "Startup")
        switch kind {
        case .recovery:
            return [system, kernel,
                    Step(title: storage, detail: growing ?? used, seconds: growing == nil ? 0.6 : 1.2),
                    Step(title: String(localized: "Recovery"), detail: String(localized: "Loads the recovery system"), seconds: 1.4)]
        case .firstStart:
            return [system, kernel,
                    Step(
                        title: storage,
                        detail: String(localized: "Prepares \(Formats.size(gib: guest.effectiveCapacityGiB)) for first use"),
                        seconds: 0.9),
                    Step(title: startup, detail: String(localized: "Sets up for the first time"), seconds: 4.0)]
        case .upgrade:
            return [system, kernel,
                    Step(title: storage, detail: growing ?? used, seconds: growing == nil ? 0.7 : 1.2),
                    Step(title: startup, detail: String(localized: "Updates apps for \(info.releaseRevision)"), seconds: 3.6)]
        case .normal, .restart:
            return [system, kernel,
                    Step(title: storage, detail: growing ?? used, seconds: growing == nil ? 0.6 : 1.2),
                    Step(title: startup, detail: String(localized: "Starts services"), seconds: 1.8)]
        }
    }
}

/// Buttons side by side at one width: the ideal width of the widest label, so
/// that `ViewThatFits` stacks them as soon as either needs more than half of
/// the row, and a label that changes does not move the other.
private struct EqualWidthRow: Layout {
    let spacing: CGFloat

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let ideals = subviews.map { $0.sizeThatFits(.unspecified) }
        let cell = ideals.map(\.width).max() ?? 0
        let ideal = cell * CGFloat(subviews.count) + spacing * CGFloat(max(subviews.count - 1, 0))
        return CGSize(width: proposal.width ?? ideal, height: ideals.map(\.height).max() ?? 0)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        guard !subviews.isEmpty else { return }
        let gaps = spacing * CGFloat(subviews.count - 1)
        let cell = (bounds.width - gaps) / CGFloat(subviews.count)
        for (index, subview) in subviews.enumerated() {
            subview.place(
                at: CGPoint(x: bounds.minX + CGFloat(index) * (cell + spacing), y: bounds.minY),
                proposal: ProposedViewSize(width: cell, height: bounds.height))
        }
    }
}

/// The boot screen's panels: a flat translucent fill over the dimmed
/// display, which needs no backdrop sampling, and glass over Recovery's
/// opaque screen.
private struct BootPanel: ViewModifier {
    let dimmed: Bool
    let radius: CGFloat

    func body(content: Content) -> some View {
        if dimmed {
            content.background(
                Color(.secondarySystemGroupedBackground).opacity(0.78),
                in: .rect(cornerRadius: radius, style: .continuous)
            )
        } else {
            content.glassEffect(.regular, in: .rect(cornerRadius: radius))
        }
    }
}

/// Overall boot progress: a cyan capsule on a quiet track.
struct ProgressBar: View {
    let value: Double
    var height: CGFloat = 8

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Capsule().fill(Color(.systemFill))
                Capsule()
                    .fill(Color.accentColor)
                    .frame(width: max(height, geo.size.width * value))
            }
        }
        .frame(height: height)
        .animation(.snappy, value: value)
        .accessibilityElement()
        .accessibilityLabel("Progress")
        .accessibilityValue("\(Int((value * 100).rounded())) percent")
    }
}

/// The live dot on the status line.
struct PulsingDot: View {
    @State private var dim = false

    var body: some View {
        Circle()
            .fill(Color.accentColor)
            .frame(width: 8, height: 8)
            .opacity(dim ? 0.35 : 1)
            .animation(.easeInOut(duration: 0.9).repeatForever(autoreverses: true), value: dim)
            .onAppear { dim = true }
            .accessibilityHidden(true)
    }
}
