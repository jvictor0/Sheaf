import Foundation

struct RenderedMessage: Identifiable, Hashable {
    let id: String
    let role: MessageRole
    let document: RenderDocument
    let renderVersion: Int
}

enum MessageRole: String, Hashable {
    case user
    case assistant
    case system
    case toolEvent

    init(rawRole: String) {
        switch rawRole.lowercased() {
        case "user":
            self = .user
        case "assistant":
            self = .assistant
        case "tool_event":
            self = .toolEvent
        default:
            self = .system
        }
    }
}

struct RenderDocument: Hashable {
    let blocks: [RenderBlock]
}

enum RenderBlock: Hashable {
    case heading(level: Int, content: [InlineNode])
    case paragraph([InlineNode])
    case unorderedList([[InlineNode]])
    case orderedList(start: Int, items: [[InlineNode]])
    case table(headers: [[InlineNode]], rows: [[[InlineNode]]])
    case quote([RenderBlock])
    case codeBlock(language: String?, text: String)
    case mathBlock(tex: String, key: String)
    case thematicBreak
}

enum InlineNode: Hashable {
    case text(String)
    case emphasis(String)
    case strong(String)
    case inlineCode(String)
    case link(text: String, destination: String)
    case lineBreak
    case mathInline(tex: String, key: String)
}
