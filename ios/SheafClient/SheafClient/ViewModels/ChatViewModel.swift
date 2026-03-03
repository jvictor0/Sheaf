import Foundation

@MainActor
final class ChatViewModel: ObservableObject {
    @Published private(set) var messages: [RenderedMessage] = []
    @Published private(set) var metadata: ChatMetadata?
    @Published private(set) var isLoading = false
    @Published private(set) var errorMessage: String?

    private let chatID: String
    private let api: SheafAPIClient
    private let segmenter: MarkdownSegmenter
    private let recentWindowSize: Int

    init(chatID: String, api: SheafAPIClient = .shared, segmenter: MarkdownSegmenter = MarkdownSegmenter(), recentWindowSize: Int = 80) {
        self.chatID = chatID
        self.api = api
        self.segmenter = segmenter
        self.recentWindowSize = recentWindowSize
    }

    var currentChatID: String { chatID }

    func loadInitial() async {
        isLoading = true
        defer { isLoading = false }

        do {
            let metadata = try await api.getMetadata(chatID: chatID)
            self.metadata = metadata
            let start = max(0, metadata.messageCount - recentWindowSize)
            let raw = try await api.getMessages(chatID: chatID, start: start, end: metadata.messageCount)
            messages = raw.map(segmenter.segmented)
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func sendMessage(_ text: String) async {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }

        let userRaw = ChatMessage(index: messages.count, role: "user", content: trimmed)
        messages.append(segmenter.segmented(message: userRaw))

        do {
            let response = try await api.sendMessage(chatID: chatID, text: trimmed)
            let assistantRaw = ChatMessage(index: messages.count, role: "assistant", content: response.response)
            messages.append(segmenter.segmented(message: assistantRaw))
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func prefetchMath(for appearance: MathAppearance) {
        let snapshot = Array(messages.suffix(20))
        Task(priority: .utility) {
            for message in snapshot {
                for segment in message.segments {
                    switch segment {
                    case .inlineMath(let tex, _):
                        _ = await MathJaxRenderService.shared.render(tex: tex, block: false, appearance: appearance)
                    case .blockMath(let tex, _):
                        _ = await MathJaxRenderService.shared.render(tex: tex, block: true, appearance: appearance)
                    default:
                        continue
                    }
                }
            }
        }
    }
}
