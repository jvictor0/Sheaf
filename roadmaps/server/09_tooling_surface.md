# Tooling Surface

## Scope

Defines initial server-side tool capabilities available to the agent runtime.

## Filesystem Tools

Required tools:
- `ListDirectory`
  - Input: directory path
  - Output: entries with name, type, and basic metadata
- `ReadFile`
  - Input: file path
  - Output: file content
- `CreateFile`
  - Input: file path, initial content
  - Behavior: creates a new file (fails if file exists unless overwrite flag is added later)
- `ApplyPatch`
  - Input: file path + patch payload
  - Behavior: applies text patch update to existing file
- `MovePath`
  - Input: source path, destination path
  - Behavior: supports moving/renaming files and directories
- `DeletePath`
  - Input: path
  - Behavior: supports deleting files and directories

## Search Tool

- `rgrep`
  - Purpose: fast project search by regex/pattern
  - Input: query pattern, target path (optional), include/exclude filters (optional)
  - Output: matching file paths and/or matching lines

## Data Directory Layout

Introduce a dedicated data area for user-managed SQLite databases:
- root: `data/`
- user DB folder: `data/user_dbs/`
- system prompts folder: `data/system_prompts/`

Each user DB should map to a distinct file in `data/user_dbs/`.
Selected system prompt file is configured via `config.json`.

## `visible_directories` table

Defines filesystem visibility and write policy for tool execution.

```sql
CREATE TABLE visible_directories (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    path TEXT NOT NULL UNIQUE,               -- absolute canonical directory path
    access_mode TEXT NOT NULL                -- read_only | read_write
        CHECK (access_mode IN ('read_only', 'read_write')),
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
```

Resolution rule for a target file/directory path:
- Use prefix matching against `visible_directories.path`.
- Choose the most specific matching prefix (longest path match).
- Effective permission is that row's `access_mode`.

This handles nesting naturally:
- `read_only` nested in `read_write` -> nested subtree is read-only.
- `read_write` nested in `read_only` -> nested subtree is read-write.

## SQLite Tools

Required tools:
- `CreateUserSqliteDb`
  - Input: database name (or user identifier + database name)
  - Behavior: creates new SQLite file under `data/user_dbs/`
  - Output: created database path/id
- `RunUserSqlQuery`
  - Input: database id/path, SQL text, optional query params
  - Behavior: executes SQL against selected user database
  - Output: rows/result metadata/error

## Notes

- Filesystem tools (`ListDirectory`, `ReadFile`, `CreateFile`, `ApplyPatch`, `MovePath`, `DeletePath`) must enforce effective access from `visible_directories`.
- `rgrep` must enforce the same visibility/access rules before searching any target path.
- Tool execution should emit `turn_events` rows (arguments and outcomes).
- Long-running tool calls should stream progress events when possible.
- Exact JSON-RPC schema for these tools can be defined in a separate protocol doc.
- Agent is not allowed to write to the system database; agent writes are limited to user databases only.
