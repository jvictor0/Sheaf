# Obsidian Chat Rollout And Validation

## Scope

This document turns the feature plan into an implementation sequence and a
validation checklist.

## Recommended Implementation Order

### Phase 1: Shared Chat Models And Protocol Adapter

Build the client-side foundations first:

- thread-summary model
- committed-turn model
- pending-send model
- streaming-assistant model
- websocket frame decoder
- REST wrappers for thread list, thread creation, and `enter-chat`
- reducer or state-application helpers for transport events

Deliverable:

- a testable chat client layer with no Obsidian-specific UI yet

### Phase 2: Chat Pane Shell And Thread List

Add the Obsidian view and basic navigation:

- register the chat pane
- open command for the pane
- thread list loading state
- error state
- empty state
- thread selection
- new-thread creation and immediate entry

Deliverable:

- users can open the pane, see threads, and enter a selected thread

### Phase 3: Conversation Transport And Replay

Wire the selected thread to the server transport:

- `enter-chat`
- websocket connect
- handshake replay handling
- committed-turn rendering
- back-to-list behavior
- thread switch disconnect and reconnect behavior

Deliverable:

- users can open a thread and see prior chat history from server replay

### Phase 4: Send Flow And Streaming

Implement live conversation behavior:

- composer input
- send on Enter
- pending-send state
- durable-ack handling
- streaming assistant token accumulation
- finalized-turn replacement of the streaming placeholder

Deliverable:

- users can send messages and watch assistant text stream in live

### Phase 5: Thinking, Tool Events, And Recovery

Add the remaining parity and resilience pieces:

- minimal thinking indicator
- streaming activity animation
- tool-call summary bubbles
- filename-only file-tool rendering
- execution-conflict resync
- stale-connection watchdog reconnect
- fatal error cleanup

Deliverable:

- the pane behaves like a durable streaming client rather than a demo transcript

### Phase 6: Polish And Mobile Validation

Finish with layout and verification:

- spacing and bubble cleanup
- pane reopen behavior
- thread-switch smoothness
- desktop and mobile-safe interaction review
- manual protocol edge-case validation

## Test Plan

### Unit-Level Validation

- decode `GET /threads` payloads
- decode `enter-chat` response payloads
- decode each websocket frame family
- apply replay and live events into deterministic client state
- consume pending sends correctly when a committed user turn arrives
- accumulate and clear streaming buffers by queue ID
- summarize tool calls to filename-only labels for file tools

### Integration-Level Validation

- create a thread from the pane and immediately enter it
- enter an existing thread and receive replayed history
- send a message and receive `message_durable_ack`
- receive incremental `assistant_token` chunks
- finalize the assistant message and remove the temporary streaming row
- switch threads while a previous thread has cached state
- reconnect using `known_tail_turn_id`

### Manual UX Validation

- pane opens to thread list
- back navigation from thread to list is obvious and reliable
- Enter sends from the composer as expected
- user bubbles and assistant bubbles are visually distinct
- a visible indicator appears while the assistant is thinking/streaming
- tool-call bubbles do not leak file contents
- error states are visible without destroying already committed transcript state

## Edge Cases To Validate Explicitly

- empty thread with no turns
- thread with more than 20 historical turns and no known tail
- reconnect with a valid `known_tail_turn_id`
- reconnect with an invalid or stale `known_tail_turn_id`
- `execution_conflict` after a local send
- fatal queue/send error
- websocket disconnect during streaming
- thread switch during or shortly after streaming

## Implementation Guardrails

- do not modify the server chat protocol unless the quest is explicitly expanded
- do not couple chat websocket code to replica websocket code
- do not block first delivery on advanced markdown or math rendering
- do not surface raw tool result bodies for file-oriented tools

## Exit Criteria For Implementation Stage

Before this quest can leave implementation:

- the Obsidian pane can list, open, switch, and create threads
- a selected thread can replay history through the live websocket handshake
- sending, durable ack, token streaming, and turn finalization all work
- the in-memory state mirrors the iOS concepts closely enough to support resume
  and conflict recovery
- file-oriented tool bubbles show filename-only summaries

## Exit Criteria For Polishing Stage

Before the quest can be considered ready for completion:

- desktop behavior feels stable during repeated thread switching
- disconnect and reconnect behavior has been manually verified
- the thinking and streaming indicators are visible but unobtrusive
- the UI does not expose full reasoning traces
- the protocol documentation still matches the actual implementation behavior
