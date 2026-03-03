import Foundation

struct ChatSummary: Decodable, Identifiable, Hashable {
    let chatID: String
    let createdAt: Date?
    let updatedAt: Date?

    var id: String { chatID }

    enum CodingKeys: String, CodingKey {
        case chatID = "chat_id"
        case createdAt = "created_at"
        case updatedAt = "updated_at"
    }
}

struct ChatListResponse: Decodable {
    let chats: [ChatSummary]
}

struct CreateChatResponse: Decodable {
    let chatID: String

    enum CodingKeys: String, CodingKey {
        case chatID = "chat_id"
    }
}

struct ChatMetadata: Decodable {
    let chatID: String
    let messageCount: Int
    let latestCheckpointID: String?

    enum CodingKeys: String, CodingKey {
        case chatID = "chat_id"
        case messageCount = "message_count"
        case latestCheckpointID = "latest_checkpoint_id"
    }
}

struct MessageEnvelope: Decodable {
    let messages: [ChatMessage]
}

struct ChatMessage: Decodable, Identifiable, Hashable {
    let index: Int?
    let role: String
    let content: String

    var id: String { "\(index ?? -1)-\(role)-\(content.hashValue)" }
}

struct SendMessageRequest: Encodable {
    let message: String
}

struct SendMessageResponse: Decodable {
    let chatID: String
    let response: String
    let checkpointID: String?

    enum CodingKeys: String, CodingKey {
        case chatID = "chat_id"
        case response
        case checkpointID = "checkpoint_id"
    }
}
