# Log Replay And Repair

## Scope

Defines how the plugin replays server log records into the local vault, verifies each result, and repairs drift when local state becomes suspicious.

## Replay Inputs

Each replayable server event should include enough information to validate the resulting local file state.

Minimum event fields:
- `lsn`
- `path`
- `action`
- `checksum`
- operation payload for create, delete, or modify

Supported actions in the first pass:
- create file
- modify file
- delete file

Directory events can be added later if the server log exposes them, but the initial mobile replica should focus on file correctness.

## Replay Contract

Replay is ordered by increasing LSN and uses at-least-once semantics.

Rules:
- Replaying the same record again must be safe.
- Deleting an already-missing file is treated as success.
- Creating or overwriting a file at an existing path is treated as success when the final file content matches the server checksum.
- Any operation that cannot be trusted falls back to fetching the full raw file from the server.

## Per-Record Apply Protocol

For each log record:

1. Apply the operation to the vault.
2. If the operation does not complete cleanly, request the raw file from the server.
3. Write the resulting file contents locally, or delete the file for delete records.
4. Read the local file mtime after the write completes.
5. Compute the checksum of the local file contents.
6. Verify the checksum matches the checksum attached to the server record.
7. If verification fails, request the raw file again, rewrite it, and verify that mtime has not changed before recording metadata.
8. Persist the file metadata update and advance `next_lsn`.

This records a local point-in-time observation that was verified against the server checksum.

## Raw File Fallback

Fallback to a raw file fetch is the main recovery path for any uncertain replay result.

Trigger conditions:
- patch application fails
- the write API returns an error
- the file does not exist after a supposed successful create or modify
- the computed checksum does not match the server checksum
- the file changed again before metadata could be recorded

Fallback behavior:
- request the current full file content from the server for that path
- overwrite the local file with the fetched content
- recompute checksum and capture mtime
- record success only after checksum verification passes

For delete records:
- if delete fails because the file is already missing, treat that as success
- if local state remains inconsistent after delete handling, ask the server for authoritative path state before advancing `next_lsn`

## Metadata Update Rules

After each successful replayed record:
- update or delete the per-file metadata entry
- set `checksum` to the verified post-write checksum for create and modify
- set `synced_mtime_ms` to the observed post-write mtime for create and modify
- remove the metadata entry for verified deletes
- advance `next_lsn` to `lsn + 1`

Atomicity goal:
- Do not advance `next_lsn` unless the local filesystem result and persisted metadata agree.

If persistence fails after a local write succeeds:
- leave `next_lsn` unchanged
- allow the record to be replayed again on restart
- rely on idempotent apply rules and checksum verification to settle on the correct final state

## Why Mtime Is Recorded

Mtime is not the primary proof of correctness. Checksum is.

Mtime is stored because it helps detect whether a file may have changed after the last verified replay.

Interpretation:
- if current mtime equals `synced_mtime_ms`, the file is likely unchanged since the last verified write
- if current mtime is greater than `synced_mtime_ms`, the file needs suspicion and likely revalidation

## Periodic Scan

The plugin should periodically scan the vault for local drift.

Scan checks:
- files whose current mtime is greater than recorded `synced_mtime_ms`
- files present on disk but missing from metadata
- files present in metadata but missing on disk

Repair policy:
- if a scanned file looks suspicious, compute its checksum
- if the checksum matches the recorded checksum, refresh `synced_mtime_ms` if appropriate
- if the checksum differs, fetch the authoritative raw file from the server and rewrite it
- if a metadata-tracked file is missing, fetch it from the server unless the server now says it should be deleted
- if an untracked local file exists, ask the server for authoritative state and either overwrite it with server content or delete it

This scan is the backstop for writes that bypass editor filtering.

## Startup Catch-Up

On startup:

1. Load persisted plugin data for the vault.
2. Read `next_lsn`.
3. Send the server the highest replayed LSN, which is `next_lsn - 1` when `next_lsn > 0`.
4. Request all newer log records.
5. Replay them in order before treating the local vault as caught up.

The same catch-up flow is used after reconnect.

## Failure Model

Expected failures:
- dropped network connection
- app suspension on mobile
- replay interruptions between file write and metadata persistence
- unexpected local writes by external code

Recovery stance:
- resume from persisted `next_lsn`
- replay idempotently
- prefer raw-file retransmission over complex local repair
- never trust an unverified file write enough to advance replay progress
