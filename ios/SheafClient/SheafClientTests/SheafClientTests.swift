import Testing
import Foundation
@testable import SheafClient

struct SheafClientTests {
    @Test func inlineMathParenStyleParsesAndDollarStaysText() {
        let text = "text \\(f(x)\\) and $g(x)$"
        let parser = MarkdownSegmenter()
        let rendered = parser.segmented(message: ChatMessage(index: 0, role: "assistant", content: text))

        guard case .paragraph(let nodes) = rendered.document.blocks.first else {
            Issue.record("Expected first block to be paragraph")
            return
        }

        let inlineMath = nodes.compactMap { node -> String? in
            if case .mathInline(let tex, _) = node { return tex }
            return nil
        }
        let textNodes = nodes.compactMap { node -> String? in
            if case .text(let value) = node { return value }
            return nil
        }.joined()

        #expect(inlineMath == ["f(x)"])
        #expect(textNodes.contains("$g(x)$"))
    }

    @Test func unclosedInlineMathStaysLiteralText() {
        let text = "value \\(f(x) and plain"
        let parser = MarkdownSegmenter()
        let rendered = parser.segmented(message: ChatMessage(index: 0, role: "assistant", content: text))

        guard case .paragraph(let nodes) = rendered.document.blocks.first else {
            Issue.record("Expected first block to be paragraph")
            return
        }

        let inlineMathCount = nodes.filter {
            if case .mathInline = $0 { return true }
            return false
        }.count
        let textContent = nodes.compactMap { node -> String? in
            if case .text(let value) = node { return value }
            return nil
        }.joined()

        #expect(inlineMathCount == 0)
        #expect(textContent.contains("\\(f(x)"))
    }

    @Test func latexDocumentFenceStaysCodeBlock() {
        let text = """
        ```latex
        \\documentclass{article}
        \\usepackage{amsmath}
        \\begin{document}
        \\title{Sketch}
        \\end{document}
        ```
        """
        let parser = MarkdownSegmenter()
        let rendered = parser.segmented(message: ChatMessage(index: 0, role: "assistant", content: text))

        #expect(rendered.document.blocks.count == 1)
        if case .codeBlock = rendered.document.blocks[0] {
            #expect(Bool(true))
        } else {
            Issue.record("Expected code block for full LaTeX document")
        }
    }

    @Test func latexMathFenceRendersAsMathBlock() {
        let text = """
        ```latex
        \\[
        x^2 + 1 = 0
        \\]
        ```
        """
        let parser = MarkdownSegmenter()
        let rendered = parser.segmented(message: ChatMessage(index: 0, role: "assistant", content: text))

        #expect(rendered.document.blocks.count == 1)
        if case .mathBlock(let tex, _) = rendered.document.blocks[0] {
            #expect(tex == "x^2 + 1 = 0")
        } else {
            Issue.record("Expected math block from latex fence")
        }
    }

    @Test func markdownSubsetBlocksParse() {
        let text = """
        # Title

        ## Section
        A paragraph with **bold** text and \\(f(x)\\).

        - first
        - second

        > quoted line
        > second line

        | Name | Value |
        | --- | ---: |
        | a | 1 |
        """
        let parser = MarkdownSegmenter()
        let rendered = parser.segmented(message: ChatMessage(index: 0, role: "assistant", content: text))

        #expect(rendered.document.blocks.count == 6)
        #expect({
            if case .heading(level: 1, _) = rendered.document.blocks[0] { return true }
            return false
        }())
        #expect({
            if case .heading(level: 2, _) = rendered.document.blocks[1] { return true }
            return false
        }())
        #expect({
            if case .paragraph = rendered.document.blocks[2] { return true }
            return false
        }())
        #expect({
            if case .unorderedList = rendered.document.blocks[3] { return true }
            return false
        }())
        #expect({
            if case .quote = rendered.document.blocks[4] { return true }
            return false
        }())
        #expect({
            if case .table = rendered.document.blocks[5] { return true }
            return false
        }())
    }

    @Test func displayMathBracketBlockParses() {
        let text = """
        \\[
        x + y = z
        \\]
        """
        let parser = MarkdownSegmenter()
        let rendered = parser.segmented(message: ChatMessage(index: 0, role: "assistant", content: text))

        #expect(rendered.document.blocks.count == 1)
        if case .mathBlock(let tex, _) = rendered.document.blocks[0] {
            #expect(tex == "x + y = z")
        } else {
            Issue.record("Expected math block from bracket delimiters")
        }
    }

    @Test func orderedListAcrossBlankLinesPreservesSequence() {
        let text = """
        1. First item

        2. Second item

        3. Third item
        """
        let parser = MarkdownSegmenter()
        let rendered = parser.segmented(message: ChatMessage(index: 0, role: "assistant", content: text))

        #expect(rendered.document.blocks.count == 1)
        guard case .orderedList(let start, let items) = rendered.document.blocks[0] else {
            Issue.record("Expected a single ordered list block")
            return
        }

        #expect(start == 1)
        #expect(items.count == 3)
    }

    @Test func insertTextAtSelectionInEmptyDraft() {
        var text = ""
        var selection = NSRange(location: 0, length: 0)

        insertTextAtSelection("hello", text: &text, selection: &selection)

        #expect(text == "hello")
        #expect(selection.location == 5)
        #expect(selection.length == 0)
    }

    @Test func insertTextAtSelectionInMiddle() {
        var text = "Hello world"
        var selection = NSRange(location: 6, length: 0)

        insertTextAtSelection("dictated ", text: &text, selection: &selection)

        #expect(text == "Hello dictated world")
        #expect(selection.location == 15)
        #expect(selection.length == 0)
    }

    @Test func insertTextAtSelectionReplacesSelection() {
        var text = "Hello planet"
        var selection = NSRange(location: 6, length: 6)

        insertTextAtSelection("world", text: &text, selection: &selection)

        #expect(text == "Hello world")
        #expect(selection.location == 11)
        #expect(selection.length == 0)
    }

    @Test func dictationInsertionUsesRevisedTextOnly() {
        let response = DictateAudioResponse(
            rawTranscript: "raw words",
            revisedText: " polished words ",
            editSummary: "",
            uncertaintyFlags: [],
            transcribeMS: 10,
            refineMS: 15
        )

        #expect(dictationInsertionText(from: response) == "polished words")
    }

    @Test func dictationInsertionRejectsEmptyRevisedText() {
        let response = DictateAudioResponse(
            rawTranscript: "raw words",
            revisedText: "   ",
            editSummary: "",
            uncertaintyFlags: [],
            transcribeMS: 10,
            refineMS: 15
        )

        #expect(dictationInsertionText(from: response) == nil)
    }

    @Test func dictationRequestConstructionUsesExpectedEndpointAndHeaders() throws {
        let baseURL = try #require(URL(string: "http://joyos-mac-mini.tail77a6ef.ts.net:8787"))
        let endpoint = DictationAPIClient.endpoint(baseURL: baseURL)

        #expect(endpoint.absoluteString == "http://joyos-mac-mini.tail77a6ef.ts.net:8787/v1/dictate-audio")

        let request = DictationAPIClient.buildRequest(
            endpoint: endpoint,
            sampleRate: 16_000,
            locale: "en-US",
            sessionID: "session-123",
            requestID: "req-123"
        )

        #expect(request.httpMethod == "POST")
        #expect(request.url?.absoluteString == "http://joyos-mac-mini.tail77a6ef.ts.net:8787/v1/dictate-audio")
        #expect(request.value(forHTTPHeaderField: "Content-Type") == "audio/wav")
        #expect(request.value(forHTTPHeaderField: "X-Sample-Rate") == "16000")
        #expect(request.value(forHTTPHeaderField: "X-Locale") == "en-US")
        #expect(request.value(forHTTPHeaderField: "X-Session-Id") == "session-123")
        #expect(request.value(forHTTPHeaderField: "X-Request-Id") == "req-123")
    }

    @Test func sendMessageRequestEncodesSelectedModel() throws {
        let request = SendMessageRequest(message: "hello", model: "gpt-5.3-codex")
        let encoded = try JSONEncoder().encode(request)
        let jsonObject = try #require(JSONSerialization.jsonObject(with: encoded) as? [String: String])

        #expect(jsonObject["message"] == "hello")
        #expect(jsonObject["model"] == "gpt-5.3-codex")
    }

    @MainActor
    @Test func clientSettingsStoreDefaultsToGpt5Mini() {
        let suiteName = "SheafClientTests.default.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suiteName)!
        defaults.removePersistentDomain(forName: suiteName)

        let store = ClientSettingsStore(defaults: defaults, modelKey: "selected_model")
        #expect(store.selectedModelName == "gpt-5-mini")
    }

    @MainActor
    @Test func clientSettingsStoreLoadsPersistedModel() {
        let suiteName = "SheafClientTests.persisted.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suiteName)!
        defaults.removePersistentDomain(forName: suiteName)

        var store = ClientSettingsStore(defaults: defaults, modelKey: "selected_model")
        store.selectedModelName = "gpt-5.2"
        store = ClientSettingsStore(defaults: defaults, modelKey: "selected_model")

        #expect(store.selectedModelName == "gpt-5.2")
    }

    @Test func websocketFrameDecodeDurableAck() {
        let payload: [String: Any] = [
            "type": "message_durable_ack",
            "queue_id": 42,
            "client_message_id": "cm-1",
        ]
        let event = ChatTransportClient.decodeEvent(from: payload)
        guard case .durableAck(let queueID, let clientMessageID) = event else {
            Issue.record("Expected durableAck event")
            return
        }
        #expect(queueID == 42)
        #expect(clientMessageID == "cm-1")
    }

    @Test func websocketFrameDecodeAssistantToken() {
        let payload: [String: Any] = [
            "type": "assistant_token",
            "queue_id": 7,
            "chunk": "hel",
        ]
        let event = ChatTransportClient.decodeEvent(from: payload)
        guard case .assistantToken(let queueID, let chunk) = event else {
            Issue.record("Expected assistantToken event")
            return
        }
        #expect(queueID == 7)
        #expect(chunk == "hel")
    }

    @Test func websocketFrameDecodeCommittedTurn() {
        let payload: [String: Any] = [
            "type": "committed_turn",
            "turn": [
                "id": "t-1",
                "thread_id": "th-1",
                "prev_turn_id": "t-0",
                "speaker": "assistant",
                "message_text": "hello",
                "model_name": "gpt-5-mini",
                "created_at": "2026-03-19T00:00:00Z",
                "tool_calls": [
                    [
                        "id": "call-1",
                        "name": "read_note",
                        "args": ["relative_path": "a.txt"],
                        "result": "ok",
                        "is_error": false,
                    ]
                ]
            ],
        ]
        let event = ChatTransportClient.decodeEvent(from: payload)
        guard case .committedTurn(let turn) = event else {
            Issue.record("Expected committedTurn event")
            return
        }
        #expect(turn.id == "t-1")
        #expect(turn.threadID == "th-1")
        #expect(turn.toolCalls.count == 1)
        #expect(turn.toolCalls[0].name == "read_note")
        #expect(turn.toolCalls[0].args["relative_path"]?.stringValue == "a.txt")
    }

    @Test func websocketFrameDecodeHeartbeat() {
        let payload: [String: Any] = ["type": "heartbeat"]
        let event = ChatTransportClient.decodeEvent(from: payload)
        guard case .heartbeat = event else {
            Issue.record("Expected heartbeat event")
            return
        }
    }

    @Test func websocketFrameDecodeRejectsMalformedCommittedTurn() {
        let payload: [String: Any] = [
            "type": "committed_turn",
            "turn": ["id": "missing-fields"],
        ]
        let event = ChatTransportClient.decodeEvent(from: payload)
        #expect(event == nil)
    }

    @Test func chatSummaryDecodesThreadName() throws {
        let json = """
        {
          "thread_id": "abc123",
          "name": "My Thread",
          "created_at": "2026-03-19T00:00:00Z",
          "updated_at": "2026-03-19T00:00:00Z"
        }
        """.data(using: .utf8)!

        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let summary = try decoder.decode(ChatSummary.self, from: json)
        #expect(summary.chatID == "abc123")
        #expect(summary.name == "My Thread")
    }
}
