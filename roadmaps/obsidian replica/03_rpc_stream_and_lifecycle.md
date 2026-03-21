# RPC Stream And Lifecycle

## Scope

Defines the network contract for keeping a local Obsidian vault synchronized with the server through a bidirectional gRPC connection.

## Transport Goals

- Allow the server to push new log records as they happen.
- Allow the client to request retransmission or raw-file fallback at any time.
- Recover cleanly from mobile suspend, disconnect, and restart.
- Keep the protocol simple enough to implement with a single long-lived stream per active vault.

## Session Model

While the extension is active, it maintains one bidirectional gRPC session for the vault.

Client responsibilities:
- identify the vault being synced
- advertise the last durable replay position
- acknowledge successfully replayed records
- request retransmission or raw-file fetch when replay cannot continue safely

Server responsibilities:
- stream ordered log entries newer than the client position
- accept retransmission requests
- deliver raw file contents on demand
- continue pushing newly committed log entries while the stream stays healthy

## Startup Handshake

The initial connection should establish both identity and replay position.

Suggested sequence:

1. Client opens the stream.
2. Client sends vault identity and local sync state.
3. If this is the first sync, client asks the server to create the named vault.
4. Client sends its current `next_lsn`.
5. Server responds with any records whose `lsn >= next_lsn`.
6. After backlog replay completes, the stream stays open for live pushes.

LSN rule:
- the client reports `next_lsn` directly
- the server treats all records with `lsn < next_lsn` as already handled by the client

## Message Types

First-pass protocol messages should cover:
- hello or start-sync request
- create-vault request for first sync
- log-record push
- replay acknowledgment
- retransmit-record request
- fetch-raw-file request
- raw-file response
- terminal error or resync-required notice

The exact protobuf shape can evolve later, but these semantic operations are required for the roadmap.

## Live Push Behavior

Once caught up, the server may push newly committed log records immediately.

Client behavior:
- process pushed records in order
- avoid acknowledging an LSN until file write and metadata persistence both succeed
- pause later records behind any unresolved earlier LSN

Server behavior:
- preserve in-order delivery within a vault stream
- tolerate the client asking for the same record or raw file more than once

## Retransmission And Raw Fetch

The stream must support recovery without creating a second protocol.

Retransmission cases:
- a log record was dropped or malformed
- client checksum verification failed and wants the same record resent
- client detects an LSN gap

Raw-file fetch cases:
- diff application failed
- checksum mismatch after replay
- repair scan found drift
- client needs the authoritative full file to recover

The server should treat both requests as normal control messages on the same stream whenever possible.

## Mobile Lifecycle Concerns

Mobile support drives several connection assumptions:
- the app may sleep without warning
- reconnects may be frequent
- the stream cannot assume hours of uninterrupted foreground execution

Design implications:
- the protocol must be resumable from persisted `next_lsn`
- reconnect should always begin with the same catch-up request pattern
- duplicate delivery is expected and must be safe
- heartbeats or lightweight keepalive messages may help detect dead streams, but correctness must not depend on them

## Backpressure And Ordering

The plugin should serialize replay for a single vault.

Rules:
- process one LSN at a time in order
- do not mark a later LSN successful if an earlier one is unresolved
- buffer or defer later pushed records while waiting on a retransmit or raw file

This keeps recovery simple and matches the server-authoritative ordering model.

## Error Handling

Expected protocol-level outcomes:
- transient disconnect followed by reconnect and catch-up
- explicit server request for client resync
- raw-file fallback for replay failures
- full revalidation after suspicious local drift

Escalation path:
- if replay repeatedly fails for the same file, request the raw file
- if raw-file verification still fails, mark the vault as unhealthy and stop advancing replay until a fresh resync is performed

## Rollout Plan

### Phase 1: Startup And Catch-Up

- define stream open and hello messages
- send highest replayed LSN at startup
- stream backlog records and replay them in order

### Phase 2: Live Updates

- keep the stream open after catch-up
- push new log entries from the server as they are committed
- add replay acknowledgments

### Phase 3: Recovery Controls

- add retransmit-record requests
- add raw-file fetch requests and responses
- add reconnect and resync-required handling

### Phase 4: Mobile Hardening

- validate behavior across suspend and resume
- tune reconnect timing and heartbeat policy
- add diagnostics for replay lag, retransmits, and raw-file fallbacks
