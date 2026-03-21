# File Write Logging

## Scope

Defines a durable logging system for filesystem write operations:
- create file
- patch file
- delete file
- rename file
- create directory
- delete directory
- rename directory

Read-path logging is out of scope.

## Goals

- Capture every successful filesystem write as a durable log record.
- Keep an append-only history that can be replayed in order.
- Maintain a fast current-state index of live files per vault.
- Track the checksum of each live file in current state.
- Support both file-level and directory-level write operations.
- Keep the write-log database separate from the main server database.
- Support a repair path that reconciles metadata to the filesystem.

## Non-Goals

- Logging reads
- File content deduplication or patch compression
- Binary diff generation
- Binary file write support
- Full filesystem snapshotting outside the log

## Core Concepts

- Vault: a tracked root directory with metadata
- Name: a path relative to the vault root
- LSN: a monotonically increasing log sequence number
- Checksum: the checksum of the current full file content
- Target kind: either `file` or `directory`
- Current-state index: the `files` table, representing the latest live file set

## Database Boundary

Introduce a second SQLite database for filesystem logging. One database stores many vaults.

Suggested location:
- `data/vaults.sqlite3`

This keeps filesystem write metadata isolated from the main runtime database while still allowing the runtime to open both databases in the same process.

## Table: `vaults`

Stores one row per tracked root directory.

```sql
CREATE TABLE IF NOT EXISTS vaults (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    root_path TEXT NOT NULL UNIQUE,
    metadata_json TEXT NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    is_active INTEGER NOT NULL DEFAULT 1
);
```

Notes:
- `root_path` should be stored in canonical absolute form.
- `metadata_json` can hold future policy or ownership metadata without forcing an immediate schema expansion.
- A vault should exist before write logging begins for that root.

## Table: `log_records`

Append-only event log. This is the source of truth for write history.

```sql
CREATE TABLE IF NOT EXISTS log_records (
    lsn INTEGER PRIMARY KEY AUTOINCREMENT,
    vault_id INTEGER NOT NULL,
    name TEXT NOT NULL,
    target_kind TEXT NOT NULL CHECK (target_kind IN ('file', 'directory')),
    action TEXT NOT NULL CHECK (action IN ('create', 'delete', 'patch', 'rename')),
    data TEXT NULL,
    new_name TEXT NULL,
    recorded_at TEXT NOT NULL,
    FOREIGN KEY (vault_id) REFERENCES vaults(id) ON DELETE CASCADE
);
```

Recommended indexes:

```sql
CREATE INDEX IF NOT EXISTS idx_log_records_vault_lsn
    ON log_records(vault_id, lsn);

CREATE INDEX IF NOT EXISTS idx_log_records_vault_name_lsn
    ON log_records(vault_id, name, lsn);
```

Field semantics:
- `lsn`: global increasing sequence for deterministic replay order
- `name`: vault-relative path of the source path
- `target_kind`: distinguishes file and directory operations
- `action`: the mutation type
- `data`: full file content for file create, patch payload for file patch, and final file content for file delete
- `new_name`: destination vault-relative path for rename operations

Action rules:
- `create` + `file`: `data` contains the full file content written to disk
- `create` + `directory`: `data` is null
- `patch` + `file`: `data` contains the exact patch payload applied to the file
- `patch` + `directory`: invalid
- `delete` + `file`: `data` contains the full final file content before deletion
- `delete` + `directory`: `data` is null for the directory marker row
- `rename`: `new_name` is required

This roadmap assumes text files only. Binary file writes are out of scope.

## Table: `files`

Current-state index of live files only.

```sql
CREATE TABLE IF NOT EXISTS files (
    vault_id INTEGER NOT NULL,
    name TEXT NOT NULL,
    created_lsn INTEGER NOT NULL,
    last_lsn INTEGER NOT NULL,
    checksum TEXT NOT NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    PRIMARY KEY (vault_id, name),
    FOREIGN KEY (vault_id) REFERENCES vaults(id) ON DELETE CASCADE,
    FOREIGN KEY (created_lsn) REFERENCES log_records(lsn),
    FOREIGN KEY (last_lsn) REFERENCES log_records(lsn)
);
```

Recommended indexes:

```sql
CREATE INDEX IF NOT EXISTS idx_files_vault_last_lsn
    ON files(vault_id, last_lsn);
```

Semantics:
- Insert on file creation
- Store the checksum of the current on-disk file content
- Update `last_lsn`, `checksum`, and `updated_at` on file patch
- Delete row on file deletion
- Move row to the new key on file rename

Checksum notes:
- Use one checksum algorithm everywhere for deterministic comparisons.
- Initial implementation should use SHA-256 over the full text file content.

## Directory Operation Semantics

The `files` table tracks files only, but directory operations must still be logged and reflected in current state.

Rules:
- Creating a directory adds only a `log_records` row.
- Deleting a directory is allowed only when the directory is empty.
- Deleting a non-empty directory must fail and must not emit any `log_records` row.
- Deleting an empty directory adds one directory `delete` row and does not modify `files`, since `files` tracks files only.
- Renaming a directory adds one `log_records` row and rewrites all descendant `files.name` values from the old prefix to the new prefix in the same transaction.

This preserves the user's desired file-only current-state table while still supporting directory writes as first-class log events.

## Write Pipeline

Each mutating filesystem tool should go through one common logging path:

1. Resolve the target path to a canonical absolute path.
2. Resolve the owning vault by longest matching root path.
3. Convert the path to a vault-relative name.
4. Execute the filesystem mutation first.
5. Capture the log payload for the operation.
6. Append one `log_records` row.
7. Update the `files` table to reflect the new live file state, including checksum updates.
8. Commit the vault database transaction.

All database updates for a single write should happen in one SQLite transaction. The filesystem write happens before the database write.

Payload rules:
- For file create, store the full file content.
- For file patch, store the patch payload.
- For file delete, store the final file content before deletion.
- For directory create, store no extra data.
- For directory delete, store no extra data.
- For rename, store the destination path in `new_name`.

## Consistency Model

This design intentionally prefers filesystem-first ordering:

- Write to the filesystem first.
- Record the write in the `vaults` database second.

Rationale:
- If the database write fails after the filesystem write succeeds, the file still exists and can be repaired later.
- This keeps the implementation simple and is acceptable for the current scale of the application.

Implication:
- The log is intended to be authoritative for normal operation, but it may occasionally need reconciliation if a filesystem write succeeds and the follow-up database write does not.

## Tool Mapping

Map existing mutating tools into the log as follows. The agent-facing write tools should be replaced by implementations that always perform the filesystem mutation and logging flow described here, while keeping the same high-level tool surface:

- `CreateFile`
  - `target_kind = 'file'`
  - `action = 'create'`
  - `data = full file content after create`
- `ApplyPatch`
  - `target_kind = 'file'`
  - `action = 'patch'`
  - `data = patch payload`
- `DeletePath`
  - `target_kind = 'file'` or `directory`
  - `action = 'delete'`
  - file delete rows store the final file content in `data`
  - directory delete succeeds only for empty directories
- `MovePath`
  - If source and destination are both files:
    - `target_kind = 'file'`
    - `action = 'rename'`
  - If source and destination are both directories:
    - `target_kind = 'directory'`
    - `action = 'rename'`

If `MovePath` later supports cross-vault moves, model it as:
- delete in source vault
- create in destination vault

That keeps `vault_id` stable per log record and avoids ambiguous rename semantics across roots.

## Current-State Update Rules

For a successful file create:
- insert log row
- compute checksum from the final on-disk content
- insert `files` row with `created_lsn = last_lsn = lsn`
- store the checksum in `files.checksum`

For a successful file patch:
- insert log row
- compute checksum from the final on-disk content
- update `files.last_lsn`
- update `files.checksum`
- update `files.updated_at`

For a successful file delete:
- insert log row
- delete the `files` row

For a successful file rename:
- insert log row
- move the `files` row to `new_name`
- set `last_lsn` and `updated_at`
- preserve the checksum if file contents do not change during rename

For a successful directory delete:
- require that the directory is empty
- insert one directory delete row
- leave `files` unchanged

For a successful directory rename:
- insert log row
- prefix-rewrite all descendant file rows
- set `last_lsn` and `updated_at` on rewritten rows

## Replay Model

`log_records` should be sufficient to rebuild `files` from scratch:

1. Start from an empty `files` table.
2. Read `log_records` ordered by `lsn`.
3. Apply each event deterministically.
4. Reconstruct the exact current-state file index.

This gives a path for:
- recovery
- integrity verification
- future backfill or schema migration work

Because file delete events carry the final file content, the log can also be replayed backward for file-level undo or forensic inspection.

## Repair Command

Add a repair command that scans a vault root and reconciles on-disk state with metadata and logs.

Suggested command shape:
- `RepairVault`
  - Input: `vault_id` or `root_path`
  - Behavior: scans the full vault root, compares disk state to reconstructed metadata state, appends repair log rows, updates `files`, and quarantines orphaned metadata files

Repair algorithm:

1. Load the vault root and the current `files` rows.
2. Scan all on-disk files under the vault.
3. Compute checksum for each on-disk file.
4. Compare the scan to current metadata.
5. For each on-disk file missing from metadata:
   - create a `create` log record with the full file content
   - insert the file into `files` with its checksum
6. For each file present in both places but with mismatched checksum:
   - reconstruct the metadata-side current content from the log
   - diff reconstructed content against on-disk content
   - append a `patch` log record using that diff
   - update `files.last_lsn`, `files.checksum`, and `files.updated_at`
7. For each file present in metadata but missing on disk:
   - reconstruct the metadata-side current content from the log
   - write the reconstructed content to a quarantine location
   - append a `delete` log record carrying the final reconstructed file content
   - remove the file from `files`

Quarantine location:
- `data/vault_quarantine/<vault_id>/<timestamp>/...`

Quarantine rules:
- Preserve the vault-relative path under the quarantine root.
- Preserve the reconstructed file content so nothing is silently lost.
- Emit a repair report describing what was created, patched, deleted, and quarantined.

Repair command intent:
- Reconcile metadata to the filesystem, not the other way around.
- Treat the on-disk filesystem as the source of truth during repair.

## Integrity Constraints

Implementation should enforce:
- one vault owns a path by canonical root-prefix matching
- every logged name is relative to exactly one vault root
- `create` logs the full file content
- `patch` logs the exact patch payload
- `delete` for files logs the final file content before removal
- `patch` is valid only for existing files
- `rename` requires a valid destination name
- non-empty directory delete is rejected without logging
- directory rename operates on descendant file rows by prefix
- `files.checksum` matches the current on-disk content after each successful create or patch
- `files` never contains duplicate `(vault_id, name)` rows

## Rollout Plan

### Phase 1: Schema And Vault Registry

- Add the new `vaults.sqlite3` database bootstrap path.
- Create `vaults`, `log_records`, and `files`.
- Add runtime helpers for canonical path resolution and vault lookup.
- Add checksum helpers for full text files.

### Phase 2: File Write Logging

- Route `CreateFile`, `ApplyPatch`, file `DeletePath`, and file `MovePath` through a shared write logger.
- Store full file contents for create events and patch payloads for patch events.
- Store final file content for file delete events.
- Compute and persist checksums in `files`.
- Persist log rows and `files` updates for file operations.
- Add tests for create, patch, delete, and rename.

### Phase 3: Directory Write Logging

- Add directory create, delete, and rename logging.
- Enforce empty-directory-only delete semantics.
- Implement descendant `files` rewrites for directory rename.
- Add tests for empty and non-empty directory delete behavior, plus rename prefix edge cases.

### Phase 4: Recovery And Verification

- Add a rebuild path from `log_records` to `files`.
- Add the `RepairVault` reconciliation command.
- Add quarantine output for metadata-only files.
- Add startup verification tooling for sample vault reconciliation.
- Add metrics or diagnostics for log growth and replay time.

## Open Questions

- Should rename operations also update `last_lsn` and timestamps for all descendant files during directory rename, or should rename markers be treated as sufficient lineage on their own.
