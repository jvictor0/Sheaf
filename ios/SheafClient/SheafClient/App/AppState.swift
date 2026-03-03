import Foundation

@MainActor
final class AppState: ObservableObject {
    @Published var selectedChatID: String?
    @Published var path: [AppRoute] = []
    let chatSessionStore = ChatSessionStore()

    func openChat(_ chatID: String) {
        if selectedChatID != chatID {
            chatSessionStore.removeAll(except: chatID)
        }
        selectedChatID = chatID
        path.append(.chat(chatID: chatID))
    }
}
