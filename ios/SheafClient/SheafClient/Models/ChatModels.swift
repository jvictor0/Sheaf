import Foundation

enum JSONValue: Decodable, Hashable {
    case string(String)
    case number(Double)
    case bool(Bool)
    case object([String: JSONValue])
    case array([JSONValue])
    case null

    var stringValue: String {
        switch self {
        case .string(let value):
            return value
        case .number(let value):
            if floor(value) == value {
                return String(Int(value))
            }
            return String(value)
        case .bool(let value):
            return value ? "true" : "false"
        case .object(let object):
            let pairs = object.sorted { $0.key < $1.key }.map { "\($0.key): \($0.value.stringValue)" }
            return "{\(pairs.joined(separator: ", "))}"
        case .array(let values):
            return "[\(values.map(\.stringValue).joined(separator: ", "))]"
        case .null:
            return "null"
        }
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() {
            self = .null
            return
        }
        if let value = try? container.decode(Bool.self) {
            self = .bool(value)
            return
        }
        if let value = try? container.decode(Double.self) {
            self = .number(value)
            return
        }
        if let value = try? container.decode(String.self) {
            self = .string(value)
            return
        }
        if let value = try? container.decode([String: JSONValue].self) {
            self = .object(value)
            return
        }
        if let value = try? container.decode([JSONValue].self) {
            self = .array(value)
            return
        }
        throw DecodingError.typeMismatch(
            JSONValue.self,
            .init(codingPath: decoder.codingPath, debugDescription: "Unsupported JSON value")
        )
    }
}

struct ToolCallPayload: Decodable, Hashable {
    let id: String
    let name: String
    let args: [String: JSONValue]
    let result: String
    let isError: Bool

    enum CodingKeys: String, CodingKey {
        case id
        case name
        case args
        case result
        case isError = "is_error"
    }
}

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

struct CreateChatRequest: Encodable {
    let name: String
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
    let toolCalls: [ToolCallPayload]
    let localID: UUID = UUID()

    var id: String {
        if let index {
            return "server-\(index)"
        }
        return "local-\(localID.uuidString)"
    }

    enum CodingKeys: String, CodingKey {
        case index
        case role
        case content
        case toolCalls = "tool_calls"
    }

    init(index: Int?, role: String, content: String, toolCalls: [ToolCallPayload] = []) {
        self.index = index
        self.role = role
        self.content = content
        self.toolCalls = toolCalls
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        index = try container.decodeIfPresent(Int.self, forKey: .index)
        role = try container.decode(String.self, forKey: .role)
        content = try container.decode(String.self, forKey: .content)
        toolCalls = try container.decodeIfPresent([ToolCallPayload].self, forKey: .toolCalls) ?? []
    }
}

struct SendMessageRequest: Encodable {
    let message: String
    let model: String
}

struct SendMessageResponse: Decodable {
    let chatID: String
    let response: String
    let checkpointID: String?
    let toolCalls: [ToolCallPayload]

    enum CodingKeys: String, CodingKey {
        case chatID = "chat_id"
        case response
        case checkpointID = "checkpoint_id"
        case toolCalls = "tool_calls"
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        chatID = try container.decode(String.self, forKey: .chatID)
        response = try container.decode(String.self, forKey: .response)
        checkpointID = try container.decodeIfPresent(String.self, forKey: .checkpointID)
        toolCalls = try container.decodeIfPresent([ToolCallPayload].self, forKey: .toolCalls) ?? []
    }
}
