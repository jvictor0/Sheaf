import SwiftUI

struct ConversationListView: View {
    @EnvironmentObject private var appState: AppState
    @StateObject private var viewModel: ConversationListViewModel

    init(viewModel: ConversationListViewModel) {
        _viewModel = StateObject(wrappedValue: viewModel)
    }

    var body: some View {
        Group {
            switch viewModel.state {
            case .loading:
                ProgressView("Loading conversations...")
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            case .error(let message):
                VStack(spacing: 12) {
                    Text("Failed to load conversations")
                        .font(.headline)
                    Text(message)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                    Button("Retry") {
                        Task { await viewModel.loadChats() }
                    }
                    .buttonStyle(.borderedProminent)
                }
                .padding()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            case .loaded:
                listContent
            }
        }
        .navigationTitle("Conversations")
        .toolbar {
            ToolbarItem {
                Button {
                    Task {
                        do {
                            let id = try await viewModel.createChatAndOpen()
                            appState.openChat(id)
                        } catch {
                            // Load errors are already reflected by the next list refresh.
                        }
                    }
                } label: {
                    Label("New", systemImage: "plus")
                }
            }
        }
        .task {
            await viewModel.loadChats()
        }
    }

    @ViewBuilder
    private var listContent: some View {
        if viewModel.chats.isEmpty {
            VStack(spacing: 12) {
                Text("No conversations yet")
                    .font(.headline)
                Button("New Chat") {
                    Task {
                        do {
                            let id = try await viewModel.createChatAndOpen()
                            appState.openChat(id)
                        } catch {
                            // covered by load state
                        }
                    }
                }
                .buttonStyle(.borderedProminent)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            List(viewModel.chats) { chat in
                Button {
                    appState.openChat(chat.chatID)
                } label: {
                    ConversationRow(chat: chat, isSelected: appState.selectedChatID == chat.chatID)
                }
                .buttonStyle(.plain)
            }
            .refreshable {
                await viewModel.loadChats()
            }
        }
    }
}

private struct ConversationRow: View {
    let chat: ChatSummary
    let isSelected: Bool

    var body: some View {
        HStack {
            VStack(alignment: .leading, spacing: 4) {
                Text(chat.chatID)
                    .font(.body.monospaced())
                    .lineLimit(1)
                if let updated = chat.updatedAt {
                    Text(updated.formatted(date: .abbreviated, time: .shortened))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            Spacer()
            if isSelected {
                Image(systemName: "checkmark.circle.fill")
                    .foregroundStyle(.blue)
            }
        }
    }
}
