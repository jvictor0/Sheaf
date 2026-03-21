import Foundation

actor SheafAPIClient {
    static let shared = SheafAPIClient()

    private let baseURL: URL
    private let session: URLSession
    private let decoder: JSONDecoder
    private let encoder: JSONEncoder

    init(baseURL: URL? = nil, session: URLSession? = nil) {
        if let baseURL {
            self.baseURL = baseURL
        } else {
            let config = AppConfig.load()
            self.baseURL = URL(string: config.apiBaseURL) ?? URL(string: "http://joyos-mac-mini.tail77a6ef.ts.net:2731")!
        }
        if let session {
            self.session = session
        } else {
            let configuration = URLSessionConfiguration.default
            configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
            configuration.timeoutIntervalForRequest = 20
            configuration.timeoutIntervalForResource = 120
            configuration.waitsForConnectivity = false
            self.session = URLSession(configuration: configuration)
        }

        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .custom(Self.decodeDate)
        self.decoder = decoder

        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        self.encoder = encoder
    }

    func createChat(name: String? = nil) async throws -> String {
        let payload = name.map { CreateChatRequest(name: $0) }
        let body = try payload.map { try encoder.encode($0) }
        let response: CreateChatResponse = try await request(
            path: "/threads",
            method: "POST",
            body: body,
            retryable: false,
            idempotencyKey: nil
        )
        return response.chatID
    }

    func listChats() async throws -> [ChatSummary] {
        let response: ChatListResponse = try await request(
            path: "/threads",
            method: "GET",
            body: Optional<Data>.none,
            retryable: true,
            idempotencyKey: nil
        )
        return response.chats
    }

    func getMetadata(chatID: String) async throws -> ChatMetadata {
        try await request(
            path: "/threads/\(chatID)/metadata",
            method: "GET",
            body: Optional<Data>.none,
            retryable: true,
            idempotencyKey: nil
        )
    }

    func getMessages(chatID: String, start: Int, end: Int) async throws -> [ChatMessage] {
        let response: MessageEnvelope = try await request(
            path: "/threads/\(chatID)/messages?start=\(start)&end=\(end)",
            method: "GET",
            body: Optional<Data>.none,
            retryable: true,
            idempotencyKey: nil
        )
        return response.messages
    }

    func listAvailableModels() async throws -> [ClientModel] {
        let response: ModelListResponse = try await request(
            path: "/models",
            method: "GET",
            body: Optional<Data>.none,
            retryable: true,
            idempotencyKey: nil
        )
        return response.models
    }

    func enterChat(threadID: String, knownTailTurnID: String?) async throws -> EnterChatResponse {
        let payload = try encoder.encode(
            EnterChatRequest(
                protocolVersion: ChatTransportClient.protocolVersion,
                knownTailTurnID: knownTailTurnID
            )
        )
        return try await request(
            path: "/threads/\(threadID)/enter-chat",
            method: "POST",
            body: payload,
            retryable: false,
            idempotencyKey: nil
        )
    }

    private func request<T: Decodable>(
        path: String,
        method: String,
        body: Data?,
        retryable: Bool,
        idempotencyKey: String?
    ) async throws -> T {
        guard let url = URL(string: path, relativeTo: baseURL) else {
            throw SheafError.invalidURL
        }

        let maxAttempts = retryable ? 3 : 1
        var attempt = 0
        var lastError: Error?

        while attempt < maxAttempts {
            attempt += 1
            let requestID = UUID().uuidString

            var request = URLRequest(url: url)
            request.httpMethod = method
            request.timeoutInterval = 20
            request.setValue("application/json", forHTTPHeaderField: "Accept")
            request.setValue(requestID, forHTTPHeaderField: "X-Request-ID")
            if let idempotencyKey {
                request.setValue(idempotencyKey, forHTTPHeaderField: "X-Idempotency-Key")
            }
            if body != nil {
                request.setValue("application/json", forHTTPHeaderField: "Content-Type")
                request.httpBody = body
            }

            await AppFileLogger.shared.log(
                "\(method) \(url.absoluteString) attempt=\(attempt)/\(maxAttempts) request_id=\(requestID) body_bytes=\(body?.count ?? 0)",
                category: "network"
            )

            do {
                let (data, response) = try await session.data(for: request)
                guard let http = response as? HTTPURLResponse else {
                    await AppFileLogger.shared.log(
                        "\(method) \(url.absoluteString) bad HTTP response request_id=\(requestID)",
                        category: "network"
                    )
                    throw SheafError.badResponse
                }

                await AppFileLogger.shared.log(
                    "\(method) \(url.absoluteString) status=\(http.statusCode) bytes=\(data.count) request_id=\(requestID)",
                    category: "network"
                )

                guard (200...299).contains(http.statusCode) else {
                    let message = String(data: data, encoding: .utf8) ?? "unknown"
                    throw SheafError.serverError(status: http.statusCode, message: message)
                }

                do {
                    return try decoder.decode(T.self, from: data)
                } catch {
                    let responsePreview = String(data: data.prefix(220), encoding: .utf8) ?? "<non-utf8>"
                    await AppFileLogger.shared.log(
                        "\(method) \(url.absoluteString) decode_failed request_id=\(requestID) preview=\(responsePreview)",
                        category: "network"
                    )
                    throw SheafError.decodingFailed(
                        details: "Status \(http.statusCode), body prefix: \(responsePreview)"
                    )
                }
            } catch {
                lastError = error
                await AppFileLogger.shared.log(
                    "\(method) \(url.absoluteString) error attempt=\(attempt) request_id=\(requestID) error=\(String(describing: error))",
                    category: "network"
                )
                guard retryable, shouldRetry(error), attempt < maxAttempts else {
                    if let urlError = error as? URLError {
                        throw SheafError.networkError(urlError.localizedDescription)
                    }
                    if let sheafError = error as? SheafError {
                        throw sheafError
                    }
                    throw error
                }

                let delay = retryDelay(for: attempt)
                await AppFileLogger.shared.log(
                    "\(method) \(url.absoluteString) retrying_in=\(String(format: "%.2f", delay))s",
                    category: "network"
                )
                try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
            }
        }

        if let sheafError = lastError as? SheafError {
            throw sheafError
        }
        if let urlError = lastError as? URLError {
            throw SheafError.networkError(urlError.localizedDescription)
        }
        throw lastError ?? SheafError.badResponse
    }

    private func shouldRetry(_ error: Error) -> Bool {
        guard let urlError = error as? URLError else {
            return false
        }

        switch urlError.code {
        case .timedOut,
             .cannotFindHost,
             .cannotConnectToHost,
             .networkConnectionLost,
             .dnsLookupFailed,
             .notConnectedToInternet,
             .internationalRoamingOff,
             .callIsActive,
             .dataNotAllowed:
            return true
        default:
            return false
        }
    }

    private func retryDelay(for attempt: Int) -> Double {
        switch attempt {
        case 1: return 0.25
        case 2: return 0.75
        default: return 1.5
        }
    }

    private static func decodeDate(from decoder: Decoder) throws -> Date {
        let container = try decoder.singleValueContainer()
        let raw = try container.decode(String.self)

        if let date = fractionalISO8601.date(from: raw) ?? standardISO8601.date(from: raw) {
            return date
        }
        if let date = microsecondDateFormatter.date(from: raw) ?? secondDateFormatter.date(from: raw) {
            return date
        }

        throw DecodingError.dataCorruptedError(in: container, debugDescription: "Invalid date format: \(raw)")
    }

    private static let standardISO8601: ISO8601DateFormatter = {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime]
        return formatter
    }()

    private static let fractionalISO8601: ISO8601DateFormatter = {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return formatter
    }()

    private static let microsecondDateFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(secondsFromGMT: 0)
        formatter.dateFormat = "yyyy-MM-dd'T'HH:mm:ss.SSSSSSXXXXX"
        return formatter
    }()

    private static let secondDateFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(secondsFromGMT: 0)
        formatter.dateFormat = "yyyy-MM-dd'T'HH:mm:ssXXXXX"
        return formatter
    }()
}
import Foundation

struct ChatTransportFrame {
    let type: String
    let payload: [String: Any]
}

enum ChatTransportEvent {
    case handshakeBegin
    case handshakeReady
    case durableAck(queueID: Int, clientMessageID: String?)
    case assistantToken(queueID: Int, chunk: String)
    case committedTurn(CommittedTurn)
    case finalized(queueID: Int, turnID: String)
    case conflict(queueID: Int)
    case heartbeat
    case error(String)
}

actor ChatTransportClient {
    static let protocolVersion = 1

    private let api: SheafAPIClient
    private let baseURL: URL
    private let session: URLSession

    private var socket: URLSessionWebSocketTask?
    private var sessionID: String?
    private var listenTask: Task<Void, Never>?
    private var onEvent: ((ChatTransportEvent) async -> Void)?

    init(api: SheafAPIClient = .shared, session: URLSession = .shared) {
        self.api = api
        self.session = session
        let config = AppConfig.load()
        self.baseURL = URL(string: config.apiBaseURL) ?? URL(string: "http://127.0.0.1:2731")!
    }

    func connect(
        threadID: String,
        knownTailTurnID: String?,
        onEvent: @escaping (ChatTransportEvent) async -> Void
    ) async throws {
        self.onEvent = onEvent
        let enter = try await api.enterChat(threadID: threadID, knownTailTurnID: knownTailTurnID)
        sessionID = enter.sessionID
        let wsURL = websocketURL(path: enter.websocketURL)

        let task = session.webSocketTask(with: wsURL)
        socket = task
        task.resume()

        listenTask?.cancel()
        listenTask = Task {
            await receiveLoop()
        }
    }

    func reconnect(threadID: String, knownTailTurnID: String?) async throws {
        await disconnect()
        try await connect(threadID: threadID, knownTailTurnID: knownTailTurnID) { [weak self] event in
            guard let self else { return }
            await self.onEvent?(event)
        }
    }

    func disconnect() {
        listenTask?.cancel()
        listenTask = nil
        socket?.cancel(with: .normalClosure, reason: nil)
        socket = nil
        sessionID = nil
    }

    func submitMessage(
        threadID: String,
        text: String,
        modelName: String,
        inResponseToTurnID: String?,
        clientMessageID: String
    ) async throws {
        guard let socket else {
            throw SheafError.networkError("Not connected")
        }
        let payload: [String: Any?] = [
            "protocol_version": Self.protocolVersion,
            "type": "submit_message",
            "thread_id": threadID,
            "text": text,
            "model_name": modelName,
            "in_response_to_turn_id": inResponseToTurnID,
            "client_message_id": clientMessageID,
        ]
        let data = try JSONSerialization.data(withJSONObject: payload.compactMapValues { $0 }, options: [])
        guard let string = String(data: data, encoding: .utf8) else {
            throw SheafError.badResponse
        }
        try await socket.send(.string(string))
    }

    private func websocketURL(path: String) -> URL {
        if let absolute = URL(string: path), absolute.scheme?.hasPrefix("ws") == true {
            return absolute
        }
        var components = URLComponents(url: baseURL, resolvingAgainstBaseURL: false)
        components?.scheme = baseURL.scheme == "https" ? "wss" : "ws"
        components?.path = path
        return components?.url ?? URL(string: "ws://127.0.0.1:2731\(path)")!
    }

    private func receiveLoop() async {
        while !Task.isCancelled {
            guard let socket else { return }
            do {
                let message = try await socket.receive()
                let raw: Data
                switch message {
                case .string(let text):
                    raw = Data(text.utf8)
                case .data(let data):
                    raw = data
                @unknown default:
                    continue
                }
                guard let obj = try JSONSerialization.jsonObject(with: raw) as? [String: Any],
                      let type = obj["type"] as? String else {
                    await onEvent?(.error("Malformed websocket frame"))
                    continue
                }
                await handleFrame(type: type, payload: obj)
            } catch {
                if Task.isCancelled { return }
                await onEvent?(.error(error.localizedDescription))
                return
            }
        }
    }

    private func handleFrame(type: String, payload: [String: Any]) async {
        var merged = payload
        merged["type"] = type
        guard let event = Self.decodeEvent(from: merged) else { return }
        await onEvent?(event)
    }

    static func decodeEvent(from payload: [String: Any]) -> ChatTransportEvent? {
        guard let type = payload["type"] as? String else {
            return nil
        }
        switch type {
        case "handshake_snapshot_begin":
            return .handshakeBegin
        case "handshake_ready":
            return .handshakeReady
        case "message_durable_ack":
            return .durableAck(
                queueID: payload["queue_id"] as? Int ?? -1,
                clientMessageID: payload["client_message_id"] as? String
            )
        case "assistant_token":
            return .assistantToken(
                queueID: payload["queue_id"] as? Int ?? -1,
                chunk: payload["chunk"] as? String ?? ""
            )
        case "committed_turn":
            guard let turnObj = payload["turn"] as? [String: Any],
                  let turn = decodeCommittedTurn(turnObj) else {
                return nil
            }
            return .committedTurn(turn)
        case "turn_finalized":
            return .finalized(
                queueID: payload["queue_id"] as? Int ?? -1,
                turnID: payload["turn_id"] as? String ?? ""
            )
        case "execution_conflict":
            return .conflict(queueID: payload["queue_id"] as? Int ?? -1)
        case "heartbeat":
            return .heartbeat
        case "error":
            return .error(payload["message"] as? String ?? "Unknown error")
        default:
            return nil
        }
    }

    static func decodeCommittedTurn(_ value: [String: Any]) -> CommittedTurn? {
        guard let id = value["id"] as? String,
              let threadID = value["thread_id"] as? String,
              let speaker = value["speaker"] as? String,
              let messageText = value["message_text"] as? String else {
            return nil
        }

        let toolCallsRaw = value["tool_calls"] as? [[String: Any]] ?? []
        let toolCalls = toolCallsRaw.compactMap { raw -> ToolCallPayload? in
            guard let id = raw["id"] as? String,
                  let name = raw["name"] as? String,
                  let result = raw["result"] as? String else {
                return nil
            }
            let args = decodeJSONValueObject(raw["args"]) ?? [:]
            let isError = raw["is_error"] as? Bool ?? false
            return ToolCallPayload(id: id, name: name, args: args, result: result, isError: isError)
        }

        return CommittedTurn(
            id: id,
            threadID: threadID,
            prevTurnID: value["prev_turn_id"] as? String,
            speaker: speaker,
            messageText: messageText,
            modelName: value["model_name"] as? String,
            createdAt: value["created_at"] as? String,
            toolCalls: toolCalls
        )
    }

    static func decodeJSONValueObject(_ value: Any?) -> [String: JSONValue]? {
        guard let object = value as? [String: Any] else { return nil }
        var out: [String: JSONValue] = [:]
        for (key, raw) in object {
            out[key] = decodeJSONValue(raw)
        }
        return out
    }

    static func decodeJSONValue(_ value: Any) -> JSONValue {
        switch value {
        case let string as String:
            return .string(string)
        case let number as NSNumber:
            if CFGetTypeID(number) == CFBooleanGetTypeID() {
                return .bool(number.boolValue)
            }
            return .number(number.doubleValue)
        case let dict as [String: Any]:
            var mapped: [String: JSONValue] = [:]
            for (key, raw) in dict {
                mapped[key] = decodeJSONValue(raw)
            }
            return .object(mapped)
        case let array as [Any]:
            return .array(array.map { decodeJSONValue($0) })
        default:
            return .null
        }
    }
}
