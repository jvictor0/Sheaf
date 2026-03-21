# Obsidian Replica Foundations

## Scope

Defines the core local model for an Obsidian plugin that treats the local vault as a materialized replica of server state.

Initial feature set:
- prevent local file edits initiated through the editor
- persist replay progress and per-file sync metadata
- support first-sync vault registration
- keep the design compatible with Obsidian mobile

## Goals

- Make mobile support the default design target.
- Treat the server as the only source of truth for replicated file contents.
- Prevent normal interactive editing from producing unsanctioned local writes.
- Persist enough metadata to resume replay safely after restart.
- Keep local recovery logic simple and biased toward correctness over minimizing bandwidth.

## Non-Goals

- Desktop-only configuration windows or workflows
- Peer-to-peer sync between clients
- Local-first conflict resolution
- Arbitrary offline editing with later merge
- Binary-file diffing in the first pass

## Mobile-First Constraints

The plugin must work on Obsidian mobile, so all critical behavior must rely on APIs available on both mobile and desktop.

Design rules:
- Prefer vault APIs, editor extensions, and plugin data persistence that exist on mobile.
- Avoid desktop-only settings panes as the only way to recover or operate the replica.
- Assume the app may be suspended frequently and the network connection may drop whenever the app backgrounds.
- Make restart and reconnect behavior cheap and deterministic.

## Source Of Truth

The local Obsidian vault is a replica cache of server-owned files.

Rules:
- The server decides authoritative file contents and ordering.
- Local files may be overwritten during replay or repair without asking the user to merge.
- Local metadata exists to track replay progress and validation state, not to establish an alternate truth source.

## Local Write Prevention

The first user-visible feature is blocking local editor writes.

Primary mechanism:
- Install a CodeMirror 6 `transactionFilter` that rejects document-changing transactions for replicated notes.

Behavior:
- Non-mutating transactions such as selection changes, scrolling, and viewport updates continue to work.
- Mutating transactions created by direct typing, paste, undo, redo, or other editor actions are rejected.
- The filter should be enabled by default for the replicated vault and should not depend on desktop-only editor hooks.

Notes:
- This blocks the main note-editing path but does not by itself protect against every possible filesystem mutation from outside the editor.
- Periodic scan and repair still remain necessary because external plugins, sync tools, or manual filesystem changes can bypass the editor layer.

## Persisted Vault Store

The plugin needs a durable store scoped to the local vault.

Minimum persisted state:
- `vault_name`: logical server-side vault identifier
- `next_lsn`: the next log sequence number to process
- `files`: map keyed by vault-relative path

Suggested per-file metadata:
- `checksum`: checksum of the file content after the last successful server-sourced write
- `synced_mtime_ms`: local file modified time captured immediately after that write completed

`next_lsn` semantics:
- All log records with `lsn < next_lsn` have been successfully handled and recorded.
- The plugin advances `next_lsn` only after the corresponding file operation and metadata update both succeed.

## File Metadata Semantics

Per-file metadata describes a verified local observation of replicated state.

Rules:
- `checksum` is the checksum of the full local file content after the server operation is applied.
- `synced_mtime_ms` is the local filesystem mtime observed immediately after that content is written.
- A metadata entry is trustworthy only if checksum verification succeeded for that recorded mtime.

This means the metadata answer is:
- at `synced_mtime_ms`, the local file checksum matched the server checksum

It does not mean:
- the file is still unchanged now

## First Sync Bootstrap

When a vault starts syncing for the first time, the plugin uses the configured vault name to establish server state.

Startup rules:
- If local plugin data for this vault does not exist, initialize empty metadata with `next_lsn` set to the server-defined starting point for a new vault.
- Ask the server to create or register the named vault if it does not already exist.
- After registration completes, request the initial catch-up stream beginning at the local `next_lsn`.

## Safety Model

This design intentionally prefers recovery-friendly behavior:
- reject local edits early
- persist replay progress conservatively
- overwrite or refetch local files whenever verification is uncertain

Correctness matters more than avoiding an extra fetch or rewrite in the first implementation.
