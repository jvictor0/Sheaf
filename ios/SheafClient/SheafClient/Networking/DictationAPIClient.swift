import Foundation

actor DictationAPIClient {
    static let shared = DictationAPIClient()

    private let baseURL: URL
    private let session: URLSession
    private let decoder: JSONDecoder

    init(baseURL: URL? = nil, session: URLSession? = nil) {
        if let baseURL {
            self.baseURL = baseURL
        } else {
            let config = AppConfig.load()
            self.baseURL = URL(string: config.dictationBaseURL) ?? URL(string: "http://joyos-mac-mini.tail77a6ef.ts.net:8787")!
        }

        if let session {
            self.session = session
        } else {
            let configuration = URLSessionConfiguration.default
            configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
            configuration.timeoutIntervalForRequest = 30
            configuration.timeoutIntervalForResource = 120
            configuration.waitsForConnectivity = false
            self.session = URLSession(configuration: configuration)
        }

        self.decoder = JSONDecoder()
    }

    func dictateAudio(
        wavData: Data,
        sampleRate: Int = 16_000,
        locale: String = "en-US",
        sessionID: String
    ) async throws -> DictateAudioResponse {
        guard !wavData.isEmpty else {
            throw SheafError.networkError("Dictation audio payload is empty.")
        }

        let endpoint = Self.endpoint(baseURL: baseURL)
        let requestID = String(UUID().uuidString.prefix(8))
        let request = Self.buildRequest(
            endpoint: endpoint,
            sampleRate: sampleRate,
            locale: locale,
            sessionID: sessionID,
            requestID: requestID
        )

        let tempFileURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("sheaf-dictation-\(UUID().uuidString)")
            .appendingPathExtension("wav")
        try wavData.write(to: tempFileURL, options: [.atomic])
        defer { try? FileManager.default.removeItem(at: tempFileURL) }

        await AppFileLogger.shared.log(
            "POST \(endpoint.absoluteString) request_id=\(requestID) payload_bytes=\(wavData.count)",
            category: "dictation"
        )

        do {
            let (data, response) = try await session.upload(for: request, fromFile: tempFileURL)
            guard let http = response as? HTTPURLResponse else {
                throw SheafError.badResponse
            }

            await AppFileLogger.shared.log(
                "POST \(endpoint.absoluteString) request_id=\(requestID) status=\(http.statusCode) bytes=\(data.count)",
                category: "dictation"
            )

            guard (200 ... 299).contains(http.statusCode) else {
                let message = String(data: data, encoding: .utf8) ?? "unknown"
                throw SheafError.serverError(status: http.statusCode, message: message)
            }

            do {
                return try decoder.decode(DictateAudioResponse.self, from: data)
            } catch {
                let responsePreview = String(data: data.prefix(220), encoding: .utf8) ?? "<non-utf8>"
                throw SheafError.decodingFailed(details: "Body prefix: \(responsePreview)")
            }
        } catch {
            await AppFileLogger.shared.log(
                "POST \(endpoint.absoluteString) request_id=\(requestID) error=\(String(describing: error))",
                category: "dictation"
            )
            if let sheafError = error as? SheafError {
                throw sheafError
            }
            if let urlError = error as? URLError {
                throw SheafError.networkError(urlError.localizedDescription)
            }
            throw error
        }
    }

    static func endpoint(baseURL: URL) -> URL {
        baseURL.appendingPathComponent("v1").appendingPathComponent("dictate-audio")
    }

    static func buildRequest(
        endpoint: URL,
        sampleRate: Int,
        locale: String,
        sessionID: String,
        requestID: String
    ) -> URLRequest {
        var request = URLRequest(url: endpoint)
        request.httpMethod = "POST"
        request.timeoutInterval = 90
        request.setValue("audio/wav", forHTTPHeaderField: "Content-Type")
        request.setValue(String(sampleRate), forHTTPHeaderField: "X-Sample-Rate")
        request.setValue(locale, forHTTPHeaderField: "X-Locale")
        request.setValue(sessionID, forHTTPHeaderField: "X-Session-Id")
        request.setValue(requestID, forHTTPHeaderField: "X-Request-Id")
        return request
    }
}
