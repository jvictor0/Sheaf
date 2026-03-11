import Foundation

enum ClientModel: String, CaseIterable, Identifiable, Codable {
    case gpt5Mini = "gpt-5-mini"
    case gpt52 = "gpt-5.2"
    case gpt53Codex = "gpt-5.3-codex"
    case gpt54 = "gpt-5.4"

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .gpt5Mini:
            return "gpt-5-mini"
        case .gpt52:
            return "gpt-5.2"
        case .gpt53Codex:
            return "gpt-5.3-codex"
        case .gpt54:
            return "gpt-5.4"
        }
    }
}
