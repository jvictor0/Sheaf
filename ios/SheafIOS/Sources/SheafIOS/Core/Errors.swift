import Foundation

enum SheafError: LocalizedError {
    case invalidURL
    case badResponse
    case decodingFailed
    case serverError(status: Int, message: String)

    var errorDescription: String? {
        switch self {
        case .invalidURL:
            return "Invalid server URL."
        case .badResponse:
            return "Invalid server response."
        case .decodingFailed:
            return "Failed to decode server response."
        case .serverError(let status, let message):
            return "Server error (\(status)): \(message)"
        }
    }
}
