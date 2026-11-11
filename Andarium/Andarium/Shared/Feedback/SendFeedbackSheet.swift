// SPDX-License-Identifier: GPL-2.0-or-later

#if FEEDBACK_MAIL

import AndariumCore
import PhotosUI
import SwiftUI

/// Send feedback, laid out as the mail it becomes: the problem is the subject
/// line, the details are the body, the files sit in the body as Mail shows
/// them, and the basic information closes it. One bar at the foot holds the
/// bug report, the screenshot, the photos and videos, and Send feedback; it
/// rides above the keyboard. A card over the bar asks for a bug report until
/// one is attached or the card is put away.
struct SendFeedbackSheet: View {
    @State private var model: SendFeedbackModel
    @State private var picking = false
    @State private var picked: [PhotosPickerItem] = []
    @FocusState private var focus: Field?
    @Environment(\.dismiss) private var dismiss
    @Environment(\.dynamicTypeSize) private var typeSize
    @Environment(\.scenePhase) private var scenePhase

    private enum Field { case problem, details }
    private static let endID = "feedback-end"

    init(app: AppModel) {
        _model = State(initialValue: SendFeedbackModel(app: app))
    }

    private var state: SendFeedbackForm { model.state }

    var body: some View {
        @Bindable var model = model
        NavigationStack {
            ScrollViewReader { proxy in
            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    if !state.mailAvailable {
                        mailUnavailable
                            .padding(.horizontal, 16)
                            .padding(.bottom, 12)
                    }
                    Group {
                        subject
                        Divider().padding(.leading, 20)
                        bodyText
                        // At accessibility sizes the card would cover the
                        // mail; it scrolls with it instead.
                        if showsCard && typeSize.isAccessibilitySize {
                            card.padding(.horizontal, 16).padding(.bottom, 14)
                        }
                        if !state.attachments.isEmpty { attachments }
                        information
                    }
                    .disabled(!state.mailAvailable)
                    .opacity(state.mailAvailable ? 1 : 0.45)
                    Color.clear.frame(height: 1).id(Self.endID)
                }
                .frame(maxWidth: 640, alignment: .leading)
                .padding(.top, 4)
                .padding(.bottom, 24)
                .frame(maxWidth: .infinity)
            }
            .scrollDismissesKeyboard(.interactively)
            // The block that opens is the last thing in the mail: bring it into view.
            .onChange(of: model.showsInformation) { _, shown in
                guard shown else { return }
                withAnimation(.snappy) { proxy.scrollTo(Self.endID, anchor: .bottom) }
            }
            }
            .background(Color(.systemBackground))
            .safeAreaBar(edge: .bottom) { bar }
            .navigationTitle("Send feedback")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel", systemImage: "xmark", role: .cancel, action: cancel)
                        .accessibilityIdentifier("feedback-cancel")
                }
            }
        }
        .overlay {
            if let busy = model.app.feedbackBusy {
                BusyOverlay(state: busy)
            } else if model.sent {
                BusyOverlay(state: BusyState(
                    title: "Feedback sent", detail: "Thank you for helping make Andarium better.",
                    result: BusyResult(failed: false, dismiss: BusyCancel { dismiss() })))
            }
        }
        .animation(.easeInOut(duration: 0.2), value: model.app.feedbackBusy != nil)
        .animation(.easeInOut(duration: 0.2), value: model.sent)
        // Once something is typed or attached, a stray swipe loses nothing.
        .interactiveDismissDisabled(state.hasContent || state.makingBugReport)
        .confirmationDialog(
            "Discard this feedback?", isPresented: $model.confirmingDiscard, titleVisibility: .visible
        ) {
            Button("Discard", role: .destructive) { dismiss() }
            Button("Keep editing", role: .cancel) {}
        }
        .alert(
            model.failure?.title ?? "",
            isPresented: Binding(get: { model.failure != nil }, set: { if !$0 { model.failure = nil } }),
            presenting: model.failure
        ) { _ in
            Button("OK", role: .cancel) {}
        } message: { failure in
            Text(failure.message)
        }
        .photosPicker(
            isPresented: $picking, selection: $picked, matching: .any(of: [.images, .videos]),
            preferredItemEncoding: .current
        )
        .onChange(of: picked) { _, items in
            guard !items.isEmpty else { return }
            picked = []
            Task { await importPicked(items) }
        }
        // An account added in iOS Settings brings the sheet to life.
        .onChange(of: scenePhase) { _, phase in
            if phase == .active { model.refresh() }
        }
        .task(id: model.sent) {
            guard model.sent else { return }
            try? await Task.sleep(for: .seconds(1.8))
            dismiss()
        }
        .task { await model.app.feedbackServices.prepare(model) }
        .onDisappear { model.close() }
        .presentationSizing(.page)
    }

    private func cancel() {
        if state.hasContent {
            model.confirmingDiscard = true
        } else {
            dismiss()
        }
    }

    private func importPicked(_ items: [PhotosPickerItem]) async {
        var files: [SendFeedbackPickedFile] = []
        for item in items {
            if let file = try? await item.loadTransferable(type: SendFeedbackPickedFile.self) { files.append(file) }
        }
        await model.importMedia(files)
    }

    // MARK: Mail not available

    private var mailUnavailable: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.title3)
                .foregroundStyle(.orange)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text("Mail can\u{2019}t send from this \(model.app.host.name)")
                    .font(.subheadline.weight(.semibold))
                Text("Feedback is sent with the Mail app. Add a mail account in Settings \u{203A} Apps \u{203A} Mail \u{203A} Mail Accounts, then come back.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
        }
        .padding(14)
        .background(Color.orange.opacity(0.12), in: .rect(cornerRadius: 18, style: .continuous))
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("feedback-mail-unavailable")
    }

    // MARK: The mail

    private var subject: some View {
        @Bindable var model = model
        return HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text("Problem")
                .foregroundStyle(.secondary)
                .accessibilityHidden(true)
            TextField("Problem", text: $model.form.problem, prompt: Text("What went wrong?"))
                .fontWeight(.semibold)
                .focused($focus, equals: .problem)
                .submitLabel(.next)
                .onSubmit { focus = .details }
                .accessibilityIdentifier("feedback-problem")
        }
        .padding(.horizontal, 20)
        .frame(minHeight: 50)
    }

    private var bodyText: some View {
        @Bindable var model = model
        return TextField(
            "Details", text: $model.form.details,
            prompt: Text("What did you do, and what happened?"), axis: .vertical
        )
        .lineLimit(6...)
        .focused($focus, equals: .details)
        .padding(.horizontal, 20)
        .padding(.vertical, 14)
        .accessibilityIdentifier("feedback-details")
    }

    private var attachments: some View {
        VStack(alignment: .leading, spacing: 8) {
            ForEach(state.attachments.items) { attachment in
                row(attachment)
            }
            let total = Formats.size(bytes: state.attachments.totalBytes)
            let count = String(AttributedString(localized: "^[\(state.attachments.count) attachment](inflect: true)").characters)
            HStack(alignment: .firstTextBaseline, spacing: 6) {
                if state.attachments.exceedsWarningSize {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .accessibilityHidden(true)
                    Text("\(count) \u{00B7} \(total). Some mail services refuse mail over 20 MiB.")
                } else {
                    Text("\(count) \u{00B7} \(total)")
                }
            }
            .font(.footnote)
            .foregroundStyle(state.attachments.exceedsWarningSize ? Color.orange : Color.secondary)
            .padding(.leading, 4)
            .accessibilityIdentifier("feedback-total")
        }
        .padding(.horizontal, 16)
        .padding(.bottom, 14)
    }

    private func row(_ attachment: SendFeedbackAttachment) -> some View {
        HStack(spacing: 12) {
            picture(attachment)
                .frame(width: 44, height: 44)
                .clipShape(.rect(cornerRadius: 9, style: .continuous))
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 0) {
                Text(Self.title(attachment.kind))
                    .font(.subheadline.weight(.semibold))
                Text(Self.detail(attachment))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
            }
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(Self.spoken(attachment))
            Spacer(minLength: 8)
            Button { withAnimation(.snappy) { model.remove(attachment) } } label: {
                Image(systemName: "xmark")
                    .font(.footnote.weight(.bold))
                    .foregroundStyle(.secondary)
                    .frame(width: 44, height: 44)
                    .contentShape(.rect)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Self.removeLabel(attachment.kind))
        }
        .padding(.leading, 8)
        .background(Color(.secondarySystemBackground), in: .rect(cornerRadius: 14, style: .continuous))
        .accessibilityIdentifier("feedback-attachment")
    }

    @ViewBuilder
    private func picture(_ attachment: SendFeedbackAttachment) -> some View {
        if let image = model.thumbnails[attachment.id] {
            ZStack {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
                if attachment.kind == .video {
                    Image(systemName: "play.fill")
                        .font(.footnote)
                        .foregroundStyle(.white)
                        .shadow(color: .black.opacity(0.4), radius: 2)
                }
            }
        } else {
            ZStack {
                Rectangle().fill(Color.accentColor.gradient)
                Image(systemName: Self.symbol(attachment.kind))
                    .font(.title3.weight(.semibold))
                    .foregroundStyle(.white)
            }
        }
    }

    private static func symbol(_ kind: SendFeedbackAttachment.Kind) -> String {
        switch kind {
        case .bugReport: "ladybug.fill"
        case .screenshot: "camera.viewfinder"
        case .photo: "photo"
        case .video: "video.fill"
        }
    }

    private static func title(_ kind: SendFeedbackAttachment.Kind) -> LocalizedStringKey {
        switch kind {
        case .bugReport: "Bug report"
        case .screenshot: "Screenshot"
        case .photo: "Photo"
        case .video: "Video"
        }
    }

    private static func removeLabel(_ kind: SendFeedbackAttachment.Kind) -> LocalizedStringKey {
        switch kind {
        case .bugReport: "Remove bug report"
        case .screenshot: "Remove screenshot"
        case .photo: "Remove photo"
        case .video: "Remove video"
        }
    }

    /// The line under the title: the picture's pixels or a video's length,
    /// then the size.
    private static func detail(_ attachment: SendFeedbackAttachment) -> String {
        var parts: [String] = []
        if let pixels = attachment.pixels { parts.append("\(pixels.width) \u{00D7} \(pixels.height)") }
        if let seconds = attachment.durationSeconds { parts.append(Formats.elapsed(seconds)) }
        parts.append(attachment.size)
        return parts.joined(separator: " \u{00B7} ")
    }

    /// What VoiceOver reads for a row: its kind, a video's length and its size.
    private static func spoken(_ attachment: SendFeedbackAttachment) -> String {
        let kind = switch attachment.kind {
        case .bugReport: String(localized: "Bug report")
        case .screenshot: String(localized: "Screenshot")
        case .photo: String(localized: "Photo")
        case .video: String(localized: "Video")
        }
        if let seconds = attachment.durationSeconds {
            return "\(kind), \(seconds) \(String(localized: "seconds")), \(attachment.size)"
        }
        return "\(kind), \(attachment.size)"
    }

    /// The block as it ends the mail, folded to its first lines.
    private var information: some View {
        @Bindable var model = model
        return VStack(alignment: .leading, spacing: 4) {
            Text("\u{2014}")
                .foregroundStyle(.tertiary)
                .accessibilityHidden(true)
            Text(model.information.text)
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)
                .lineLimit(model.showsInformation ? nil : 2)
            Button(model.showsInformation ? "Hide" : "Show") {
                withAnimation(.snappy) { model.showsInformation.toggle() }
            }
            .font(.caption.weight(.semibold))
            .accessibilityLabel(model.showsInformation ? "Hide app and device details" : "Show app and device details")
        }
        .padding(.horizontal, 20)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("feedback-information")
    }

    // MARK: The bar

    private var showsCard: Bool {
        state.mailAvailable && state.attachments.bugReport == nil && !model.hintDismissed && focus == nil
            && !state.makingBugReport
    }

    private var bar: some View {
        VStack(spacing: 10) {
            if showsCard && !typeSize.isAccessibilitySize {
                card.transition(.move(edge: .bottom).combined(with: .opacity))
            }
            ViewThatFits(in: .horizontal) {
                barContent(send: .words)
                barContent(send: .short)
                barContent(send: .icon)
            }
            .padding(.leading, 8)
            .padding(.trailing, 6)
            .padding(.vertical, 6)
            .glassEffect(.regular, in: .capsule)
            .disabled(!state.mailAvailable)
            .dynamicTypeSize(...DynamicTypeSize.xxxLarge)
        }
        .frame(maxWidth: 560)
        .padding(.horizontal, 12)
        .padding(.bottom, 6)
        .frame(maxWidth: .infinity)
        .animation(.snappy, value: showsCard)
    }

    private enum SendStyle { case words, short, icon }

    private func barContent(send style: SendStyle) -> some View {
        HStack(spacing: 2) {
            if !state.running {
                // From Info neither can be made: dimmed, they bring the card
                // with the way to them back.
                let showWay = { withAnimation(.snappy) { model.hintDismissed = false } }
                source(symbol: "ladybug.fill", title: "Bug report", enabled: false, id: "feedback-bug-report",
                       hint: "Only while the virtual machine is running", action: showWay)
                source(symbol: "camera.viewfinder", title: "Screenshot", enabled: false, id: "feedback-screenshot",
                       hint: "Only while the virtual machine is running", action: showWay)
            } else if state.attachments.bugReport != nil {
                // One report at most: attached, the button says so and does
                // nothing more; its row in the body removes it.
                source(symbol: "checkmark.circle.fill", title: "Bug report", enabled: true,
                       id: "feedback-bug-report", value: "Attached", action: {})
                    .allowsHitTesting(false)
                source(symbol: "camera.viewfinder", title: "Screenshot", enabled: state.canTakeScreenshot,
                       id: "feedback-screenshot", action: model.takeScreenshot)
            } else {
                source(symbol: "ladybug.fill", title: "Bug report", enabled: state.canMakeBugReport,
                       id: "feedback-bug-report", action: model.createBugReport)
                source(symbol: "camera.viewfinder", title: "Screenshot", enabled: state.canTakeScreenshot,
                       id: "feedback-screenshot", action: model.takeScreenshot)
            }
            source(symbol: "photo.on.rectangle.angled", title: "Photos", enabled: state.canAddMedia,
                   id: "feedback-photos", spoken: "Add photos or videos") { picking = true }
            Spacer(minLength: 8)
            sendButton(style)
        }
    }

    private func source(
        symbol: String, title: LocalizedStringKey, enabled: Bool, id: String, hint: LocalizedStringKey? = nil,
        value: LocalizedStringKey? = nil, spoken: LocalizedStringKey? = nil, action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            VStack(spacing: 3) {
                Image(systemName: symbol)
                    .font(.system(size: 19, weight: .semibold))
                    .frame(height: 22)
                Text(title)
                    .font(.caption2.weight(.medium))
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
            }
            .foregroundStyle(enabled && state.mailAvailable ? Color.accentColor : Color(.tertiaryLabel))
            .padding(.horizontal, 4)
            .frame(minWidth: 58, minHeight: 48)
            .contentShape(.capsule)
        }
        .buttonStyle(.plain)
        .accessibilityLabel(spoken ?? title)
        .accessibilityValue(value ?? "")
        .accessibilityHint(hint ?? "")
        .accessibilityIdentifier(id)
        .accessibilityShowsLargeContentViewer()
    }

    /// The control that hands the mail to Mail. Its words shorten when the
    /// bar cannot hold them; its accessibility name never does.
    private func sendButton(_ style: SendStyle) -> some View {
        Button(action: model.send) {
            Group {
                switch style {
                case .words: Label("Send feedback", systemImage: "arrow.up")
                case .short: Label("Send", systemImage: "arrow.up")
                case .icon: Image(systemName: "arrow.up")
                }
            }
            .font(.subheadline.weight(.bold))
            .foregroundStyle(.white)
            .lineLimit(1)
            .padding(.horizontal, style == .icon ? 0 : 16)
            .frame(minWidth: 48, minHeight: 48)
            .background(Capsule().fill(state.canSend ? Color.accentColor : Color(.systemGray3)))
            .contentShape(.capsule)
        }
        .buttonStyle(.plain)
        .glassEffect(.regular.interactive(), in: .capsule)
        .disabled(!state.canSend)
        .accessibilityLabel("Send feedback")
        .accessibilityIdentifier("feedback-send")
        .fixedSize()
    }

    /// The request for a bug report, over the bar; from Info, where no virtual
    /// machine runs, the way to one.
    private var card: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: "ladybug.fill")
                .font(.title3)
                .foregroundStyle(Color.accentColor)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 8) {
                if state.running {
                    Text("For any bug, attach a bug report. It tells us what the virtual machine and the app were doing.")
                        .font(.footnote)
                        .fixedSize(horizontal: false, vertical: true)
                    Button("Create bug report", action: model.createBugReport)
                        .buttonStyle(.glassProminent)
                        .controlSize(.small)
                        .disabled(!state.canMakeBugReport)
                        .accessibilityIdentifier("feedback-create-bug-report")
                } else {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Reporting a bug?")
                            .font(.subheadline.weight(.semibold))
                        Text("A bug report and a screenshot need the virtual machine running.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    .accessibilityElement(children: .combine)
                    SendFeedbackSteps(font: .footnote)
                }
            }
            Spacer(minLength: 0)
            Button { withAnimation(.snappy) { model.hintDismissed = true } } label: {
                Image(systemName: "xmark")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(.secondary)
                    .frame(width: 28, height: 28)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Dismiss")
        }
        .padding(14)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
        .accessibilityIdentifier("feedback-card")
    }
}

/// The way to a bug report from where no virtual machine runs (Info): start it,
/// make the problem happen again, and open Send feedback from the session's
/// Settings. VoiceOver reads them as one, numbers included.
struct SendFeedbackSteps: View {
    @ScaledMetric(relativeTo: .subheadline) private var badge: CGFloat = 20
    var font: Font = .subheadline

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            step(1, "Start the virtual machine.")
            step(2, "Make the problem happen again.")
            step(3, "Tap the side keys, then Settings \u{203A} Send feedback.")
        }
        .accessibilityElement(children: .combine)
    }

    private func step(_ number: Int, _ words: LocalizedStringKey) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Text("\(number)")
                .font(.caption.weight(.bold))
                .monospacedDigit()
                .foregroundStyle(.white)
                .frame(width: badge, height: badge)
                .background(Color.accentColor, in: .circle)
            Text(words)
                .font(font)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}

#endif
