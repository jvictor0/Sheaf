import Foundation

@MainActor
final class ChatSessionStore {
    struct Session {
        var messages: [RenderedMessage]
        var metadata: ChatMetadata?
        var oldestLoadedIndex: Int
        var newestLoadedExclusiveIndex: Int
        var hasMoreOlder: Bool
    }

    private var sessions: [String: Session] = [:]

    func session(for chatID: String) -> Session? {
        sessions[chatID]
    }

    func save(_ session: Session, for chatID: String) {
        sessions[chatID] = session
    }

    func removeAll(except chatID: String) {
        sessions = sessions.filter { $0.key == chatID }
    }
}
