# Obsidian Chat Client Experience And State

## Scope

This document defines the planned Obsidian pane UX and the client-side state
model for thread browsing, active chat, pending sends, token streaming, and
tool-call summaries.

## View Model

The chat pane has two primary screens:

- thread list screen
- active conversation screen

The pane starts in the thread list screen when opened fresh.

## Thread List Screen

### Required Elements

- list of threads returned by `GET /threads`
- thread title
- thread ID or stable secondary metadata for debugging
- updated-at hint when available
- create-new-thread control
- gear icon or equivalent settings affordance that opens configuration for model
  selection and related chat settings
- retry or refresh affordance for load failure

### Behavior

- opening the pane triggers a thread list fetch
- every return to the thread list triggers a fresh thread list fetch
- selecting a thread opens the conversation screen for that thread
- creating a new thread inserts through `POST /threads`, then enters that thread
- thread-list refresh is automatic rather than relying on stale cached data

### Empty State

If no threads exist:

- show a simple empty state
- provide a prominent new-thread action

## Conversation Screen

### Required Elements

- back control to return to thread list
- thread title or thread identifier in header
- message transcript area
- composer text area
- send-on-Enter behavior
- visible state for loading, reconnecting, and errors

### Message Ordering

Messages render in transcript order using committed turn order from the server,
with local pending and streaming artifacts layered on top in a predictable way.

Render order should follow the iOS model:

1. committed turns
2. local pending user sends not yet matched to committed user turns
3. in-progress streaming assistant messages keyed by queue ID

## Message Presentation

### User Messages

- render in bubble form
- align as local/user-originated content
- show plain text only in v1

### Assistant Messages

- render in a simple assistant style similar to ChatGPT
- support basic text layout only
- avoid advanced markdown, math, and rich formatting requirements in v1

### In-Progress Assistant Messages

- maintain a streaming text buffer while `assistant_token` frames arrive
- render that buffer inside an open in-progress assistant container
- include a subtle activity animation while streaming continues
- replace or refresh the in-progress rendering with the finalized committed
  assistant turn when `turn_finalized` and `committed_turn` processing complete

### Thinking Indicator

The user requested a minimal thinking treatment:

- show a small thinking icon while work is underway
- animate that icon only when at least one thinking token or thinking-trace
  signal has been received within a recent time window
- stop the animation after a short quiet period with no new thinking activity
- do not render the full reasoning trace
- treat reasoning or operational traces as internal progress signals, not
  transcript content

## Tool Call Rendering

### Placement

Tool-call events should render adjacent to the assistant turn they belong to,
similar to lightweight system or tool-event bubbles.

### Privacy Rule

For file-oriented tools, the rendered summary must show only the filename, not
the full file contents and not the expanded payload.

Expected examples:

- `Read note.md`
- `Wrote tasks.md`
- `Patched syncClient.ts`

Not allowed:

- file contents
- large tool argument dumps
- full patch bodies
- raw structured JSON payloads when a filename-only summary is possible

### Filename Extraction Rule

For tools that expose a path:

- if the path is inside the currently synced vault, show the path relative to
  the vault root rather than only the basename
- otherwise extract the basename from the most specific path-like argument
  available
- preferred fields include `relative_path`, `path`, or other tool-specific path
  keys introduced during implementation
- if no filename can be derived safely, fall back to a generic label such as
  `Read file`, `Wrote file`, or `Applied patch`

## In-Memory State Model

The Obsidian implementation should mirror the iOS chat view-model concepts as
closely as practical.

### Per Thread Summary

Equivalent to iOS `ChatSummary`:

- `thread_id`
- `name`
- `created_at`
- `updated_at`
- optionally `prev_thread_id`, `start_turn_id`, `tail_turn_id`, `is_archived`
  when retaining the raw server shape

### Active Conversation State

Equivalent to the iOS active-chat state:

- `committedTurns: CommittedTurn[]`
- `pendingSends: PendingSend[]`
- `streamingByQueue: Map<queue_id, StreamingAssistantTurn>`
- `lastCommittedTurnID: string | null`
- `lastFrameAt: timestamp`
- `errorMessage: string | null`
- transport/session connection status

### Cached Session State

Equivalent in spirit to iOS `ChatSessionStore.Session`:

- rendered messages for the thread
- chat metadata if used
- oldest loaded index
- newest loaded exclusive index
- has-more-older flag

For v1, this cache may remain in memory only.

Additional rule:

- do not persist rendered transcript UI state across plugin reloads
- treat the server as the refresh source whenever the pane is reopened or a
  thread is re-entered

## Data Types To Mirror

### `PendingSend`

Track:

- `clientMessageID`
- submitted text
- `responseToTurnID`
- local rendered message ID

### `StreamingAssistantTurn`

Track:

- `queueID`
- accumulated text buffer

### `CommittedTurn`

Track:

- `id`
- `thread_id`
- `prev_turn_id`
- `speaker`
- `message_text`
- `model_name`
- `created_at`
- `tool_calls`

## Rebuild Rules

The transcript renderer should be deterministic from state:

- append committed turns in order
- consume matching pending sends when the corresponding committed user turn
  arrives
- keep streaming assistant buffers separate until finalized
- drop stale pending or streaming artifacts when handshake reset or execution
  conflict requires resync

## Reconnect Behavior

When the transport becomes stale or disconnects:

- retain the last committed turn ID
- drop uncommitted local artifacts that can no longer be trusted
- reconnect using `known_tail_turn_id = lastCommittedTurnID`
- let handshake replay restore the authoritative transcript

## Thread Switching Behavior

When the user changes threads:

- disconnect the current websocket
- preserve the cached state for the old thread if session caching is kept
- open the selected thread using its last known committed tail ID if available
- do not mix streaming or pending state across threads

## New Thread Behavior

When the user starts a new thread:

1. create the thread with `POST /threads`
2. refresh or update the thread list
3. enter the new thread immediately
4. start with an empty committed transcript and no known tail turn ID

## Error Treatment

- thread-list errors should stay local to the list screen
- active-thread transport errors should surface in the conversation screen
- fatal send errors should not leave indefinite streaming indicators behind
- execution conflicts should trigger resync instead of preserving stale local UI
