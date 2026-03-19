import Foundation

struct ClientModel: Identifiable, Decodable, Hashable {
    let name: String
    let provider: String
    let source: String
    let metadata: [String: JSONValue]
    let isDefault: Bool

    var id: String { name }
    var displayName: String { name }

    enum CodingKeys: String, CodingKey {
        case name
        case provider
        case source
        case metadata
        case isDefault = "is_default"
    }
}
