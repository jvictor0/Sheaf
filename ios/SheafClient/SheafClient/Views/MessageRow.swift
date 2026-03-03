import SwiftUI

struct MessageRow: View {
    let message: RenderedMessage

    var body: some View {
        HStack {
            if message.role == .assistant || message.role == .system {
                bubble
                Spacer(minLength: 32)
            } else {
                Spacer(minLength: 32)
                bubble
            }
        }
    }

    private var bubble: some View {
        VStack(alignment: .leading, spacing: 8) {
            ForEach(Array(message.segments.enumerated()), id: \.offset) { _, segment in
                switch segment {
                case .markdownText(let text):
                    MarkdownText(text: text)
                case .codeBlock(let language, let text):
                    CodeBlock(language: language, code: text)
                case .inlineMath(let tex, _):
                    MathFormulaView(tex: tex, block: false)
                        .frame(minHeight: 20)
                case .blockMath(let tex, _):
                    MathFormulaView(tex: tex, block: true)
                        .frame(minHeight: 40)
                }
            }
        }
        .padding(12)
        .background(message.role == .user ? Color.blue.opacity(0.12) : Color.gray.opacity(0.14))
        .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
        .frame(maxWidth: 680, alignment: .leading)
    }
}

private struct MarkdownText: View {
    let text: String

    var body: some View {
        if let attr = try? AttributedString(markdown: text, options: .init(interpretedSyntax: .full)) {
            Text(attr)
                .textSelection(.enabled)
        } else {
            Text(text)
                .textSelection(.enabled)
        }
    }
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
