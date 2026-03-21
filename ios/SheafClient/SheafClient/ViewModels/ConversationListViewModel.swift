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
            async let loadedChats = api.listChats()
            async let _ = ClientSettingsStore.shared.refreshAvailableModels(client: api)
            chats = try await loadedChats.sorted { ($0.updatedAt ?? .distantPast) > ($1.updatedAt ?? .distantPast) }
            state = .loaded
        } catch {
            state = .error(error.localizedDescription)
        }
    }

    func createChatAndOpen(name: String? = nil) async throws -> String {
        let normalizedName = name?.trimmingCharacters(in: .whitespacesAndNewlines)
        let chatID = try await api.createChat(name: normalizedName?.isEmpty == false ? normalizedName : nil)
        await loadChats()
        return chatID
    }
}
