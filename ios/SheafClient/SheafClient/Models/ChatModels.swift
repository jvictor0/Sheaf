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
    let name: String
    let createdAt: Date?
    let updatedAt: Date?

    var id: String { chatID }

    enum CodingKeys: String, CodingKey {
        case chatID = "chat_id"
        case threadID = "thread_id"
        case id
        case name
        case createdAt = "created_at"
        case updatedAt = "updated_at"
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        if let chatID = try container.decodeIfPresent(String.self, forKey: .chatID) {
            self.chatID = chatID
        } else if let threadID = try container.decodeIfPresent(String.self, forKey: .threadID) {
            self.chatID = threadID
        } else {
            self.chatID = try container.decode(String.self, forKey: .id)
        }
        self.name = try container.decodeIfPresent(String.self, forKey: .name) ?? self.chatID
        self.createdAt = try container.decodeIfPresent(Date.self, forKey: .createdAt)
        self.updatedAt = try container.decodeIfPresent(Date.self, forKey: .updatedAt)
    }
}

struct ChatListResponse: Decodable {
    let chats: [ChatSummary]

    enum CodingKeys: String, CodingKey {
        case chats
        case threads
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        chats = try container.decodeIfPresent([ChatSummary].self, forKey: .threads)
            ?? container.decodeIfPresent([ChatSummary].self, forKey: .chats)
            ?? []
    }
}

struct ModelListResponse: Decodable {
    let models: [ClientModel]
}

struct CreateChatResponse: Decodable {
    let chatID: String

    enum CodingKeys: String, CodingKey {
        case chatID = "chat_id"
        case threadID = "thread_id"
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        chatID = try container.decodeIfPresent(String.self, forKey: .threadID)
            ?? container.decode(String.self, forKey: .chatID)
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

struct EnterChatRequest: Encodable {
    let protocolVersion: Int
    let knownTailTurnID: String?

    enum CodingKeys: String, CodingKey {
        case protocolVersion = "protocol_version"
        case knownTailTurnID = "known_tail_turn_id"
    }
}

struct EnterChatResponse: Decodable {
    let sessionID: String
    let websocketURL: String
    let acceptedProtocolVersion: Int

    enum CodingKeys: String, CodingKey {
        case sessionID = "session_id"
        case websocketURL = "websocket_url"
        case acceptedProtocolVersion = "accepted_protocol_version"
    }
}

struct CommittedTurn: Decodable, Hashable {
    let id: String
    let threadID: String
    let prevTurnID: String?
    let speaker: String
    let messageText: String
    let modelName: String?
    let createdAt: String?
    let toolCalls: [ToolCallPayload]

    enum CodingKeys: String, CodingKey {
        case id
        case threadID = "thread_id"
        case prevTurnID = "prev_turn_id"
        case speaker
        case messageText = "message_text"
        case modelName = "model_name"
        case createdAt = "created_at"
        case toolCalls = "tool_calls"
    }
}

struct PendingSend: Hashable {
    let clientMessageID: String
    let text: String
    let responseToTurnID: String?
    let localMessageID: String
}

struct StreamingAssistantTurn: Hashable {
    let queueID: Int
    var text: String
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
