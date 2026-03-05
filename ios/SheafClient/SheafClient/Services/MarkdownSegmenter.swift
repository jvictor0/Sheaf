import Foundation
import Markdown

struct MarkdownSegmenter {
    private enum SourceChunk {
        case markdown(String)
        case mathBlock(String)
    }

    func segmented(message: ChatMessage) -> RenderedMessage {
        let document = parseDocument(message.content)
        return RenderedMessage(
            id: message.id,
            role: MessageRole(rawRole: message.role),
            document: document,
            renderVersion: 3
        )
    }

    func parseDocument(_ text: String) -> RenderDocument {
        let chunks = splitSourceIntoChunks(text)
        var blocks: [RenderBlock] = []

        for chunk in chunks {
            switch chunk {
            case .mathBlock(let tex):
                blocks.append(.mathBlock(tex: tex, key: MathCacheKey.make(tex: tex, block: true)))
            case .markdown(let markdown):
                blocks.append(contentsOf: translateMarkdown(markdown))
            }
        }

        return RenderDocument(blocks: blocks)
    }

    private func splitSourceIntoChunks(_ text: String) -> [SourceChunk] {
        let normalized = text.replacingOccurrences(of: "\r\n", with: "\n")
        let lines = normalized.components(separatedBy: "\n")

        var chunks: [SourceChunk] = []
        var markdownBuffer: [String] = []
        var i = 0

        func flushMarkdownBuffer() {
            guard !markdownBuffer.isEmpty else { return }
            chunks.append(.markdown(markdownBuffer.joined(separator: "\n")))
            markdownBuffer.removeAll(keepingCapacity: true)
        }

        while i < lines.count {
            if let (tex, nextIndex) = parseMathFence(lines: lines, start: i) {
                flushMarkdownBuffer()
                chunks.append(.mathBlock(tex))
                i = nextIndex
                continue
            }

            if let (tex, nextIndex) = parseBracketMathBlock(lines: lines, start: i) {
                flushMarkdownBuffer()
                chunks.append(.mathBlock(tex))
                i = nextIndex
                continue
            }

            markdownBuffer.append(lines[i])
            i += 1
        }

        flushMarkdownBuffer()
        return chunks
    }

    private func parseMathFence(lines: [String], start: Int) -> (tex: String, nextIndex: Int)? {
        let opener = lines[start].trimmingCharacters(in: .whitespaces)
        guard opener.hasPrefix("```") else { return nil }

        let languageText = String(opener.dropFirst(3)).trimmingCharacters(in: .whitespaces)
        let language = languageText.isEmpty ? nil : languageText

        var i = start + 1
        var fenceBody: [String] = []
        while i < lines.count {
            if lines[i].trimmingCharacters(in: .whitespaces) == "```" {
                break
            }
            fenceBody.append(lines[i])
            i += 1
        }

        let body = fenceBody.joined(separator: "\n")
        guard isMathFence(language: language), shouldRenderAsMath(code: body) else {
            return nil
        }

        let tex = normalizeFenceMathCode(body)
        guard !tex.isEmpty else { return nil }
        return (tex, min(i + 1, lines.count))
    }

    private func parseBracketMathBlock(lines: [String], start: Int) -> (tex: String, nextIndex: Int)? {
        let trimmed = lines[start].trimmingCharacters(in: .whitespaces)
        guard trimmed.hasPrefix("\\[") else { return nil }

        if trimmed.hasSuffix("\\]"), trimmed.count > 4 {
            let s = trimmed.index(trimmed.startIndex, offsetBy: 2)
            let e = trimmed.index(trimmed.endIndex, offsetBy: -2)
            let tex = String(trimmed[s..<e]).trimmingCharacters(in: .whitespacesAndNewlines)
            guard !tex.isEmpty else { return nil }
            return (tex, start + 1)
        }

        var i = start
        var collected: [String] = []
        var isFirst = true

        while i < lines.count {
            var line = lines[i]
            if isFirst, let r = line.range(of: "\\[") {
                line.removeSubrange(r)
            }

            if let close = line.range(of: "\\]") {
                let pre = String(line[..<close.lowerBound])
                if !pre.isEmpty {
                    collected.append(pre)
                }
                let tex = collected.joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
                guard !tex.isEmpty else { return nil }
                return (tex, i + 1)
            }

            collected.append(line)
            i += 1
            isFirst = false
        }

        return nil
    }

    private func translateMarkdown(_ markdown: String) -> [RenderBlock] {
        let tokenized = tokenizeInlineMath(in: markdown)
        let doc = Document(parsing: tokenized.markdown)
        return doc.children.compactMap { mapBlock($0, tokenMap: tokenized.tokenMap) }
    }

    private func mapBlock(_ node: Markup, tokenMap: [String: String]) -> RenderBlock? {
        if let heading = node as? Heading {
            return .heading(level: heading.level, content: mapInlineChildren(heading, tokenMap: tokenMap))
        }

        if let paragraph = node as? Paragraph {
            return .paragraph(mapInlineChildren(paragraph, tokenMap: tokenMap))
        }

        if let list = node as? UnorderedList {
            let items = list.children.compactMap { child -> [InlineNode]? in
                guard let item = child as? ListItem else { return nil }
                return mapListItem(item, tokenMap: tokenMap)
            }
            return .unorderedList(items)
        }

        if let list = node as? OrderedList {
            let items = list.children.compactMap { child -> [InlineNode]? in
                guard let item = child as? ListItem else { return nil }
                return mapListItem(item, tokenMap: tokenMap)
            }
            return .orderedList(start: Int(list.startIndex), items: items)
        }

        if let quote = node as? BlockQuote {
            let blocks = quote.children.compactMap { child in
                mapBlock(child, tokenMap: tokenMap)
            }
            return .quote(blocks)
        }

        if node is ThematicBreak {
            return .thematicBreak
        }

        if let code = node as? CodeBlock {
            let language = code.language?.isEmpty == true ? nil : code.language
            return .codeBlock(language: language, text: code.code)
        }

        if let html = node as? HTMLBlock {
            return .paragraph([.text(html.rawHTML)])
        }

        if let table = node as? Table {
            return mapTable(table, tokenMap: tokenMap)
        }

        let text = textContent(of: node).trimmingCharacters(in: CharacterSet.whitespacesAndNewlines)
        if text.isEmpty {
            return nil
        }
        return .paragraph(splitInlineMath(from: text, tokenMap: tokenMap))
    }

    private func mapTable(_ table: Table, tokenMap: [String: String]) -> RenderBlock {
        var headers: [[InlineNode]] = []
        var rows: [[[InlineNode]]] = []

        for child in table.children {
            if let head = child as? Table.Head {
                for row in head.children {
                    guard let headerRow = row as? Table.Row else { continue }
                    headers = headerRow.children.compactMap { cell in
                        guard let cell = cell as? Table.Cell else { return nil }
                        return mapInlineChildren(cell, tokenMap: tokenMap)
                    }
                }
            } else if let body = child as? Table.Body {
                for row in body.children {
                    guard let row = row as? Table.Row else { continue }
                    let cells = row.children.compactMap { cell -> [InlineNode]? in
                        guard let cell = cell as? Table.Cell else { return nil }
                        return mapInlineChildren(cell, tokenMap: tokenMap)
                    }
                    rows.append(cells)
                }
            }
        }

        return .table(headers: headers, rows: rows)
    }

    private func mapListItem(_ item: ListItem, tokenMap: [String: String]) -> [InlineNode] {
        var output: [InlineNode] = []

        for (idx, child) in item.children.enumerated() {
            if let paragraph = child as? Paragraph {
                output.append(contentsOf: mapInlineChildren(paragraph, tokenMap: tokenMap))
            } else {
                let text = textContent(of: child).trimmingCharacters(in: CharacterSet.whitespacesAndNewlines)
                if !text.isEmpty {
                    output.append(contentsOf: splitInlineMath(from: text, tokenMap: tokenMap))
                }
            }

            if idx < item.childCount - 1 {
                output.append(.lineBreak)
            }
        }

        return output
    }

    private func mapInlineChildren(_ node: Markup, tokenMap: [String: String]) -> [InlineNode] {
        var output: [InlineNode] = []
        for child in node.children {
            output.append(contentsOf: mapInline(child, tokenMap: tokenMap))
        }
        return output
    }

    private func mapInline(_ node: Markup, tokenMap: [String: String]) -> [InlineNode] {
        if let text = node as? Markdown.Text {
            return splitInlineMath(from: text.string, tokenMap: tokenMap)
        }

        if node is SoftBreak || node is LineBreak {
            return [.lineBreak]
        }

        if let strong = node as? Strong {
            return stylizedInlineNodes(from: splitInlineMath(from: textContent(of: strong), tokenMap: tokenMap), strong: true)
        }

        if let emphasis = node as? Emphasis {
            return stylizedInlineNodes(from: splitInlineMath(from: textContent(of: emphasis), tokenMap: tokenMap), strong: false)
        }

        if let code = node as? InlineCode {
            return [.inlineCode(code.code)]
        }

        if let link = node as? Markdown.Link {
            let destination = link.destination ?? ""
            return [.link(text: textContent(of: link), destination: destination)]
        }

        var nested: [InlineNode] = []
        for child in node.children {
            nested.append(contentsOf: mapInline(child, tokenMap: tokenMap))
        }
        if !nested.isEmpty {
            return nested
        }

        let text = textContent(of: node)
        return text.isEmpty ? [] : splitInlineMath(from: text, tokenMap: tokenMap)
    }

    private func textContent(of node: Markup) -> String {
        if let text = node as? Markdown.Text {
            return text.string
        }
        var output = ""
        for child in node.children {
            output.append(textContent(of: child))
        }
        return output
    }

    private func stylizedInlineNodes(from nodes: [InlineNode], strong: Bool) -> [InlineNode] {
        nodes.map { node in
            switch node {
            case .text(let text):
                return strong ? .strong(text) : .emphasis(text)
            default:
                return node
            }
        }
    }

    private func splitInlineMath(from text: String, tokenMap: [String: String]) -> [InlineNode] {
        var nodes: [InlineNode] = []
        var buffer = ""
        var i = text.startIndex
        let tokens = tokenMap.keys.sorted { $0.count > $1.count }

        func flush() {
            guard !buffer.isEmpty else { return }
            nodes.append(.text(buffer))
            buffer = ""
        }

        while i < text.endIndex {
            var matchedToken: String?
            for token in tokens {
                if text[i...].hasPrefix(token) {
                    matchedToken = token
                    break
                }
            }
            if let token = matchedToken, let tex = tokenMap[token] {
                flush()
                nodes.append(.mathInline(tex: tex, key: MathCacheKey.make(tex: tex, block: false)))
                i = text.index(i, offsetBy: token.count, limitedBy: text.endIndex) ?? text.endIndex
                continue
            }

            if text[i] == "\\" {
                let next = text.index(after: i)
                if next < text.endIndex, text[next] == "(" {
                    let contentStart = text.index(after: next)
                    if let close = text[contentStart...].range(of: "\\)")?.lowerBound {
                        flush()
                        let tex = String(text[contentStart..<close]).trimmingCharacters(in: .whitespacesAndNewlines)
                        if !tex.isEmpty {
                            nodes.append(.mathInline(tex: tex, key: MathCacheKey.make(tex: tex, block: false)))
                        }
                        i = text.index(close, offsetBy: 2, limitedBy: text.endIndex) ?? text.endIndex
                        continue
                    }
                }
            }

            buffer.append(text[i])
            i = text.index(after: i)
        }

        flush()
        return nodes
    }

    private func tokenizeInlineMath(in markdown: String) -> (markdown: String, tokenMap: [String: String]) {
        var tokenMap: [String: String] = [:]
        var output = ""
        var i = markdown.startIndex
        var tokenIndex = 0
        var inFence = false

        while i < markdown.endIndex {
            if markdown[i...].hasPrefix("```") {
                inFence.toggle()
                output.append("```")
                i = markdown.index(i, offsetBy: 3, limitedBy: markdown.endIndex) ?? markdown.endIndex
                continue
            }

            if !inFence, markdown[i] == "\\" {
                let next = markdown.index(after: i)
                if next < markdown.endIndex, markdown[next] == "(" {
                    let contentStart = markdown.index(after: next)
                    if let close = markdown[contentStart...].range(of: "\\)")?.lowerBound {
                        let tex = String(markdown[contentStart..<close]).trimmingCharacters(in: .whitespacesAndNewlines)
                        if !tex.isEmpty {
                            let token = "@@SHEAF_MATH_\(tokenIndex)@@"
                            tokenMap[token] = tex
                            output.append(token)
                            tokenIndex += 1
                            i = markdown.index(close, offsetBy: 2, limitedBy: markdown.endIndex) ?? markdown.endIndex
                            continue
                        }
                    }

                    // Preserve literal "\\(" text when unclosed by double-escaping the slash for markdown parsing.
                    output.append("\\\\(")
                    i = markdown.index(after: next)
                    continue
                }
            }

            output.append(markdown[i])
            i = markdown.index(after: i)
        }

        return (output, tokenMap)
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
        if code.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            return false
        }
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
        return !documentMarkers.contains(where: { lowered.contains($0) })
    }

    private func normalizeFenceMathCode(_ code: String) -> String {
        let trimmed = code.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.hasPrefix("\\[") && trimmed.hasSuffix("\\]") && trimmed.count >= 4 {
            let start = trimmed.index(trimmed.startIndex, offsetBy: 2)
            let end = trimmed.index(trimmed.endIndex, offsetBy: -2)
            return String(trimmed[start..<end]).trimmingCharacters(in: .whitespacesAndNewlines)
        }
        return trimmed
    }
}
