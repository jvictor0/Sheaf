CREATE TABLE IF NOT EXISTS schema_migrations (
    version TEXT PRIMARY KEY,
    applied_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS vaults (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    root_path TEXT NOT NULL UNIQUE,
    metadata_json TEXT NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    is_active INTEGER NOT NULL DEFAULT 1
);

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

CREATE INDEX IF NOT EXISTS idx_log_records_vault_lsn
    ON log_records(vault_id, lsn);

CREATE INDEX IF NOT EXISTS idx_log_records_vault_name_lsn
    ON log_records(vault_id, name, lsn);

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

CREATE INDEX IF NOT EXISTS idx_files_vault_last_lsn
    ON files(vault_id, last_lsn);
