# Sheaf Spec (Rewrite)

## Thread and Turn Ledger

- Threads are stored in `threads`
- Messages are stored as immutable `turns`
- Thread consistency uses CAS on `threads.tail_turn_id`

## Queue Execution

1. Client submits websocket message
2. Server persists queue row and sends durable ack
3. Worker claims runnable row
4. Worker executes model call and streams tokens/events
   - Deterministic pre-LLM compaction runs when estimated context exceeds model trigger ratio
   - Worker emits `turn_event` with `event=context_compaction` when compaction occurs
5. Worker commits user+assistant turns atomically
6. Queue row deleted on success

## Failure Model

- Non-fatal execution failures retry forever with exponential backoff capped at 10 seconds
- Fatal execution failures are moved to `queue_errors`
- Mid-stream disconnects rely on reconnect and ledger replay

## Stream Delivery

- Stream output is best-effort
- Ledger commit determines truth
- Reconnect provisioning restores canonical state

## Context Compaction

- Context usage is estimated deterministically from message content length
- Per-model compaction limits are sourced from `ModelProperties` and config tuning
- If usage exceeds trigger threshold:
  - Older messages are collapsed into one deterministic summary system message
  - Recent messages are retained based on `recent_messages_to_keep`
  - Summary may be trimmed to reach compaction target ratio
- On commit, a `turn_events` row with `event_type=context_compaction` is persisted for auditability
