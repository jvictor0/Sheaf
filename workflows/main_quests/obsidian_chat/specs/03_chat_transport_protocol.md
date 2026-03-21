# Obsidian Chat Transport Protocol

## Scope

This document records the chat protocol that the Obsidian pane must implement.
It is based on the live server behavior in `src/sheaf/server/app.py`,
`src/sheaf/server/runtime.py`, and the current iOS client transport/models.

The goal is to avoid implementation-time reverse engineering and to preserve
behavioral parity with the iOS app wherever practical.

## Transport Overview

The chat flow is a REST-plus-websocket protocol:

1. fetch or create a thread over REST
2. enter chat for a chosen thread over REST
3. connect to a websocket session for replay and live streaming
4. send user messages over the websocket
5. receive committed turns, streamed assistant tokens, and control events

This chat websocket is distinct from the Obsidian replica websocket used for
file synchronization.

The plugin is expected to treat them as parallel streams:

- chat traffic uses `WS /ws/chat/{session_id}`
- replica sync traffic uses `WS /ws/replica/{session_id}`
- both connections may exist concurrently without sharing message types or
  delivery state

## Protocol Version

- Current version: `1`
- The client sends `protocol_version` during `enter-chat` and websocket message
  submission.
- A mismatch on `enter-chat` is an HTTP `409`.
- A mismatch on websocket submission yields an `error` frame.

## Thread REST APIs

### `GET /threads`

Returns non-archived threads.

Expected response shape:

```json
{
  "threads": [
    {
      "thread_id": "uuid",
      "name": "Thread name",
      "prev_thread_id": null,
      "start_turn_id": null,
      "is_archived": false,
      "tail_turn_id": "uuid-or-null",
      "created_at": "iso8601",
      "updated_at": "iso8601"
    }
  ]
}
```

The Obsidian client should preserve this shape closely even if its local UI
also derives a simpler thread-summary type.

### `POST /threads`

Creates a new thread.

Accepted request fields from the server implementation:

- `name`
- `thread_id`
- `prev_thread_id`
- `start_turn_id`

Minimal creation request for the Obsidian pane:

```json
{
  "name": "New thread"
}
```

Response shape:

```json
{
  "thread_id": "uuid"
}
```

## Enter Chat REST API

### `POST /threads/{thread_id}/enter-chat`

Request body:

```json
{
  "protocol_version": 1,
  "known_tail_turn_id": "uuid-or-null"
}
```

Behavior:

- `known_tail_turn_id` is the client's last durable committed turn for that
  thread
- `null` or omission means the client has no known durable tail

Response body:

```json
{
  "session_id": "uuid",
  "websocket_url": "/ws/chat/{session_id}",
  "accepted_protocol_version": 1
}
```

## Websocket Connection

### Endpoint

- `WS /ws/chat/{session_id}`

### Unknown Session Handling

If the client opens a websocket for an unknown session ID:

- the server sends an `error` frame
- the websocket closes with code `1008`

## Frame Envelope

Every server-to-client frame includes the common envelope:

```json
{
  "protocol_version": 1,
  "type": "frame_type",
  "session_id": "uuid",
  "server_time": "iso8601"
}
```

Each frame then adds type-specific payload fields at the top level.

## Handshake Sequence

On successful websocket connect, the server performs:

1. `handshake_snapshot_begin`
2. zero or more `committed_turn` frames from history replay
3. one `context_budget` frame
4. draining of outstanding queued work for the thread
5. `handshake_ready`

### Replay Rule

If `known_tail_turn_id` is supplied:

- the server walks backward from the thread tail
- if that turn is found in the chain, it replays all turns after it
- if it is not found, the server falls back to replaying the last 20 turns

If `known_tail_turn_id` is absent:

- the server replays the last 20 turns

## Client To Server Websocket Frame

### `submit_message`

Request shape:

```json
{
  "protocol_version": 1,
  "type": "submit_message",
  "thread_id": "uuid",
  "text": "Hello",
  "model_name": "model-name-or-empty",
  "in_response_to_turn_id": "uuid-or-null",
  "client_message_id": "uuid"
}
```

Notes:

- `thread_id` defaults to the session thread if omitted or wrong on the server
  side, but the client should always send the correct thread ID
- `model_name` may be empty, in which case the server resolves the default model
- `in_response_to_turn_id` should be the current `lastCommittedTurnID`
- `client_message_id` is client-generated and used to correlate pending sends

## Server To Client Frame Types

### `handshake_snapshot_begin`

Payload:

```json
{
  "thread_id": "uuid"
}
```

Client behavior:

- begin authoritative transcript reload for the thread
- clear stale uncommitted state before rebuilding from replay

### `committed_turn`

Payload:

```json
{
  "turn": {
    "id": "uuid",
    "thread_id": "uuid",
    "prev_turn_id": "uuid-or-null",
    "speaker": "user|assistant|system",
    "message_text": "text",
    "model_name": "model-or-null",
    "created_at": "iso8601-or-null",
    "tool_calls": [
      {
        "id": "tool-call-id",
        "name": "tool_name",
        "args": {},
        "result": "raw tool result string",
        "is_error": false
      }
    ]
  }
}
```

Rules:

- user and assistant turns are the primary chat transcript entries
- assistant turns may include `tool_calls`
- `tool_calls` are sourced from the turn's stored `stats_json`

### `context_budget`

Payload:

```json
{
  "context_size": 1234,
  "max_context_size": 8192
}
```

Usage:

- informational metadata for UI or diagnostics
- not required to render in the transcript
- may be used later for subtle context indicators if desired

### `handshake_ready`

Payload:

```json
{}
```

Meaning:

- replay and startup drain are complete
- live chat interaction is ready

### `message_durable_ack`

Payload:

```json
{
  "queue_id": 42,
  "client_message_id": "uuid-or-null"
}
```

Meaning:

- the request has been durably written to the message queue
- it has not yet been committed as transcript turns

### `assistant_token`

Payload:

```json
{
  "queue_id": 42,
  "chunk": "partial text"
}
```

Rules:

- append by `queue_id`
- chunks are arbitrary string fragments, not guaranteed lexical tokens
- the client should concatenate them exactly in arrival order

### `turn_event`

Observed payload families:

```json
{
  "queue_id": 42,
  "event": "execution_started"
}
```

```json
{
  "queue_id": 42,
  "event": "retry_scheduled",
  "attempt": 2,
  "retry_in_seconds": 1.0,
  "error": "message"
}
```

```json
{
  "queue_id": 42,
  "event": "context_compaction",
  "payload": {
    "original_context_size": 12000,
    "compacted_context_size": 5000,
    "max_context_size": 8192
  }
}
```

```json
{
  "queue_id": 42,
  "event": "thinking_trace",
  "trace": "streamed_25_chunks",
  "trace_kind": "operational|model_reasoning"
}
```

Obsidian UI rule:

- parse these events for state, debugging, and indicators
- do not render full thinking traces in the transcript
- treat them as progress/control metadata, not user-facing conversation text

### `turn_finalized`

Payload:

```json
{
  "queue_id": 42,
  "turn_id": "assistant-turn-uuid"
}
```

Meaning:

- the assistant turn for the queue item has been committed
- remove the streaming buffer for that queue ID once the committed turn is in
  place

### `execution_conflict`

Payload:

```json
{
  "queue_id": 42,
  "expected_tail_turn_id": "uuid-or-null",
  "actual_tail_turn_id": "uuid-or-null"
}
```

Meaning:

- the client's `in_response_to_turn_id` was stale
- the queued request was discarded rather than committed

Client behavior:

- drop pending and streaming artifacts that depended on the stale tail
- reconnect using the latest durable `lastCommittedTurnID`
- let handshake replay restore server truth

### `heartbeat`

Payload:

```json
{
  "interval_seconds": 15
}
```

Meaning:

- keepalive and liveness signal only

### `error`

Observed payload shapes:

```json
{
  "message": "error text"
}
```

```json
{
  "queue_id": 42,
  "message": "error text"
}
```

```json
{
  "queue_id": 42,
  "message": "error text",
  "fatal": true
}
```

Client behavior:

- surface the message in UI state
- clear indefinite loading indicators when the error ends useful progress
- treat `fatal: true` as a terminal failure for the queued send

## Committed Turn Model

The Obsidian implementation should mirror the iOS `CommittedTurn` shape:

- `id`
- `thread_id`
- `prev_turn_id`
- `speaker`
- `message_text`
- `model_name`
- `created_at`
- `tool_calls`

## Tool Call Payload

Current assistant-turn tool call payload shape:

- `id`
- `name`
- `args`
- `result`
- `is_error`

Important UI rule for Obsidian:

- retain the raw payload in memory if needed for deterministic rendering
- do not render raw `result` strings for file-oriented tools
- summarize file tools to filename-only user-facing labels

## Resume And Reconnect Contract

The client should always treat committed turns as the durable source of truth.

Resume algorithm:

1. track `lastCommittedTurnID` whenever a new committed turn arrives
2. on reconnect, call `enter-chat` with `known_tail_turn_id = lastCommittedTurnID`
3. clear untrusted local pending and streaming artifacts
4. rebuild transcript from replayed committed turns

## Ordering Guarantees The UI Should Assume

- committed turns are authoritative
- assistant token chunks are transient
- `turn_finalized` closes the active stream for a queue item
- execution conflicts invalidate local assumptions and require resync
- a queue durable ack does not mean the message was committed

## Known Contract Drift To Document

- The iOS model layer still contains metadata/message REST helpers that the live
  server does not implement.
- The current iOS event decoder ignores `turn_event` and `context_budget`, but
  the Obsidian implementation may use them for indicators and diagnostics.
- The current server sends `tool_calls` only on committed assistant turns, not
  as incremental dedicated websocket frames.

## Obsidian Implementation Requirement

The implementer should build the Obsidian chat transport directly against this
document and the live server, not against older roadmap shorthand alone.
