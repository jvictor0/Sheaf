import Foundation

enum AppRoute: Hashable {
    case conversationList
    case chat(chatID: String)
}
