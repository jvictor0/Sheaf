import Foundation

struct RenderedMessage: Identifiable, Hashable {
    let id: String
    let role: MessageRole
    let segments: [MessageSegment]
    let renderVersion: Int
}

enum MessageRole: String, Hashable {
    case user
    case assistant
    case system

    init(rawRole: String) {
        switch rawRole.lowercased() {
        case "user":
            self = .user
        case "assistant":
            self = .assistant
        default:
            self = .system
        }
    }
}

enum MessageSegment: Hashable {
    case markdownText(String)
    case codeBlock(language: String?, text: String)
    case inlineMath(tex: String, key: String)
    case blockMath(tex: String, key: String)
}
