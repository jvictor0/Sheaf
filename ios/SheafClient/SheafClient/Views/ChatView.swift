import SwiftUI

struct ChatView: View {
    @Environment(\.colorScheme) private var colorScheme
    @Environment(\.dismiss) private var dismiss
    @EnvironmentObject private var appState: AppState
    @StateObject private var viewModel: ChatViewModel
    @State private var draft = ""
    @State private var isRequestingOlder = false
    @State private var previousMessageCount = 0
    @State private var previousFirstMessageID: String?
    @State private var hasInitialBottomFocus = false

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

            HStack(alignment: .bottom, spacing: 8) {
                TextField("Message", text: $draft, axis: .vertical)
                    .textFieldStyle(.roundedBorder)
                    .lineLimit(1...6)

                Button("Send") {
                    let sending = draft
                    draft = ""
                    Task {
                        await viewModel.sendMessage(sending)
                    }
                }
                .buttonStyle(.borderedProminent)
                .disabled(draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            }
            .padding()
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
            if let error = viewModel.errorMessage {
                Text(error)
                    .font(.footnote)
                    .padding(8)
                    .background(.thinMaterial)
                    .clipShape(Capsule())
                    .padding(.top, 8)
            }
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
}
