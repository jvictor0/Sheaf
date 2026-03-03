import Foundation

@MainActor
final class ChatViewModel: ObservableObject {
    @Published private(set) var messages: [RenderedMessage] = []
    @Published private(set) var metadata: ChatMetadata?
    @Published private(set) var isLoading = false
    @Published private(set) var isLoadingOlder = false
    @Published private(set) var errorMessage: String?

    private let chatID: String
    private let api: SheafAPIClient
    private let segmenter: MarkdownSegmenter
    private let sessionStore: ChatSessionStore
    private let recentWindowSize: Int
    private let olderPageSize: Int
    private var oldestLoadedIndex: Int = 0
    private var newestLoadedExclusiveIndex: Int = 0
    private var hasMoreOlder: Bool = false
    private var hasLoadedInitial = false

    init(
        chatID: String,
        sessionStore: ChatSessionStore,
        api: SheafAPIClient = .shared,
        segmenter: MarkdownSegmenter = MarkdownSegmenter(),
        recentWindowSize: Int = 80,
        olderPageSize: Int = 80
    ) {
        self.chatID = chatID
        self.sessionStore = sessionStore
        self.api = api
        self.segmenter = segmenter
        self.recentWindowSize = recentWindowSize
        self.olderPageSize = olderPageSize
    }

    var currentChatID: String { chatID }
    var canLoadOlder: Bool { hasMoreOlder && !isLoading && !isLoadingOlder }

    func loadInitial() async {
        if hasLoadedInitial {
            return
        }
        if let cached = sessionStore.session(for: chatID) {
            messages = cached.messages
            metadata = cached.metadata
            oldestLoadedIndex = cached.oldestLoadedIndex
            newestLoadedExclusiveIndex = cached.newestLoadedExclusiveIndex
            hasMoreOlder = cached.hasMoreOlder
            hasLoadedInitial = true
            return
        }

        isLoading = true
        defer { isLoading = false }

        do {
            let metadata = try await api.getMetadata(chatID: chatID)
            self.metadata = metadata
            newestLoadedExclusiveIndex = metadata.messageCount
            let start = max(0, newestLoadedExclusiveIndex - recentWindowSize)
            let raw = try await api.getMessages(chatID: chatID, start: start, end: newestLoadedExclusiveIndex)
            messages = raw.map(segmenter.segmented)
            oldestLoadedIndex = start
            hasMoreOlder = start > 0
            hasLoadedInitial = true
            persistSession()
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func loadOlder() async -> Bool {
        guard hasLoadedInitial else {
            await loadInitial()
            return false
        }
        guard canLoadOlder else {
            return false
        }

        isLoadingOlder = true
        defer { isLoadingOlder = false }

        let end = oldestLoadedIndex
        let start = max(0, end - olderPageSize)
        guard start < end else {
            hasMoreOlder = false
            persistSession()
            return false
        }

        do {
            let raw = try await api.getMessages(chatID: chatID, start: start, end: end)
            let prepended = raw.map(segmenter.segmented)
            guard !prepended.isEmpty else {
                oldestLoadedIndex = start
                hasMoreOlder = start > 0
                persistSession()
                return false
            }

            messages = prepended + messages
            oldestLoadedIndex = start
            hasMoreOlder = start > 0
            persistSession()
            errorMessage = nil
            return true
        } catch {
            errorMessage = error.localizedDescription
            return false
        }
    }

    func sendMessage(_ text: String) async {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }

        let userRaw = ChatMessage(index: nil, role: "user", content: trimmed)
        messages.append(segmenter.segmented(message: userRaw))
        newestLoadedExclusiveIndex += 1
        persistSession()

        do {
            let response = try await api.sendMessage(chatID: chatID, text: trimmed)
            let assistantRaw = ChatMessage(index: nil, role: "assistant", content: response.response)
            messages.append(segmenter.segmented(message: assistantRaw))
            newestLoadedExclusiveIndex += 1
            persistSession()
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

    private func persistSession() {
        let session = ChatSessionStore.Session(
            messages: messages,
            metadata: metadata,
            oldestLoadedIndex: oldestLoadedIndex,
            newestLoadedExclusiveIndex: newestLoadedExclusiveIndex,
            hasMoreOlder: hasMoreOlder
        )
        sessionStore.save(session, for: chatID)
    }
}
