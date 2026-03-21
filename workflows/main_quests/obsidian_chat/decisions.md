# Decisions

## 2026-03-21 - Polish chat error cleanup and conflict recovery

- Stage: `polishing`
- Decision: Preserve websocket `error` frame metadata (`queue_id`, `fatal`) in the chat transport layer so the service can distinguish fatal queue failures from generic transport noise.
- Decision: Route `execution_conflict` recovery through the existing cancellable reconnect path instead of a raw `setTimeout`, so leaving the thread or pane reliably cancels delayed re-entry.
- Implementation Time: `~25 minutes`

## 2026-03-21 - Polish queue-scoped cleanup and replay send gating

- Stage: `polishing`
- Decision: Track `queueID` on pending sends once `durable_ack` arrives so fatal queue errors can remove only the affected pending and streaming artifacts instead of wiping unrelated in-flight work.
- Decision: Treat the composer as unavailable until the session reaches `live`, and reject service-level sends during `connecting` or `replaying` so replay can establish the authoritative tail before new user input is submitted.
- Implementation Time: `~30 minutes`

- 2026-03-21: This main quest plans an additive chat feature inside the existing `apps/obsidian-replica` plugin rather than a separate Obsidian plugin.
- 2026-03-21: The feature target is iOS chat parity where practical, so the planning documents treat the Python server chat transport and the iOS chat client structures as the canonical references.
- 2026-03-21: The websocket contract is documented from current implementation in `src/sheaf/server/app.py`, `src/sheaf/server/runtime.py`, and the iOS client transport code rather than from older roadmap summaries alone.
- 2026-03-21: The first implementation pass keeps chat rendering intentionally simple: plain text-oriented bubbles, incremental token streaming, lightweight activity indicators, and no advanced markdown or LaTeX parity requirement.
- 2026-03-21: Tool-call rendering for file-oriented tools must be privacy-preserving in the Obsidian pane by showing only the filename involved, never file contents and never expanded tool arguments.
- 2026-03-21: The chat feature should use its own thread/chat transport flow and must not overload the replica sync websocket protocol.
- 2026-03-21: Model configuration for chat should be reached through a gear icon that takes the user to plugin configuration rather than an inline v1 model selector inside the pane.
- 2026-03-21: The thread list should refresh automatically every time the user returns to or otherwise displays the list view.
- 2026-03-21: Chat session caching should remain memory-only in v1 so conversation state is always refreshed from the server after reload or reconnect.
- 2026-03-21: The implementation uses a dedicated `src/chat/` module split with protocol decoding, REST client, websocket transport, state store, service orchestration, tool summarization, and the Obsidian `ItemView`.
- 2026-03-21: The chat pane disconnects its websocket when leaving the conversation screen or closing the pane, then reconnects from `lastCommittedTurnID` on re-entry for simpler mobile-safe lifecycle behavior.
- 2026-03-21: Thinking and operational `turn_event` frames drive lightweight status text and activity indicators only; committed transcript content still comes solely from committed turns and streaming assistant buffers.
- 2026-03-21: During polishing, the user approved a spec refinement for chat-pane rendering: heartbeats and other non-visible transport frames should not trigger full-pane rerenders, committed rows should remain mounted once committed during normal updates, the composer should stay mounted and focused, and auto-scroll should apply only when new visible transcript content arrives while the user is already near the bottom.
- 2026-03-21: The implementation-time expectation for this refinement is an incremental renderer that updates the transcript, status region, and streaming row independently instead of clearing and rebuilding the entire chat view on every store emission.
