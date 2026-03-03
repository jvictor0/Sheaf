import Foundation
import Markdown

struct MarkdownSegmenter {
    func segmented(message: ChatMessage) -> RenderedMessage {
        // Force AST parse once to reject malformed markdown assumptions in future extensions.
        _ = Document(parsing: message.content)

        let segments = splitSegments(text: message.content)
        return RenderedMessage(
            id: message.id,
            role: MessageRole(rawRole: message.role),
            segments: segments,
            renderVersion: 1
        )
    }

    private func splitSegments(text: String) -> [MessageSegment] {
        var segments: [MessageSegment] = []
        var cursor = text.startIndex

        while cursor < text.endIndex {
            if let codeRange = nextCodeFence(in: text, from: cursor), codeRange.lowerBound == cursor {
                let fenced = String(text[codeRange])
                let parsed = parseCodeFence(fenced)
                if isMathFence(language: parsed.language) {
                    let code = parsed.code.trimmingCharacters(in: .whitespacesAndNewlines)
                    if shouldRenderAsMath(code: code) {
                        let tex = normalizedMathFenceContent(code)
                        if !tex.isEmpty {
                            segments.append(.blockMath(tex: tex, key: MathCacheKey.make(tex: tex, block: true)))
                        }
                    } else if !code.isEmpty {
                        segments.append(.codeBlock(language: parsed.language, text: parsed.code))
                    }
                } else {
                    segments.append(.codeBlock(language: parsed.language, text: parsed.code))
                }
                cursor = codeRange.upperBound
                continue
            }

            let nextFenceStart = nextCodeFence(in: text, from: cursor)?.lowerBound ?? text.endIndex
            let plainChunk = String(text[cursor..<nextFenceStart])
            segments.append(contentsOf: splitMath(in: plainChunk))
            cursor = nextFenceStart
        }

        return segments.filter {
            switch $0 {
            case .markdownText(let text):
                return !text.isEmpty
            default:
                return true
            }
        }
    }

    private func nextCodeFence(in text: String, from start: String.Index) -> Range<String.Index>? {
        guard let fenceStart = text[start...].range(of: "```")?.lowerBound else {
            return nil
        }
        let afterFence = text.index(fenceStart, offsetBy: 3, limitedBy: text.endIndex) ?? text.endIndex
        guard let fenceEnd = text[afterFence...].range(of: "```")?.upperBound else {
            return nil
        }
        return fenceStart..<fenceEnd
    }

    private func parseCodeFence(_ fenced: String) -> (language: String?, code: String) {
        let lines = fenced.components(separatedBy: "\n")
        guard lines.count >= 2 else { return (nil, fenced) }

        let opener = lines[0]
        let language = opener.replacingOccurrences(of: "```", with: "").trimmingCharacters(in: .whitespaces)
        let codeLines = Array(lines.dropFirst().dropLast())
        return (language.isEmpty ? nil : language, codeLines.joined(separator: "\n"))
    }

    private func isMathFence(language: String?) -> Bool {
        guard let language else { return false }
        switch language.lowercased() {
        case "math", "latex", "tex", "katex":
            return true
        default:
            return false
        }
    }

    private func shouldRenderAsMath(code: String) -> Bool {
        if code.isEmpty {
            return false
        }

        // Full LaTeX document snippets should remain code blocks.
        let lowered = code.lowercased()
        let documentMarkers = [
            "\\documentclass",
            "\\begin{document}",
            "\\end{document}",
            "\\usepackage",
            "\\maketitle",
            "\\section",
            "\\subsection",
            "\\paragraph",
            "\\title{",
            "\\author{",
            "\\date{",
        ]
        if documentMarkers.contains(where: { lowered.contains($0) }) {
            return false
        }

        return true
    }

    private func normalizedMathFenceContent(_ code: String) -> String {
        let trimmed = code.trimmingCharacters(in: .whitespacesAndNewlines)

        if trimmed.hasPrefix("\\[") && trimmed.hasSuffix("\\]") {
            let start = trimmed.index(trimmed.startIndex, offsetBy: 2)
            let end = trimmed.index(trimmed.endIndex, offsetBy: -2)
            if start <= end {
                return String(trimmed[start..<end]).trimmingCharacters(in: .whitespacesAndNewlines)
            }
        }

        if trimmed.hasPrefix("$$") && trimmed.hasSuffix("$$") {
            let start = trimmed.index(trimmed.startIndex, offsetBy: 2)
            let end = trimmed.index(trimmed.endIndex, offsetBy: -2)
            if start <= end {
                return String(trimmed[start..<end]).trimmingCharacters(in: .whitespacesAndNewlines)
            }
        }

        return trimmed
    }

    private func splitMath(in text: String) -> [MessageSegment] {
        var output: [MessageSegment] = []
        var i = text.startIndex
        var buffer = ""

        func flushBuffer() {
            if !buffer.isEmpty {
                output.append(.markdownText(buffer))
                buffer = ""
            }
        }

        while i < text.endIndex {
            if text[i] == "\\" {
                let next = text.index(after: i)
                if next < text.endIndex {
                    // Support TeX delimiter forms: \( ... \) and \[ ... \].
                    if text[next] == "(" || text[next] == "[" {
                        let isBlock = text[next] == "["
                        let contentStart = text.index(after: next)
                        let closePattern = isBlock ? "\\]" : "\\)"
                        if let close = text[contentStart...].range(of: closePattern)?.lowerBound {
                            flushBuffer()
                            let tex = String(text[contentStart..<close]).trimmingCharacters(in: .whitespacesAndNewlines)
                            if !tex.isEmpty {
                                let key = MathCacheKey.make(tex: tex, block: isBlock)
                                output.append(isBlock ? .blockMath(tex: tex, key: key) : .inlineMath(tex: tex, key: key))
                            }
                            i = text.index(close, offsetBy: 2, limitedBy: text.endIndex) ?? text.endIndex
                            continue
                        }
                    }
                    buffer.append(text[i])
                    buffer.append(text[next])
                    i = text.index(after: next)
                    continue
                }
            }

            if text[i] == "$" {
                let next = text.index(after: i)
                let isBlock = next < text.endIndex && text[next] == "$"
                let start = isBlock ? text.index(after: next) : next
                if let close = findMathClose(in: text, from: start, block: isBlock) {
                    flushBuffer()
                    let tex = String(text[start..<close]).trimmingCharacters(in: .whitespacesAndNewlines)
                    if !tex.isEmpty {
                        let key = MathCacheKey.make(tex: tex, block: isBlock)
                        output.append(isBlock ? .blockMath(tex: tex, key: key) : .inlineMath(tex: tex, key: key))
                    }
                    i = isBlock ? text.index(close, offsetBy: 2) : text.index(after: close)
                    continue
                }
            }

            buffer.append(text[i])
            i = text.index(after: i)
        }

        flushBuffer()
        return output
    }

    private func findMathClose(in text: String, from start: String.Index, block: Bool) -> String.Index? {
        var i = start
        while i < text.endIndex {
            if text[i] == "\\" {
                i = text.index(i, offsetBy: 2, limitedBy: text.endIndex) ?? text.endIndex
                continue
            }

            if text[i] == "$" {
                if block {
                    let next = text.index(after: i)
                    if next < text.endIndex && text[next] == "$" {
                        return i
                    }
                } else {
                    return i
                }
            }

            i = text.index(after: i)
        }
        return nil
    }
}
