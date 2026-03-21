# DoWork Runner Contract

## Scope

Defines the worker entry point and in-memory delivery map behavior.

## In-Memory Delivery Map

Server maintains in-memory map:
- key: queued request ID
- value: struct with websocket handle (nullable/closed allowed)

Usage:
- On incoming client message, insert map entry for created queue row ID.
- If request came from pre-restart leftovers, map entry may be absent.
- If socket is closed, map entry can exist with closed/null websocket.

Map is delivery assistance only. Database remains source of truth.

## `DoWork` Entry Point

`DoWork` performs one pass:
1. Read one runnable row from `message_queue`.
2. If none exists, exit immediately.
3. Validate `in_response_to_turn_id` against current thread tail.
4. If valid, execute turn and stream events if websocket is live.
5. Finalize with atomic commit transaction (append turn, CAS tail update, delete queue row).
6. After commit, send final turn ID to websocket if still connected.

If validation fails:
- mark as non-executable conflict outcome
- require client reconnect/resync flow

## Delivery Rules

- Streaming events are attempted only when websocket exists and is open.
- Missing or closed websocket must not block execution and commit.
- Post-commit final ID message is best effort; reconnect covers missed delivery.
- Include `context_size` and `max_context_size` in handshake stream and per-message stream updates.

## Restart Behavior

After restart:
- Before accepting/processing work, run queue lock reset:
  - `UPDATE message_queue SET locked_by = NULL, locked_at = NULL;`
- Queue rows may exist with no in-memory websocket mapping.
- `DoWork` still executes them.
- Results become visible on next client reconnect/sync.

## Implementation Notes

- Keep DB transaction boundaries narrow for commit-critical steps.
- Keep model/tool execution outside final CAS+commit transaction when possible.
- Revalidate tail inside commit transaction to avoid stale pre-check races.
