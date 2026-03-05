//
//  SheafClientTests.swift
//  SheafClientTests
//
//  Created by Joyo on 3/3/26.
//

import Testing
@testable import SheafClient

struct SheafClientTests {
    @Test func latexDocumentFenceStaysCodeBlock() {
        let text = """
        Certainly! Here is a sketch:
        ```latex
        \\documentclass{article}
        \\usepackage{amsmath}
        \\begin{document}
        \\title{Sketch}
        \\end{document}
        ```
        """
        let segmenter = MarkdownSegmenter()
        let rendered = segmenter.segmented(message: ChatMessage(index: 0, role: "assistant", content: text))

        let hasCodeBlock = rendered.segments.contains {
            if case .codeBlock = $0 { return true }
            return false
        }
        let hasBlockMath = rendered.segments.contains {
            if case .blockMath = $0 { return true }
            return false
        }

        #expect(hasCodeBlock)
        #expect(!hasBlockMath)
    }

    @Test func latexMathFenceRendersAsBlockMath() {
        let text = """
        ```latex
        \\[
        x^2 + 1 = 0
        \\]
        ```
        """
        let segmenter = MarkdownSegmenter()
        let rendered = segmenter.segmented(message: ChatMessage(index: 0, role: "assistant", content: text))

        let blockMath = rendered.segments.compactMap { segment -> String? in
            if case .blockMath(let tex, _) = segment {
                return tex
            }
            return nil
        }

        #expect(blockMath.count == 1)
        #expect(blockMath.first == "x^2 + 1 = 0")
    }

    @Test func markdownBlockParserParsesHeadingsAndParagraphs() {
        let text = """
        # Title

        ## Section
        A paragraph with **bold** text.
        """

        let blocks = MarkdownBlockParser().parse(text)

        #expect(blocks.count == 3)
        #expect(blocks[0] == .heading(level: 1, text: "Title"))
        #expect(blocks[1] == .heading(level: 2, text: "Section"))
        #expect(blocks[2] == .paragraph("A paragraph with **bold** text."))
    }

    @Test func markdownBlockParserParsesTables() {
        let text = """
        | Name | Value |
        | --- | ---: |
        | a | 1 |
        | b | 2 |
        """

        let blocks = MarkdownBlockParser().parse(text)
        #expect(blocks.count == 1)
        #expect(
            blocks[0] == .table(
                headers: ["Name", "Value"],
                rows: [
                    ["a", "1"],
                    ["b", "2"],
                ]
            )
        )
    }

    @Test func markdownBlockParserParsesListsAndQuotes() {
        let text = """
        - first
        - second

        > quoted line
        > second line

        1. one
        2. two
        """

        let blocks = MarkdownBlockParser().parse(text)
        #expect(blocks.count == 3)
        #expect(blocks[0] == .unorderedList(["first", "second"]))
        #expect(blocks[1] == .quote("quoted line\nsecond line"))
        #expect(blocks[2] == .orderedList(["one", "two"]))
    }
}
