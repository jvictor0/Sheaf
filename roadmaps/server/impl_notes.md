# Implementation Notes

## Queue retry cap

Current worker behavior intentionally has no maximum retry count for non-fatal execution errors.

- Retries use exponential back-off.
- Back-off is capped at 10 seconds per attempt.
- Retries continue indefinitely unless the error is classified as fatal, in which case the row moves to `queue_errors`.

## Migration policy (current)

Migration framework is intentionally deferred for now.

- We currently use a single bootstrap schema file at `src/sheaf/server/migrations/001_bootstrap.sql`.
- Server startup applies that bootstrap SQL idempotently.
- We are not implementing incremental migrations, migration tracking, or migration backups yet.
- We will add a migration mechanism when we need the first real in-place schema/data migration.

## Handshake drain behavior

Current handshake implementation drains outstanding queue work for the thread before sending `handshake_ready`.

- This can delay handshake completion when pending generation is long-running.
- The tradeoff is deterministic replay/state catch-up before the session is marked ready.

## OpenAI streaming timeout

Current OpenAI streaming path does not set an explicit per-stream timeout in dispatcher code.

- A hung upstream stream can stall worker execution for that queue item.
- Timeout/reclaim controls for this path are deferred and should be added with a worker-level cancellation strategy.

## Tool surface completeness

Current tool implementation intentionally does not yet include the full filesystem surface described in `09_tooling_surface.md`.

- Implemented now: `list_notes`, `read_note`, `write_note`, and SQLite tools.
- Deferred: patch application, move/rename, delete, and regex search (`rgrep`) tooling.
