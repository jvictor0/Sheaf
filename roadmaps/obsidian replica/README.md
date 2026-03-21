# Obsidian Replica Roadmap

This directory contains first-pass documents for an Obsidian plugin that mirrors server-owned vault state.

Current scope:
- Mobile-first Obsidian plugin design
- Server-authoritative file replication into the local vault
- Local write prevention for note editing
- Durable per-vault sync metadata
- Log replay, raw-file fallback, and repair scanning
- Bidirectional gRPC transport for catch-up and live updates

Documents:
- `01_mobile_replica_foundations.md`
- `02_log_replay_and_repair.md`
- `03_rpc_stream_and_lifecycle.md`

Assumptions currently encoded in this roadmap:
- Mobile support is the highest priority and desktop-only UI surfaces are out of scope.
- The server and its files are the source of truth for replicated vault contents.
- The plugin prevents local text edits through a CodeMirror 6 transaction filter.
- Sync state is persisted per vault, including the next replay LSN and per-file metadata.
- Replay uses at-least-once semantics and falls back to raw-file fetch when diff replay is not trustworthy.
- Recorded file metadata is considered valid only after write completion, mtime capture, and checksum verification.
- The plugin keeps a long-lived bidirectional gRPC connection while active so the server can push new log entries.
- A periodic scan reconciles local drift caused by unexpected writes or missing metadata.
- First sync can bootstrap the vault on the server if the named vault does not exist yet.
