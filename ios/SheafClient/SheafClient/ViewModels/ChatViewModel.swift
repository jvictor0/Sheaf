import Foundation

@MainActor
final class ChatViewModel: ObservableObject {
    private static let currentRenderVersion = 3
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
        if let cached = sessionStore.session(for: chatID),
           cached.messages.allSatisfy({ $0.renderVersion == Self.currentRenderVersion }) {
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
            messages = renderTimeline(raw)
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
            let prepended = renderTimeline(raw)
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
            let assistantRaw = ChatMessage(
                index: nil,
                role: "assistant",
                content: response.response,
                toolCalls: response.toolCalls
            )
            messages.append(contentsOf: renderTimeline([assistantRaw]))
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
                for math in collectMathSegments(in: message.document.blocks) {
                    _ = await MathJaxRenderService.shared.render(
                        tex: math.tex,
                        block: math.block,
                        appearance: appearance
                    )
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

    private func renderTimeline(_ raw: [ChatMessage]) -> [RenderedMessage] {
        var rendered: [RenderedMessage] = []
        rendered.reserveCapacity(raw.count * 2)
        for message in raw {
            if MessageRole(rawRole: message.role) == .assistant {
                for (sequence, call) in message.toolCalls.enumerated() {
                    rendered.append(renderToolEvent(call, parentMessageID: message.id, sequence: sequence))
                }
            }
            rendered.append(segmenter.segmented(message: message))
        }
        return rendered
    }

    private func renderToolEvent(_ call: ToolCallPayload, parentMessageID: String, sequence: Int) -> RenderedMessage {
        let summary = toolSummary(call)
        return RenderedMessage(
            id: "tool-\(parentMessageID)-\(sequence)-\(call.id)",
            role: .toolEvent,
            document: RenderDocument(blocks: [.paragraph([.text(summary)])]),
            renderVersion: 3
        )
    }

    private func collectMathSegments(in blocks: [RenderBlock]) -> [(tex: String, block: Bool)] {
        var output: [(tex: String, block: Bool)] = []

        for block in blocks {
            switch block {
            case .heading(_, let content), .paragraph(let content):
                output.append(contentsOf: collectInlineMath(in: content))
            case .unorderedList(let items):
                for item in items {
                    output.append(contentsOf: collectInlineMath(in: item))
                }
            case .orderedList(_, let items):
                for item in items {
                    output.append(contentsOf: collectInlineMath(in: item))
                }
            case .table(let headers, let rows):
                for cell in headers {
                    output.append(contentsOf: collectInlineMath(in: cell))
                }
                for row in rows {
                    for cell in row {
                        output.append(contentsOf: collectInlineMath(in: cell))
                    }
                }
            case .quote(let nested):
                output.append(contentsOf: collectMathSegments(in: nested))
            case .mathBlock(let tex, _):
                output.append((tex: tex, block: true))
            case .codeBlock, .thematicBreak:
                continue
            }
        }

        return output
    }

    private func collectInlineMath(in nodes: [InlineNode]) -> [(tex: String, block: Bool)] {
        nodes.compactMap { node in
            if case .mathInline(let tex, _) = node {
                return (tex: tex, block: false)
            }
            return nil
        }
    }

    private func toolSummary(_ call: ToolCallPayload) -> String {
        let args = call.args
        let path = args["relative_path"]?.stringValue
        let directory = args["relative_dir"]?.stringValue

        let prefix = call.isError ? "Tool call failed" : "Sheaf"

        switch call.name {
        case "list_notes":
            if let directory, !directory.isEmpty {
                return "\(prefix) listed this directory: \(directory)"
            }
            return "\(prefix) listed a directory"
        case "read_note":
            if let path, !path.isEmpty {
                return "\(prefix) read this file: \(path)"
            }
            return "\(prefix) read a file"
        case "write_note":
            if let path, !path.isEmpty {
                return "\(prefix) wrote this file: \(path)"
            }
            return "\(prefix) wrote a file"
        default:
            let details = args.isEmpty
                ? ""
                : " (\(args.sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value.stringValue)" }.joined(separator: ", ")))"
            return "\(prefix) called \(call.name)\(details)"
        }
    }
}
