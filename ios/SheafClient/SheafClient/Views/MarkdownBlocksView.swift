import SwiftUI

struct MarkdownBlocksView: View {
    let blocks: [RenderBlock]

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ForEach(Array(blocks.enumerated()), id: \.offset) { _, block in
                blockView(block)
            }
        }
        .textSelection(.enabled)
    }

    @ViewBuilder
    private func blockView(_ block: RenderBlock) -> some View {
        switch block {
        case .heading(let level, let content):
            InlineContentView(nodes: content)
                .font(headingFont(level: level))
                .fontWeight(.semibold)
                .padding(.top, level <= 2 ? 6 : 2)
        case .paragraph(let nodes):
            InlineContentView(nodes: nodes)
        case .unorderedList(let items):
            VStack(alignment: .leading, spacing: 4) {
                ForEach(Array(items.enumerated()), id: \.offset) { _, item in
                    HStack(alignment: .top, spacing: 8) {
                        Text("•")
                            .font(.body)
                        InlineContentView(nodes: item)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
            }
        case .orderedList(let start, let items):
            VStack(alignment: .leading, spacing: 4) {
                ForEach(Array(items.enumerated()), id: \.offset) { index, item in
                    HStack(alignment: .top, spacing: 8) {
                        Text("\(start + index).")
                            .font(.body.monospacedDigit())
                        InlineContentView(nodes: item)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
            }
        case .table(let headers, let rows):
            MarkdownTableView(headers: headers, rows: rows)
        case .quote(let blocks):
            HStack(alignment: .top, spacing: 10) {
                Rectangle()
                    .fill(Color.secondary.opacity(0.35))
                    .frame(width: 3)
                MarkdownBlocksView(blocks: blocks)
            }
            .padding(.vertical, 2)
        case .codeBlock(let language, let code):
            CodeBlock(language: language, code: code)
        case .mathBlock(let tex, _):
            ScrollView(.horizontal, showsIndicators: false) {
                MathFormulaView(tex: tex, block: true)
                    .frame(minHeight: 40, alignment: .leading)
            }
        case .thematicBreak:
            Divider()
                .padding(.vertical, 4)
        }
    }

    private func headingFont(level: Int) -> Font {
        switch level {
        case 1:
            return .title2
        case 2:
            return .title3
        case 3:
            return .headline
        default:
            return .body
        }
    }
}

private struct InlineContentView: View {
    let nodes: [InlineNode]

    private var fragments: [InlineFragment] {
        nodes.flatMap(InlineFragment.fromNode)
    }

    var body: some View {
        InlineWrappingLayout(spacing: 0) {
            ForEach(Array(fragments.enumerated()), id: \.offset) { _, fragment in
                fragmentView(fragment)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    @ViewBuilder
    private func fragmentView(_ fragment: InlineFragment) -> some View {
        switch fragment {
        case .text(let text, let style):
            InlineStyledText(text: text, style: style)
                .layoutValue(key: InlineWhitespaceKey.self, value: text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
        case .inlineCode(let value):
            Text(value)
                .font(.system(.body, design: .monospaced))
                .padding(.horizontal, 4)
                .padding(.vertical, 1)
                .background(Color.black.opacity(0.08))
                .clipShape(RoundedRectangle(cornerRadius: 4, style: .continuous))
                .fixedSize(horizontal: true, vertical: true)
        case .lineBreak:
            Color.clear
                .frame(width: 0, height: 0)
                .layoutValue(key: InlineBreakKey.self, value: true)
        case .mathInline(let tex, _):
            MathFormulaView(tex: tex, block: false)
                .frame(minHeight: 20)
                .fixedSize(horizontal: true, vertical: true)
        }
    }
}

private struct InlineStyledText: View {
    let text: String
    let style: InlineTextStyle

    var body: some View {
        switch style {
        case .plain:
            Text(text)
                .fixedSize(horizontal: true, vertical: true)
        case .emphasis:
            Text(text)
                .italic()
                .fixedSize(horizontal: true, vertical: true)
        case .strong:
            Text(text)
                .fontWeight(.semibold)
                .fixedSize(horizontal: true, vertical: true)
        case .link(let destination):
            Text(text)
                .foregroundStyle(.blue)
                .underline()
                .help(destination)
                .fixedSize(horizontal: true, vertical: true)
        }
    }
}

private enum InlineTextStyle: Hashable {
    case plain
    case emphasis
    case strong
    case link(destination: String)
}

private enum InlineFragment: Hashable {
    case text(String, style: InlineTextStyle)
    case inlineCode(String)
    case lineBreak
    case mathInline(tex: String, key: String)

    static func fromNode(_ node: InlineNode) -> [InlineFragment] {
        switch node {
        case .text(let value):
            return splitText(value, style: .plain)
        case .emphasis(let value):
            return splitText(value, style: .emphasis)
        case .strong(let value):
            return splitText(value, style: .strong)
        case .link(let text, let destination):
            return splitText(text, style: .link(destination: destination))
        case .inlineCode(let value):
            return [.inlineCode(value)]
        case .lineBreak:
            return [.lineBreak]
        case .mathInline(let tex, let key):
            return [.mathInline(tex: tex, key: key)]
        }
    }

    private static func splitText(_ text: String, style: InlineTextStyle) -> [InlineFragment] {
        guard !text.isEmpty else { return [] }
        var result: [InlineFragment] = []
        var i = text.startIndex

        func isPunctuationLike(_ c: Character) -> Bool {
            c.unicodeScalars.allSatisfy {
                CharacterSet.punctuationCharacters.contains($0) || CharacterSet.symbols.contains($0)
            }
        }

        while i < text.endIndex {
            let start = i
            let ch = text[i]

            if ch.isWhitespace {
                while i < text.endIndex, text[i].isWhitespace {
                    i = text.index(after: i)
                }
                result.append(.text(String(text[start..<i]), style: style))
                continue
            }

            if isPunctuationLike(ch) {
                i = text.index(after: i)
                result.append(.text(String(text[start..<i]), style: style))
                continue
            }

            while i < text.endIndex, !text[i].isWhitespace, !isPunctuationLike(text[i]) {
                i = text.index(after: i)
            }
            result.append(.text(String(text[start..<i]), style: style))
        }

        return result
    }
}

private struct MarkdownTableView: View {
    let headers: [[InlineNode]]
    let rows: [[[InlineNode]]]

    private var columnCount: Int {
        max(headers.count, rows.map(\.count).max() ?? 0)
    }

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            VStack(alignment: .leading, spacing: 0) {
                rowView(cells: normalize(headers), isHeader: true)
                ForEach(Array(rows.enumerated()), id: \.offset) { _, row in
                    rowView(cells: normalize(row), isHeader: false)
                }
            }
            .overlay(
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .stroke(Color.secondary.opacity(0.35), lineWidth: 1)
            )
            .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
        }
    }

    @ViewBuilder
    private func rowView(cells: [[InlineNode]], isHeader: Bool) -> some View {
        HStack(spacing: 0) {
            ForEach(Array(cells.enumerated()), id: \.offset) { index, cell in
                InlineContentView(nodes: cell)
                    .font(isHeader ? .subheadline.weight(.semibold) : .subheadline)
                    .frame(minWidth: 90, maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 6)
                    .background(isHeader ? Color.secondary.opacity(0.1) : Color.clear)

                if index < cells.count - 1 {
                    Rectangle()
                        .fill(Color.secondary.opacity(0.25))
                        .frame(width: 1)
                }
            }
        }
        .background(isHeader ? Color.secondary.opacity(0.06) : Color.clear)
        .overlay(alignment: .bottom) {
            Rectangle()
                .fill(Color.secondary.opacity(0.25))
                .frame(height: 1)
        }
    }

    private func normalize(_ row: [[InlineNode]]) -> [[InlineNode]] {
        guard columnCount > 0 else { return [] }
        if row.count == columnCount {
            return row
        }
        if row.count < columnCount {
            return row + Array(repeating: [], count: columnCount - row.count)
        }
        return Array(row.prefix(columnCount))
    }
}

private struct InlineWrappingLayout: Layout {
    var spacing: CGFloat

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let width = max(1, resolveWidth(proposal: proposal, subviews: subviews))
        var x: CGFloat = 0
        var y: CGFloat = 0
        var lineHeight: CGFloat = 0

        for subview in subviews {
            if subview[InlineBreakKey.self] {
                y += max(lineHeight, 1)
                x = 0
                lineHeight = 0
                continue
            }
            if x == 0, subview[InlineWhitespaceKey.self] {
                continue
            }
            let size = subview.sizeThatFits(ProposedViewSize(width: width, height: nil))
            if size.width > width && x == 0 {
                y += lineHeight
                lineHeight = 0
            } else if x > 0 && (x + size.width) > width {
                y += lineHeight
                x = 0
                lineHeight = 0
            }
            x += size.width + spacing
            lineHeight = max(lineHeight, size.height)
        }

        return CGSize(width: width, height: max(1, y + lineHeight))
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        let width = max(1, bounds.width)
        var x = bounds.minX
        var y = bounds.minY
        var lineHeight: CGFloat = 0

        for subview in subviews {
            if subview[InlineBreakKey.self] {
                y += max(lineHeight, 1)
                x = bounds.minX
                lineHeight = 0
                continue
            }
            if x == bounds.minX, subview[InlineWhitespaceKey.self] {
                continue
            }
            let size = subview.sizeThatFits(ProposedViewSize(width: width, height: nil))
            if x > bounds.minX && (x + size.width) > (bounds.minX + width) {
                y += lineHeight
                x = bounds.minX
                lineHeight = 0
            }
            subview.place(
                at: CGPoint(x: x, y: y),
                anchor: .topLeading,
                proposal: ProposedViewSize(width: min(size.width, width), height: size.height)
            )
            x += size.width + spacing
            lineHeight = max(lineHeight, size.height)
        }
    }

    private func resolveWidth(proposal: ProposedViewSize, subviews: Subviews) -> CGFloat {
        if let width = proposal.width, width > 0 {
            return width
        }
        var total: CGFloat = 0
        for subview in subviews where !subview[InlineBreakKey.self] {
            total += subview.sizeThatFits(.unspecified).width + spacing
        }
        return total
    }
}

private struct InlineBreakKey: LayoutValueKey {
    static let defaultValue = false
}

private struct InlineWhitespaceKey: LayoutValueKey {
    static let defaultValue = false
}

private struct CodeBlock: View {
    let language: String?
    let code: String

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if let language {
                Text(language)
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
            }
            ScrollView(.horizontal, showsIndicators: false) {
                Text(code)
                    .font(.system(.body, design: .monospaced))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
            }
        }
        .padding(10)
        .background(Color.black.opacity(0.08))
        .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
    }
}
