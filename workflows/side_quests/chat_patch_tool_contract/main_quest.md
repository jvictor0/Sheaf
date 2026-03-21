# Main Quest

- Main Quest: `obsidian_chat`
- Path: `../../main_quests/obsidian_chat/`
- Reason Split Out: Clarify and possibly redesign the chat editing tool contract so agents stop sending incompatible patch formats and tool-event UX can rely on consistent file-operation behavior.
- Source Issue: `OpenAI-style agents are emitting patch envelopes that do not match the server's current unified-diff-only apply_patch contract, causing repeated failed tool calls in chat transcripts.`
