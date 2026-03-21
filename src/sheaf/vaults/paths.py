"""Vault ownership and path helpers."""

from __future__ import annotations

import sqlite3
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


def canonicalize_path(path: Path | str) -> Path:
    return Path(path).expanduser().resolve()


@dataclass(frozen=True)
class VaultRecord:
    id: int
    root_path: Path


def get_vault_for_path(conn: sqlite3.Connection, path: Path) -> Optional[VaultRecord]:
    resolved = canonicalize_path(path)
    rows = conn.execute(
        "SELECT id, root_path FROM vaults WHERE is_active = 1 ORDER BY LENGTH(root_path) DESC"
    ).fetchall()
    for row in rows:
        root = canonicalize_path(str(row["root_path"]))
        try:
            resolved.relative_to(root)
            return VaultRecord(id=int(row["id"]), root_path=root)
        except ValueError:
            continue
    return None


def require_vault_for_path(conn: sqlite3.Connection, path: Path) -> VaultRecord:
    existing = get_vault_for_path(conn, path)
    if existing is None:
        raise ValueError(f"No vault owns path: {path}")
    return existing


def validate_distinct_root(conn: sqlite3.Connection, root_path: Path) -> None:
    resolved = canonicalize_path(root_path)
    rows = conn.execute("SELECT root_path FROM vaults WHERE is_active = 1").fetchall()
    for row in rows:
        existing = canonicalize_path(str(row["root_path"]))
        overlaps = False
        try:
            resolved.relative_to(existing)
            overlaps = True
        except ValueError:
            pass
        if not overlaps:
            try:
                existing.relative_to(resolved)
                overlaps = True
            except ValueError:
                pass
        if overlaps:
            raise ValueError(f"Vault root overlaps existing vault: {existing}")


def relative_name(root_path: Path, path: Path) -> str:
    return canonicalize_path(path).relative_to(canonicalize_path(root_path)).as_posix()
