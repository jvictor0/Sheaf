# Sheaf Product and Technical Spec

## 1. Agent identity

- Name: `sheaf`
- Pronouns: they/them
- Runtime model: server-based conversational agent

## 2. Core API requirements

The server exposes these APIs:

1. `POST /chats`
- Purpose: create a new chat session
- Result: returns a unique `chat_id`
- Persistence note: chat directories are created lazily on first message

2. `POST /chats/{chat_id}/messages`
- Purpose: continue an existing conversation by sending the next message
- Input: user message payload
- Result: returns assistant response and metadata (`chat_id`, `checkpoint_id`, `tool_calls`)

3. `GET /chats`
- Purpose: list existing chats
- Result: collection of chat descriptors (`chat_id`, created/updated timestamps)

4. `GET /chats/{chat_id}/metadata`
- Purpose: fetch chat metadata for UI/session restore
- Result: metadata including `message_count` and `latest_checkpoint_id`

5. `GET /chats/{chat_id}/messages?start=<int>&end=<int>`
- Purpose: fetch a bounded message slice
- Result: ordered message list with zero-based indices (`start` inclusive, `end` exclusive), including per-assistant `tool_calls`

## 3. Conversation and context model

- Context is scoped per chat.
- No context sharing across different chats.
- Each chat persists its own transcript and metadata.
- Checkpoints are stored under each chat directory.
- Messages are indexed per chat as zero-based positions in the transcript.
- LangGraph manages runtime state checkpoints (SQLite checkpointer per chat thread).
- Runtime state supports context compaction via rolling summary fields when token budget is high.

## 4. LLM dispatch architecture

- LLM calls are isolated behind an abstract dispatcher interface.
- Non-LLM modules do not call provider SDKs directly.
- Current concrete provider: OpenAI `gpt-4.1-mini`.
- Design supports future provider/model swaps without changing API handlers.

## 5. Secrets and provider configuration

- Configuration is read from `sheaf_server.config` only.
- Secrets are read from the configured `secrets_file` path (default `.secrets.json`, gitignored).
- `.secrets.example.json` documents multi-provider shape.
- `.secrets.json` must never be committed.

## 6. Filesystem tool capabilities (planned, restricted)

The agent will have constrained access to an allowlisted set of directories.

Current implementation:
- Write or overwrite files inside `tome_dir/**` via `write_note` tool
- List directories/files inside `tome_dir/**` via `list_notes` tool
- Read files (full or line-range) inside `tome_dir/**` via `read_note` tool

Planned next actions:
- Expand allowlist beyond `tome_dir`

Guardrails:
- Deny access outside allowlist
- Validate and normalize paths before access
- Emit clear errors for unauthorized paths or invalid ranges

## 7. Storage layout

- Runtime data directory: `data/` (tracked, contents ignored)
- Per chat:
  - `data/chats/<chat_id>/chat.json`
  - `data/chats/<chat_id>/checkpoints/langgraph.sqlite`

## 8. Interface

- Primary interface: HTTP server APIs
- Secondary interface: simple Python CLI loop that creates/selects chat and sends messages

## 9. Delivery approach

Implementation proceeds in phased increments (see `docs/EXECUTION_PLAN.md`).
