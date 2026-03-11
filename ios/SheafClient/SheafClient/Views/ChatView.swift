import SwiftUI
import AVFoundation

struct ChatView: View {
    @Environment(\.colorScheme) private var colorScheme
    @Environment(\.dismiss) private var dismiss
    @EnvironmentObject private var appState: AppState
    @StateObject private var viewModel: ChatViewModel
    @State private var draft = ""
    @State private var composeSelection = NSRange(location: 0, length: 0)
    @State private var isComposeFocused = false
    @State private var isRequestingOlder = false
    @State private var previousMessageCount = 0
    @State private var previousFirstMessageID: String?
    @State private var hasInitialBottomFocus = false
    @State private var dismissedKeyboardForCurrentDrag = false
    @State private var dictationState: DictationState = .idle
    @State private var dictationErrorMessage: String?
    @State private var dictationSessionID = UUID().uuidString

    private let recorder = AudioSnippetRecorder()
    private var isComposerExpanded: Bool {
        isComposeFocused
            || !draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            || dictationState != .idle
    }

    init(viewModel: ChatViewModel) {
        _viewModel = StateObject(wrappedValue: viewModel)
    }

    var body: some View {
        VStack(spacing: 0) {
            if viewModel.isLoading {
                ProgressView("Loading messages...")
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollViewReader { proxy in
                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: 12) {
                            Color.clear
                                .frame(height: 1)
                                .onAppear {
                                    requestOlderMessagesIfNeeded(proxy: proxy)
                                }

                            if viewModel.isLoadingOlder {
                                HStack {
                                    Spacer()
                                    ProgressView()
                                    Spacer()
                                }
                            }

                            ForEach(viewModel.messages) { message in
                                MessageRow(message: message)
                                    .id(message.id)
                            }
                        }
                        .padding()
                    }
                    .simultaneousGesture(
                        DragGesture(minimumDistance: 6)
                            .onChanged { value in
                                guard !dismissedKeyboardForCurrentDrag else { return }
                                guard value.translation.height < -6 else { return }
                                guard isComposeFocused else { return }
                                isComposeFocused = false
                                dismissedKeyboardForCurrentDrag = true
                            }
                            .onEnded { _ in
                                dismissedKeyboardForCurrentDrag = false
                            }
                    )
                    .onAppear {
                        guard !hasInitialBottomFocus else { return }
                        if let last = viewModel.messages.last {
                            withAnimation(.none) {
                                proxy.scrollTo(last.id, anchor: .bottom)
                            }
                            hasInitialBottomFocus = true
                        }
                        previousMessageCount = viewModel.messages.count
                        previousFirstMessageID = viewModel.messages.first?.id
                    }
                    .onChange(of: viewModel.messages.count) {
                        let currentCount = viewModel.messages.count
                        let currentFirstID = viewModel.messages.first?.id
                        let isInitialLoad = previousMessageCount == 0
                        let didPrepend = !isInitialLoad
                            && currentCount > previousMessageCount
                            && currentFirstID != previousFirstMessageID

                        if !didPrepend, let last = viewModel.messages.last {
                            withAnimation(.easeOut(duration: 0.15)) {
                                proxy.scrollTo(last.id, anchor: .bottom)
                            }
                            hasInitialBottomFocus = true
                        }

                        previousMessageCount = currentCount
                        previousFirstMessageID = currentFirstID
                        viewModel.prefetchMath(for: colorScheme == .dark ? .dark : .light)
                    }
                }
            }

            Divider()

            composerSection
        }
        .navigationTitle("Chat")
        .navigationBarBackButtonHidden(true)
        .toolbar {
            ToolbarItem {
                Button {
                    dismiss()
                } label: {
                    Label("Conversations", systemImage: "chevron.left")
                }
            }
        }
        .task {
            await viewModel.loadInitial()
            appState.selectedChatID = viewModel.currentChatID
        }
        .overlay(alignment: .top) {
            if let error = dictationErrorMessage ?? viewModel.errorMessage {
                Text(error)
                    .font(.footnote)
                    .padding(8)
                    .background(.thinMaterial)
                    .clipShape(Capsule())
                    .padding(.top, 8)
            }
        }
    }

    @ViewBuilder
    private var composerSection: some View {
        if isComposerExpanded {
            expandedComposer
        } else {
            collapsedComposer
        }
    }

    private var expandedComposer: some View {
        HStack(alignment: .bottom, spacing: 8) {
            ZStack {
                RoundedRectangle(cornerRadius: 10)
                    .strokeBorder(Color.secondary.opacity(0.35))
                CursorTextView(
                    text: $draft,
                    selectedRange: $composeSelection,
                    isFocused: $isComposeFocused,
                    placeholder: "Message"
                )
                .frame(minHeight: 36, maxHeight: 140)
            }

            VStack(spacing: 8) {
                dictationButton

                Button("Send") {
                    sendCurrentDraft()
                }
                .buttonStyle(.borderedProminent)
                .disabled(draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || dictationState == .uploading)
            }
        }
        .padding(.horizontal)
        .padding(.vertical, 12)
        .animation(.easeInOut(duration: 0.18), value: isComposerExpanded)
    }

    private var collapsedComposer: some View {
        HStack(spacing: 8) {
            Button {
                isComposeFocused = true
            } label: {
                HStack(spacing: 8) {
                    Image(systemName: "square.and.pencil")
                        .foregroundStyle(.secondary)
                    Text("Message")
                        .foregroundStyle(.secondary)
                    Spacer(minLength: 0)
                }
                .padding(.horizontal, 10)
                .frame(height: 34)
                .background(
                    RoundedRectangle(cornerRadius: 10)
                        .strokeBorder(Color.secondary.opacity(0.35))
                )
            }
            .buttonStyle(.plain)

            Button {
                Task { await handleDictationTap() }
            } label: {
                Image(systemName: "mic")
                    .frame(width: 24, height: 24)
            }
            .buttonStyle(.bordered)
        }
        .padding(.horizontal)
        .padding(.vertical, 6)
        .animation(.easeInOut(duration: 0.18), value: isComposerExpanded)
    }

    private var dictationButton: some View {
        Button {
            Task { await handleDictationTap() }
        } label: {
            Group {
                if dictationState == .uploading {
                    ProgressView()
                } else if dictationState == .recording {
                    Image(systemName: "stop.circle.fill")
                } else {
                    Image(systemName: "mic")
                }
            }
            .frame(width: 30, height: 30)
        }
        .buttonStyle(.bordered)
        .tint(dictationState == .recording ? .red : nil)
        .disabled(dictationState == .uploading)
    }

    private func sendCurrentDraft() {
        let sending = draft
        draft = ""
        composeSelection = NSRange(location: 0, length: 0)
        Task {
            await viewModel.sendMessage(sending)
        }
    }

    private func requestOlderMessagesIfNeeded(proxy: ScrollViewProxy) {
        guard viewModel.canLoadOlder, !isRequestingOlder else { return }
        let anchorID = viewModel.messages.first?.id
        isRequestingOlder = true

        Task {
            let didPrepend = await viewModel.loadOlder()
            if didPrepend, let anchorID {
                withAnimation(.none) {
                    proxy.scrollTo(anchorID, anchor: .top)
                }
            }
            isRequestingOlder = false
        }
    }

    private func handleDictationTap() async {
        switch dictationState {
        case .idle:
            await startDictation()
        case .recording:
            await finishDictationAndInsert()
        case .uploading:
            return
        }
    }

    private func startDictation() async {
        let granted = await requestMicrophonePermission()
        guard granted else {
            dictationErrorMessage = "Microphone access is required for dictation."
            return
        }

        do {
            try recorder.start()
            dictationErrorMessage = nil
            dictationState = .recording
            isComposeFocused = true
        } catch {
            dictationErrorMessage = "Mic start failed: \(error.localizedDescription)"
            dictationState = .idle
        }
    }

    private func finishDictationAndInsert() async {
        dictationState = .uploading
        let wavData: Data
        do {
            wavData = try recorder.stopAndBuildWAV()
        } catch {
            dictationErrorMessage = "Mic stop failed: \(error.localizedDescription)"
            dictationState = .idle
            return
        }

        do {
            let response = try await DictationAPIClient.shared.dictateAudio(
                wavData: wavData,
                sampleRate: 16_000,
                locale: "en-US",
                sessionID: dictationSessionID
            )
            guard let insertion = dictationInsertionText(from: response) else {
                dictationErrorMessage = "Server returned empty revised text."
                dictationState = .idle
                return
            }
            insertTextAtSelection(insertion, text: &draft, selection: &composeSelection)
            dictationErrorMessage = nil
            dictationState = .idle
            isComposeFocused = true
        } catch {
            dictationErrorMessage = dictationRequestErrorMessage(error)
            dictationState = .idle
        }
    }

    private func requestMicrophonePermission() async -> Bool {
        switch AVAudioApplication.shared.recordPermission {
        case .granted:
            return true
        case .denied:
            return false
        case .undetermined:
            return await withCheckedContinuation { continuation in
                AVAudioApplication.requestRecordPermission { granted in
                    continuation.resume(returning: granted)
                }
            }
        @unknown default:
            return false
        }
    }

    private func dictationRequestErrorMessage(_ error: Error) -> String {
        if let urlError = error as? URLError {
            switch urlError.code {
            case .notConnectedToInternet:
                return "Cannot reach Dictator server. Check Local Network access and Wi-Fi."
            case .cannotConnectToHost:
                return "Cannot connect to Dictator server. Verify dictation_base_url host and port."
            case .timedOut:
                return "Dictation request timed out. Confirm Dictator server is running."
            default:
                return "Dictation request failed: \(urlError.localizedDescription)"
            }
        }
        return error.localizedDescription
    }
}

private enum DictationState {
    case idle
    case recording
    case uploading
}
