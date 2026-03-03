import Testing
@testable import SheafIOSApp

struct MarkdownSegmenterTests {
    @Test
    func splitsInlineAndBlockMath() {
        let segmenter = MarkdownSegmenter()
        let input = ChatMessage(index: 0, role: "assistant", content: "hello $x^2$ and $$\\frac{a}{b}$$")

        let rendered = segmenter.segmented(message: input)
        #expect(rendered.segments.count == 4)

        guard case .inlineMath(let inline, _) = rendered.segments[1] else {
            Issue.record("Expected inline math segment")
            return
        }
        #expect(inline == "x^2")

        guard case .blockMath(let block, _) = rendered.segments[3] else {
            Issue.record("Expected block math segment")
            return
        }
        #expect(block == "\\frac{a}{b}")
    }

    @Test
    func keepsCodeFenceAsCodeBlock() {
        let segmenter = MarkdownSegmenter()
        let input = ChatMessage(index: 0, role: "assistant", content: "```swift\nlet x = 1\n```")

        let rendered = segmenter.segmented(message: input)
        #expect(rendered.segments.count == 1)

        guard case .codeBlock(let language, let code) = rendered.segments[0] else {
            Issue.record("Expected code block segment")
            return
        }
        #expect(language == "swift")
        #expect(code == "let x = 1")
    }

    @Test
    func mapsMathFenceToBlockMath() {
        let segmenter = MarkdownSegmenter()
        let input = ChatMessage(index: 0, role: "assistant", content: "```math\n\\\\frac{5!}{2}\n```")

        let rendered = segmenter.segmented(message: input)
        #expect(rendered.segments.count == 1)

        guard case .blockMath(let tex, _) = rendered.segments[0] else {
            Issue.record("Expected block math segment for math fence")
            return
        }
        #expect(tex == "\\\\frac{5!}{2}")
    }
}
