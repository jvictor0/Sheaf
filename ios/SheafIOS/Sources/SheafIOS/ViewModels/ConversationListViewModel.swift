import Foundation

@MainActor
final class ConversationListViewModel: ObservableObject {
    enum State: Equatable {
        case loading
        case loaded
        case error(String)
    }

    @Published private(set) var chats: [ChatSummary] = []
    @Published private(set) var state: State = .loading

    private let api: SheafAPIClient

    init(api: SheafAPIClient = .shared) {
        self.api = api
    }

    func loadChats() async {
        state = .loading
        do {
            chats = try await api.listChats().sorted { ($0.updatedAt ?? .distantPast) > ($1.updatedAt ?? .distantPast) }
            state = .loaded
        } catch {
            state = .error(error.localizedDescription)
        }
    }

    func createChatAndOpen() async throws -> String {
        let chatID = try await api.createChat()
        await loadChats()
        return chatID
    }
}
