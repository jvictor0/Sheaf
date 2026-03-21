# File Write Logging Roadmap

This directory contains first-pass documents for filesystem write logging.

Current scope:
- Dedicated SQLite database for vault metadata and write logs
- Append-only log for file and directory mutations
- Current-state file index keyed by vault-relative path
- Current-state file checksums keyed by vault-relative path
- Write-path integration for filesystem tools only
- Repair and reconciliation command for vault scans

Documents:
- `01_write_log_core.md`

Assumptions currently encoded in this roadmap:
- The new database is separate from the existing server database.
- A single `vaults` database contains metadata for many vault roots.
- Paths are stored relative to the vault root and are the stable key for tracked files.
- Log sequence numbers are globally increasing within the `log_records` table.
- File creation logs store full text content.
- File modification logs store patch payloads.
- File deletion logs store the final file content before removal.
- The `files` table stores the checksum of the current on-disk file state.
- Binary file writes are out of scope.
- Filesystem mutation happens before the logging database write.


