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

}
