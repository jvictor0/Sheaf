# Sheaf Execution Plan

## Objective

Deliver a Python-based conversational agent server with per-chat persistence, scoped filesystem tools, and CLI interaction, implemented in incremental phases.

## Current baseline (completed)

- FastAPI server with:
  - `POST /chats`
  - `GET /chats`
  - `POST /chats/{chat_id}/messages`
- Per-chat on-disk checkpoint persistence under `data/chats/<chat_id>/checkpoints/`
- Dedicated LLM dispatcher abstraction (`LLMDispatcher`) and provider implementation (`LangChainOpenAIDispatcher`)
- Secrets policy in place with gitignored `.secrets.json` and tracked `.secrets.example.json`

## Phase 1: Minimal conversational graph + per-chat disk persistence

Scope:
- Replace direct chat orchestration with a minimal LangGraph state graph.
- Keep current endpoint contracts and per-chat isolation.
- Keep checkpoint snapshots per chat while introducing graph execution state.

Acceptance criteria:
- New chat creation returns unique `chat_id`.
- Message continuation uses only that chat's own context.
- Restarting server preserves existing chats from disk.
- Listing chats returns previously created sessions.

## Phase 2: Context summarization for long conversations

Scope:
- Introduce token/message budget thresholds.
- Summarize older turns when threshold exceeded.
- Keep recent turns verbatim plus rolling summary per chat.

Current status:
- Initial implementation in place:
  - `ModelProperties` + `ModelLimits` source-of-truth per provider/model.
  - LangGraph `maybe_compact` node before assistant turn.
  - Rolling summary stored in checkpoint state.

Acceptance criteria:
- Chat continuity is preserved after summarization.
- Context size remains bounded under sustained usage.
- Summary data is persisted per chat.

## Phase 3: Controlled file write capability

Scope:
- Add write/overwrite tool with allowlisted directories.
- Route explicit agent commands to write tool.
- Persist write operations in chat history (audit trail).

Current status:
- Initial write tool enabled:
  - `write_note(relative_path, content, overwrite=True)`
  - allowlist rooted at `data/notes/**`

Acceptance criteria:
- Writes succeed only within allowed directories.
- Attempts outside allowlist are rejected with explicit errors.
- Written files are visible on disk and referenced in responses.

## Phase 4: Controlled file read capability

Scope:
- Add full-file read and line-range read tools.
- Support path validation and line-range validation.
- Integrate read outputs into chat responses.

Current status:
- Initial read/list tool support enabled under `data/notes/**`:
  - `read_note(relative_path, start_line=0, end_line=0)`
  - `list_notes(relative_dir='.', recursive=False)`

Acceptance criteria:
- Reads return exact content requested (or precise error).
- Line-range reads handle bounds correctly.
- Unauthorized paths are denied.

## Phase 5: Intelligent directory navigation and search

Scope:
- Add directory listing and constrained search/navigation helpers.
- Let agent discover relevant files in allowed directories.
- Improve tool-use policy for multi-step exploration.

Acceptance criteria:
- Agent can find and reference relevant files efficiently.
- Navigation/search remains constrained to allowlist.
- Conversation quality improves on file-centric tasks.

## Cross-cutting engineering tasks (all phases)

- Add tests per phase (unit + API integration).
- Maintain schema/versioning for persisted chat data.
- Add logging and structured error responses.
- Keep interfaces stable and backward-compatible.
