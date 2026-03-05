import SwiftUI

struct MarkdownBlocksView: View {
    private let blocks: [MarkdownBlock]

    init(text: String) {
        self.blocks = MarkdownBlockParser().parse(text)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ForEach(Array(blocks.enumerated()), id: \.offset) { _, block in
                blockView(for: block)
            }
        }
        .textSelection(.enabled)
    }

    @ViewBuilder
    private func blockView(for block: MarkdownBlock) -> some View {
        switch block {
        case .heading(let level, let text):
            InlineMarkdownText(text: text)
                .font(headingFont(level: level))
                .fontWeight(.semibold)
                .padding(.top, level <= 2 ? 6 : 2)
        case .paragraph(let text):
            InlineMarkdownText(text: text)
                .fixedSize(horizontal: false, vertical: true)
        case .unorderedList(let items):
            VStack(alignment: .leading, spacing: 4) {
                ForEach(Array(items.enumerated()), id: \.offset) { _, item in
                    HStack(alignment: .top, spacing: 8) {
                        Text("•")
                            .font(.body)
                        InlineMarkdownText(text: item)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
            }
        case .orderedList(let items):
            VStack(alignment: .leading, spacing: 4) {
                ForEach(Array(items.enumerated()), id: \.offset) { index, item in
                    HStack(alignment: .top, spacing: 8) {
                        Text("\(index + 1).")
                            .font(.body.monospacedDigit())
                        InlineMarkdownText(text: item)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
            }
        case .table(let headers, let rows):
            MarkdownTableView(headers: headers, rows: rows)
        case .quote(let text):
            HStack(alignment: .top, spacing: 10) {
                Rectangle()
                    .fill(Color.secondary.opacity(0.35))
                    .frame(width: 3)
                MarkdownBlocksView(text: text)
            }
            .padding(.vertical, 2)
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

private struct InlineMarkdownText: View {
    let text: String

    var body: some View {
        if let attr = try? AttributedString(
            markdown: text,
            options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace)
        ) {
            Text(attr)
        } else {
            Text(text)
        }
    }
}

private struct MarkdownTableView: View {
    let headers: [String]
    let rows: [[String]]

    private var columnCount: Int {
        max(headers.count, rows.map(\.count).max() ?? 0)
    }

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            VStack(alignment: .leading, spacing: 0) {
                rowView(cells: normalized(headers), isHeader: true)
                ForEach(Array(rows.enumerated()), id: \.offset) { _, row in
                    rowView(cells: normalized(row), isHeader: false)
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
    private func rowView(cells: [String], isHeader: Bool) -> some View {
        HStack(spacing: 0) {
            ForEach(Array(cells.enumerated()), id: \.offset) { index, cell in
                InlineMarkdownText(text: cell)
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

    private func normalized(_ row: [String]) -> [String] {
        guard columnCount > 0 else { return [] }
        if row.count == columnCount {
            return row
        }
        if row.count < columnCount {
            return row + Array(repeating: "", count: columnCount - row.count)
        }
        return Array(row.prefix(columnCount))
    }
}
