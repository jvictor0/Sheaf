# Decisions

- 2026-03-21: This main quest plans an additive chat feature inside the existing `apps/obsidian-replica` plugin rather than a separate Obsidian plugin.
- 2026-03-21: The feature target is iOS chat parity where practical, so the planning documents treat the Python server chat transport and the iOS chat client structures as the canonical references.
- 2026-03-21: The websocket contract is documented from current implementation in `src/sheaf/server/app.py`, `src/sheaf/server/runtime.py`, and the iOS client transport code rather than from older roadmap summaries alone.
- 2026-03-21: The first implementation pass keeps chat rendering intentionally simple: plain text-oriented bubbles, incremental token streaming, lightweight activity indicators, and no advanced markdown or LaTeX parity requirement.
- 2026-03-21: Tool-call rendering for file-oriented tools must be privacy-preserving in the Obsidian pane by showing only the filename involved, never file contents and never expanded tool arguments.
- 2026-03-21: The chat feature should use its own thread/chat transport flow and must not overload the replica sync websocket protocol.
- 2026-03-21: Model configuration for chat should be reached through a gear icon that takes the user to plugin configuration rather than an inline v1 model selector inside the pane.
- 2026-03-21: The thread list should refresh automatically every time the user returns to or otherwise displays the list view.
- 2026-03-21: Chat session caching should remain memory-only in v1 so conversation state is always refreshed from the server after reload or reconnect.
