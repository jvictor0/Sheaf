import Foundation

@MainActor
final class ChatViewModel: ObservableObject {
    private static let currentRenderVersion = 3
    private static let streamFlushIntervalNS: UInt64 = 75_000_000

    @Published private(set) var messages: [RenderedMessage] = []
    @Published private(set) var metadata: ChatMetadata?
    @Published private(set) var isLoading = false
    @Published private(set) var isLoadingOlder = false
    @Published private(set) var errorMessage: String?

    private let chatID: String
    private let api: SheafAPIClient
    private let transport: ChatTransportClient
    private let segmenter: MarkdownSegmenter
    private let sessionStore: ChatSessionStore

    private var hasLoadedInitial = false
    private var committedTurns: [CommittedTurn] = []
    private var pendingSends: [PendingSend] = []
    private var streamingByQueue: [Int: StreamingAssistantTurn] = [:]
    private var dirtyStreamingQueueIDs: Set<Int> = []
    private var lastCommittedTurnID: String?

    private var watchdogTask: Task<Void, Never>?
    private var streamFlushTask: Task<Void, Never>?
    private var lastFrameAt = Date()

    init(
        chatID: String,
        sessionStore: ChatSessionStore,
        api: SheafAPIClient = .shared,
        transport: ChatTransportClient = ChatTransportClient(),
        segmenter: MarkdownSegmenter = MarkdownSegmenter()
    ) {
        self.chatID = chatID
        self.sessionStore = sessionStore
        self.api = api
        self.transport = transport
        self.segmenter = segmenter
    }

    deinit {
        let transport = self.transport
        watchdogTask?.cancel()
        streamFlushTask?.cancel()
        Task { await transport.disconnect() }
    }

    var currentChatID: String { chatID }
    var canLoadOlder: Bool { false }

    func loadInitial() async {
        guard !hasLoadedInitial else { return }
        isLoading = true
        defer { isLoading = false }

        do {
            try await connectTransport(knownTailTurnID: nil)
            hasLoadedInitial = true
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func loadOlder() async -> Bool {
        false
    }

    func sendMessage(_ text: String) async {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }

        let localMessageID = "local-user-\(UUID().uuidString)"
        let userRaw = ChatMessage(index: nil, role: "user", content: trimmed)
        let rendered = segmenter.segmented(message: userRaw)
        messages.append(
            RenderedMessage(
                id: localMessageID,
                role: rendered.role,
                document: rendered.document,
                renderVersion: Self.currentRenderVersion
            )
        )

        let clientMessageID = UUID().uuidString
        pendingSends.append(
            PendingSend(
                clientMessageID: clientMessageID,
                text: trimmed,
                responseToTurnID: lastCommittedTurnID,
                localMessageID: localMessageID
            )
        )

        do {
            let selectedModel = await MainActor.run { ClientSettingsStore.shared.selectedModelName }
            try await transport.submitMessage(
                threadID: chatID,
                text: trimmed,
                modelName: selectedModel,
                inResponseToTurnID: lastCommittedTurnID,
                clientMessageID: clientMessageID
            )
        } catch {
            errorMessage = error.localizedDescription
        }
        persistSession()
    }

    func prefetchMath(for appearance: MathAppearance) {
        let snapshot = Array(messages.suffix(20))
        Task(priority: .utility) {
            for message in snapshot {
                for math in self.collectMathSegments(in: message.document.blocks) {
                    _ = await MathJaxRenderService.shared.render(
                        tex: math.tex,
                        block: math.block,
                        appearance: appearance
                    )
                }
            }
        }
    }

    private func connectTransport(knownTailTurnID: String?) async throws {
        try await transport.connect(threadID: chatID, knownTailTurnID: knownTailTurnID) { [weak self] event in
            guard let self else { return }
            await MainActor.run {
                self.handleTransportEvent(event)
            }
        }

        lastFrameAt = Date()
        watchdogTask?.cancel()
        watchdogTask = Task { [weak self] in
            guard let self else { return }
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 5_000_000_000)
                await self.checkConnectionHealth()
            }
        }
    }

    private func checkConnectionHealth() async {
        let age = Date().timeIntervalSince(lastFrameAt)
        if age < 45 {
            return
        }

        dropUncommittedArtifacts()
        do {
            try await connectTransport(knownTailTurnID: lastCommittedTurnID)
            errorMessage = nil
        } catch {
            errorMessage = "Reconnect failed: \(error.localizedDescription)"
        }
    }

    private func handleTransportEvent(_ event: ChatTransportEvent) {
        lastFrameAt = Date()

        switch event {
        case .handshakeBegin:
            committedTurns.removeAll()
            dropUncommittedArtifacts()
            rebuildMessages()
        case .handshakeReady:
            errorMessage = nil
        case .durableAck:
            return
        case .assistantToken(let queueID, let chunk):
            var stream = streamingByQueue[queueID] ?? StreamingAssistantTurn(queueID: queueID, text: "")
            stream.text += chunk
            streamingByQueue[queueID] = stream
            dirtyStreamingQueueIDs.insert(queueID)
            scheduleStreamingFlushIfNeeded()
        case .committedTurn(let turn):
            if committedTurns.contains(where: { $0.id == turn.id }) {
                return
            }
            committedTurns.append(turn)
            lastCommittedTurnID = turn.id
            consumeMatchingPendingSendIfNeeded(turn)
            rebuildMessages()
        case .finalized(let queueID, _):
            streamingByQueue.removeValue(forKey: queueID)
            dirtyStreamingQueueIDs.remove(queueID)
            removeStreamingRenderedMessage(queueID: queueID)
            if streamingByQueue.isEmpty {
                streamFlushTask?.cancel()
                streamFlushTask = nil
            }
            persistSession()
        case .conflict:
            dropUncommittedArtifacts()
            Task { [weak self] in
                guard let self else { return }
                do {
                    try await self.connectTransport(knownTailTurnID: self.lastCommittedTurnID)
                } catch {
                    await MainActor.run {
                        self.errorMessage = "Resync failed: \(error.localizedDescription)"
                    }
                }
            }
        case .heartbeat:
            return
        case .error(let message):
            errorMessage = message
        }
    }

    private func consumeMatchingPendingSendIfNeeded(_ turn: CommittedTurn) {
        guard turn.speaker == "user" else { return }
        guard let idx = pendingSends.firstIndex(where: { $0.text == turn.messageText }) else { return }
        pendingSends.remove(at: idx)
    }

    private func rebuildMessages() {
        var rendered: [RenderedMessage] = []
        rendered.reserveCapacity(committedTurns.count + pendingSends.count + streamingByQueue.count)

        for turn in committedTurns {
            let chat = ChatMessage(index: nil, role: turn.speaker, content: turn.messageText, toolCalls: turn.toolCalls)
            if MessageRole(rawRole: chat.role) == .assistant {
                for (sequence, call) in chat.toolCalls.enumerated() {
                    rendered.append(renderToolEvent(call, parentMessageID: chat.id, sequence: sequence))
                }
            }
            rendered.append(segmenter.segmented(message: chat))
        }

        for pending in pendingSends {
            rendered.append(renderPendingSend(pending))
        }

        for stream in streamingByQueue.values.sorted(by: { $0.queueID < $1.queueID }) {
            rendered.append(renderStreamingMessage(stream))
        }

        messages = rendered
        persistSession()
    }

    private func dropUncommittedArtifacts() {
        pendingSends.removeAll()
        streamingByQueue.removeAll()
        dirtyStreamingQueueIDs.removeAll()
        streamFlushTask?.cancel()
        streamFlushTask = nil
        rebuildMessages()
    }

    private func scheduleStreamingFlushIfNeeded() {
        if streamFlushTask != nil {
            return
        }
        streamFlushTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: Self.streamFlushIntervalNS)
                guard let self else { return }
                self.flushStreamingUpdates()
                if self.shouldStopStreamingFlushLoop() {
                    self.markStreamingFlushStopped()
                    return
                }
            }
        }
    }

    private func flushStreamingUpdates() {
        guard !dirtyStreamingQueueIDs.isEmpty else { return }
        let queueIDs = dirtyStreamingQueueIDs.sorted()
        dirtyStreamingQueueIDs.removeAll()
        for queueID in queueIDs {
            upsertStreamingRenderedMessage(queueID: queueID)
        }
        persistSession()
    }

    private func upsertStreamingRenderedMessage(queueID: Int) {
        guard let stream = streamingByQueue[queueID] else { return }
        let rendered = renderStreamingMessage(stream)
        if let idx = messages.firstIndex(where: { $0.id == rendered.id }) {
            messages[idx] = rendered
        } else {
            messages.append(rendered)
        }
    }

    private func removeStreamingRenderedMessage(queueID: Int) {
        messages.removeAll { $0.id == "stream-\(queueID)" }
    }

    private func shouldStopStreamingFlushLoop() -> Bool {
        dirtyStreamingQueueIDs.isEmpty && streamingByQueue.isEmpty
    }

    private func markStreamingFlushStopped() {
        streamFlushTask = nil
    }

    private func persistSession() {
        let session = ChatSessionStore.Session(
            messages: messages,
            metadata: metadata,
            oldestLoadedIndex: 0,
            newestLoadedExclusiveIndex: messages.count,
            hasMoreOlder: false
        )
        sessionStore.save(session, for: chatID)
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

    private func renderPendingSend(_ pending: PendingSend) -> RenderedMessage {
        let raw = ChatMessage(index: nil, role: "user", content: pending.text)
        return RenderedMessage(
            id: pending.localMessageID,
            role: .user,
            document: segmenter.segmented(message: raw).document,
            renderVersion: Self.currentRenderVersion
        )
    }

    private func renderStreamingMessage(_ stream: StreamingAssistantTurn) -> RenderedMessage {
        let raw = ChatMessage(index: nil, role: "assistant", content: stream.text)
        return RenderedMessage(
            id: "stream-\(stream.queueID)",
            role: .assistant,
            document: segmenter.segmented(message: raw).document,
            renderVersion: Self.currentRenderVersion
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
                : " (\(args.sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value.stringValue)" }.joined(separator: ", ")) )"
            return "\(prefix) called \(call.name)\(details)"
        }
    }
}
