"""Vault database schema bootstrap helpers."""

from __future__ import annotations

import sqlite3
from pathlib import Path

from sheaf.config.settings import REPO_ROOT


_MIGRATIONS_DIR = REPO_ROOT / "src" / "sheaf" / "vaults" / "migrations"


def migration_files() -> list[Path]:
    return sorted(_MIGRATIONS_DIR.glob("*.sql"))


def apply_migrations(conn: sqlite3.Connection, *, applied_at: str) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS schema_migrations (
            version TEXT PRIMARY KEY,
            applied_at TEXT NOT NULL
        )
        """
    )
    applied_versions = {
        str(row[0]) for row in conn.execute("SELECT version FROM schema_migrations").fetchall()
    }
    for path in migration_files():
        version = path.stem
        if version in applied_versions:
            continue
        sql = path.read_text(encoding="utf-8")
        conn.executescript(sql)
        conn.execute(
            "INSERT INTO schema_migrations(version, applied_at) VALUES (?, ?)",
            (version, applied_at),
        )
    conn.commit()
