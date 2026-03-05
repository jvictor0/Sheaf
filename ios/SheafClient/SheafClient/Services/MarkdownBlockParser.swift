import Foundation

enum MarkdownBlock: Hashable {
    case heading(level: Int, text: String)
    case paragraph(String)
    case unorderedList([String])
    case orderedList([String])
    case table(headers: [String], rows: [[String]])
    case quote(String)
    case thematicBreak
}

struct MarkdownBlockParser {
    func parse(_ text: String) -> [MarkdownBlock] {
        let normalized = text.replacingOccurrences(of: "\r\n", with: "\n")
        let lines = normalized.components(separatedBy: "\n")

        var blocks: [MarkdownBlock] = []
        var i = 0

        while i < lines.count {
            if lines[i].trimmingCharacters(in: .whitespaces).isEmpty {
                i += 1
                continue
            }

            if let heading = parseHeading(lines[i]) {
                blocks.append(heading)
                i += 1
                continue
            }

            if isThematicBreak(lines[i]) {
                blocks.append(.thematicBreak)
                i += 1
                continue
            }

            if isTableStart(lines: lines, at: i) {
                let table = parseTable(lines: lines, start: i)
                if let table {
                    blocks.append(table.block)
                    i = table.nextIndex
                    continue
                }
            }

            if isQuoteLine(lines[i]) {
                let quote = parseQuote(lines: lines, start: i)
                blocks.append(.quote(quote.text))
                i = quote.nextIndex
                continue
            }

            if let item = parseUnorderedListItem(lines[i]) {
                let list = parseUnorderedList(lines: lines, start: i, firstItem: item)
                blocks.append(.unorderedList(list.items))
                i = list.nextIndex
                continue
            }

            if let item = parseOrderedListItem(lines[i]) {
                let list = parseOrderedList(lines: lines, start: i, firstItem: item)
                blocks.append(.orderedList(list.items))
                i = list.nextIndex
                continue
            }

            let paragraph = parseParagraph(lines: lines, start: i)
            blocks.append(.paragraph(paragraph.text))
            i = paragraph.nextIndex
        }

        return blocks
    }

    private func parseHeading(_ line: String) -> MarkdownBlock? {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        guard trimmed.hasPrefix("#") else { return nil }
        let level = trimmed.prefix { $0 == "#" }.count
        guard (1...6).contains(level) else { return nil }
        let rest = trimmed.dropFirst(level).trimmingCharacters(in: .whitespaces)
        guard !rest.isEmpty else { return nil }
        return .heading(level: level, text: rest)
    }

    private func isThematicBreak(_ line: String) -> Bool {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return false }
        let chars = Array(trimmed)
        guard let first = chars.first, first == "-" || first == "*" || first == "_" else { return false }
        let nonSpace = chars.filter { $0 != " " }
        guard nonSpace.count >= 3 else { return false }
        return nonSpace.allSatisfy { $0 == first }
    }

    private func isTableStart(lines: [String], at index: Int) -> Bool {
        guard index + 1 < lines.count else { return false }
        let header = lines[index]
        let separator = lines[index + 1]
        return header.contains("|") && isTableSeparator(separator)
    }

    private func parseTable(lines: [String], start: Int) -> (block: MarkdownBlock, nextIndex: Int)? {
        guard start + 1 < lines.count else { return nil }
        let headers = parseTableCells(lines[start])
        guard !headers.isEmpty else { return nil }

        var rows: [[String]] = []
        var i = start + 2
        while i < lines.count {
            let line = lines[i]
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty || !line.contains("|") {
                break
            }

            var row = parseTableCells(line)
            if row.count < headers.count {
                row.append(contentsOf: Array(repeating: "", count: headers.count - row.count))
            } else if row.count > headers.count {
                row = Array(row.prefix(headers.count))
            }
            rows.append(row)
            i += 1
        }

        return (.table(headers: headers, rows: rows), i)
    }

    private func parseTableCells(_ line: String) -> [String] {
        var trimmed = line.trimmingCharacters(in: .whitespaces)
        if trimmed.hasPrefix("|") {
            trimmed.removeFirst()
        }
        if trimmed.hasSuffix("|") {
            trimmed.removeLast()
        }
        if trimmed.isEmpty {
            return []
        }
        return trimmed
            .split(separator: "|", omittingEmptySubsequences: false)
            .map { String($0).trimmingCharacters(in: .whitespaces) }
    }

    private func isTableSeparator(_ line: String) -> Bool {
        let cells = parseTableCells(line)
        guard !cells.isEmpty else { return false }
        return cells.allSatisfy { cell in
            guard !cell.isEmpty else { return false }
            let trimmed = cell.trimmingCharacters(in: .whitespaces)
            var content = trimmed
            if content.hasPrefix(":") {
                content.removeFirst()
            }
            if content.hasSuffix(":") {
                content.removeLast()
            }
            return !content.isEmpty && content.allSatisfy { $0 == "-" } && content.count >= 3
        }
    }

    private func isQuoteLine(_ line: String) -> Bool {
        line.trimmingCharacters(in: .whitespaces).hasPrefix(">")
    }

    private func parseQuote(lines: [String], start: Int) -> (text: String, nextIndex: Int) {
        var quoteLines: [String] = []
        var i = start

        while i < lines.count, isQuoteLine(lines[i]) {
            var trimmed = lines[i].trimmingCharacters(in: .whitespaces)
            trimmed.removeFirst()
            if trimmed.hasPrefix(" ") {
                trimmed.removeFirst()
            }
            quoteLines.append(trimmed)
            i += 1
        }

        return (quoteLines.joined(separator: "\n"), i)
    }

    private func parseUnorderedListItem(_ line: String) -> String? {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        guard let first = trimmed.first, first == "-" || first == "*" || first == "+" else { return nil }
        let afterMarker = trimmed.dropFirst()
        guard afterMarker.first == " " else { return nil }
        return afterMarker.trimmingCharacters(in: .whitespaces)
    }

    private func parseUnorderedList(lines: [String], start: Int, firstItem: String) -> (items: [String], nextIndex: Int) {
        var items = [firstItem]
        var i = start + 1

        while i < lines.count, let item = parseUnorderedListItem(lines[i]) {
            items.append(item)
            i += 1
        }

        return (items, i)
    }

    private func parseOrderedListItem(_ line: String) -> String? {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        var idx = trimmed.startIndex

        while idx < trimmed.endIndex, trimmed[idx].isNumber {
            idx = trimmed.index(after: idx)
        }
        guard idx > trimmed.startIndex, idx < trimmed.endIndex else { return nil }
        guard trimmed[idx] == "." || trimmed[idx] == ")" else { return nil }

        idx = trimmed.index(after: idx)
        guard idx < trimmed.endIndex, trimmed[idx] == " " else { return nil }

        let content = trimmed[idx...].trimmingCharacters(in: .whitespaces)
        return content.isEmpty ? nil : content
    }

    private func parseOrderedList(lines: [String], start: Int, firstItem: String) -> (items: [String], nextIndex: Int) {
        var items = [firstItem]
        var i = start + 1

        while i < lines.count, let item = parseOrderedListItem(lines[i]) {
            items.append(item)
            i += 1
        }

        return (items, i)
    }

    private func parseParagraph(lines: [String], start: Int) -> (text: String, nextIndex: Int) {
        var collected: [String] = []
        var i = start

        while i < lines.count {
            let line = lines[i]
            if line.trimmingCharacters(in: .whitespaces).isEmpty || isBlockStart(lines: lines, at: i) {
                break
            }
            collected.append(line)
            i += 1
        }

        if collected.isEmpty {
            return (lines[start], start + 1)
        }
        return (collected.joined(separator: "\n"), i)
    }

    private func isBlockStart(lines: [String], at index: Int) -> Bool {
        if parseHeading(lines[index]) != nil { return true }
        if isThematicBreak(lines[index]) { return true }
        if isTableStart(lines: lines, at: index) { return true }
        if isQuoteLine(lines[index]) { return true }
        if parseUnorderedListItem(lines[index]) != nil { return true }
        if parseOrderedListItem(lines[index]) != nil { return true }
        return false
    }
}
