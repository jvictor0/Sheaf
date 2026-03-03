import Foundation

actor SheafAPIClient {
    static let shared = SheafAPIClient()

    private let baseURL: URL
    private let session: URLSession
    private let decoder: JSONDecoder
    private let encoder: JSONEncoder

    init(baseURL: URL? = nil, session: URLSession = .shared) {
        if let baseURL {
            self.baseURL = baseURL
        } else {
            let config = AppConfig.load()
            self.baseURL = URL(string: config.apiBaseURL) ?? URL(string: "http://127.0.0.1:2731")!
        }
        self.session = session

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
        let response: CreateChatResponse = try await request(path: "/chats", method: "POST", body: body)
        return response.chatID
    }

    func listChats() async throws -> [ChatSummary] {
        let response: ChatListResponse = try await request(path: "/chats", method: "GET", body: Optional<Data>.none)
        return response.chats
    }

    func getMetadata(chatID: String) async throws -> ChatMetadata {
        try await request(path: "/chats/\(chatID)/metadata", method: "GET", body: Optional<Data>.none)
    }

    func getMessages(chatID: String, start: Int, end: Int) async throws -> [ChatMessage] {
        let response: MessageEnvelope = try await request(
            path: "/chats/\(chatID)/messages?start=\(start)&end=\(end)",
            method: "GET",
            body: Optional<Data>.none
        )
        return response.messages
    }

    func sendMessage(chatID: String, text: String) async throws -> SendMessageResponse {
        let payload = try encoder.encode(SendMessageRequest(message: text))
        return try await request(path: "/chats/\(chatID)/messages", method: "POST", body: payload)
    }

    private func request<T: Decodable>(path: String, method: String, body: Data?) async throws -> T {
        guard let url = URL(string: path, relativeTo: baseURL) else {
            throw SheafError.invalidURL
        }

        var request = URLRequest(url: url)
        request.httpMethod = method
        request.setValue("application/json", forHTTPHeaderField: "Accept")
        if body != nil {
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
            request.httpBody = body
        }

        let (data, response) = try await session.data(for: request)
        guard let http = response as? HTTPURLResponse else {
            throw SheafError.badResponse
        }

        guard (200...299).contains(http.statusCode) else {
            let message = String(data: data, encoding: .utf8) ?? "unknown"
            throw SheafError.serverError(status: http.statusCode, message: message)
        }

        do {
            return try decoder.decode(T.self, from: data)
        } catch {
            throw SheafError.decodingFailed
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
