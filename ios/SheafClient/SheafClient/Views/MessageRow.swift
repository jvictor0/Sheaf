import SwiftUI

struct MessageRow: View {
    let message: RenderedMessage

    var body: some View {
        Group {
            if message.role == .user {
                userRow
            } else {
                agentRow
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var userRow: some View {
        HStack {
            Spacer(minLength: 32)
            MarkdownBlocksView(blocks: message.document.blocks)
                .padding(12)
                .background(Color.blue.opacity(0.12))
                .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                .frame(maxWidth: 680, alignment: .leading)
        }
    }

    private var agentRow: some View {
        MarkdownBlocksView(blocks: message.document.blocks)
            .font(message.role == .toolEvent ? .callout : .body)
            .foregroundStyle(message.role == .toolEvent ? .secondary : .primary)
            .frame(maxWidth: 760, alignment: .leading)
    }
}
