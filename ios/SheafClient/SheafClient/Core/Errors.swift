import Foundation

enum SheafError: LocalizedError {
    case invalidURL
    case badResponse
    case decodingFailed(details: String?)
    case networkError(String)
    case serverError(status: Int, message: String)

    var errorDescription: String? {
        switch self {
        case .invalidURL:
            return "Invalid server URL."
        case .badResponse:
            return "Invalid server response."
        case .decodingFailed(let details):
            if let details, !details.isEmpty {
                return "Failed to decode server response. \(details)"
            }
            return "Failed to decode server response."
        case .networkError(let message):
            return "Network error: \(message)"
        case .serverError(let status, let message):
            return "Server error (\(status)): \(message)"
        }
    }
}
