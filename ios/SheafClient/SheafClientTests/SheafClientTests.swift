import Testing
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
}
