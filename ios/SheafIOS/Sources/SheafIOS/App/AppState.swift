import Foundation

@MainActor
final class AppState: ObservableObject {
    @Published var selectedChatID: String?
    @Published var path: [AppRoute] = []

    func openChat(_ chatID: String) {
        selectedChatID = chatID
        path.append(.chat(chatID: chatID))
    }
}
