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
            path: "/chats",
            method: "POST",
            body: body,
            retryable: false,
            idempotencyKey: nil
        )
        return response.chatID
    }

    func listChats() async throws -> [ChatSummary] {
        let response: ChatListResponse = try await request(
            path: "/chats",
            method: "GET",
            body: Optional<Data>.none,
            retryable: true,
            idempotencyKey: nil
        )
        return response.chats
    }

    func getMetadata(chatID: String) async throws -> ChatMetadata {
        try await request(
            path: "/chats/\(chatID)/metadata",
            method: "GET",
            body: Optional<Data>.none,
            retryable: true,
            idempotencyKey: nil
        )
    }

    func getMessages(chatID: String, start: Int, end: Int) async throws -> [ChatMessage] {
        let response: MessageEnvelope = try await request(
            path: "/chats/\(chatID)/messages?start=\(start)&end=\(end)",
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

    func sendMessage(chatID: String, text: String) async throws -> SendMessageResponse {
        let selectedModel = await MainActor.run { ClientSettingsStore.shared.selectedModelName }
        let payload = try encoder.encode(SendMessageRequest(message: text, model: selectedModel))
        let idempotencyKey = UUID().uuidString
        return try await request(
            path: "/chats/\(chatID)/messages",
            method: "POST",
            body: payload,
            retryable: true,
            idempotencyKey: idempotencyKey
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
