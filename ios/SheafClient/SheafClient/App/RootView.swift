import SwiftUI

struct RootView: View {
    @EnvironmentObject private var appState: AppState

    var body: some View {
        NavigationStack(path: Binding(get: { appState.path }, set: { appState.path = $0 })) {
            ConversationListView(viewModel: ConversationListViewModel())
                .navigationDestination(for: AppRoute.self) { route in
                    switch route {
                    case .conversationList:
                        ConversationListView(viewModel: ConversationListViewModel())
                    case .chat(let chatID):
                        ChatView(viewModel: ChatViewModel(chatID: chatID, sessionStore: appState.chatSessionStore))
                    case .settings:
                        SettingsView()
                    }
                }
        }
    }
}
