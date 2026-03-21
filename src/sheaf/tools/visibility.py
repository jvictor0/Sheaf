"""Filesystem visibility/access resolution based on visible_directories table."""

from __future__ import annotations

import sqlite3
from pathlib import Path
from typing import Optional

from sheaf.config.settings import REPO_ROOT, SERVER_DB_PATH


def resolve_input_path(path_text: str, *, default_to_repo_root: bool = False) -> Path:
    text = path_text.strip()
    if not text:
        if default_to_repo_root:
            return REPO_ROOT.resolve()
        raise ValueError("path must not be empty")
    candidate = Path(text).expanduser()
    if not candidate.is_absolute():
        candidate = REPO_ROOT / candidate
    return candidate.resolve()


def _load_visible_directories(*, db_path: Path | None = None) -> list[tuple[Path, str]]:
    resolved_db_path = (db_path or SERVER_DB_PATH).resolve()
    if not resolved_db_path.exists():
        return []
    conn = sqlite3.connect(resolved_db_path)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA busy_timeout=5000;")
    try:
        rows = conn.execute("SELECT path, access_mode FROM visible_directories").fetchall()
    finally:
        conn.close()

    out: list[tuple[Path, str]] = []
    for row in rows:
        raw_path = str(row["path"])
        mode = str(row["access_mode"])
        try:
            out.append((Path(raw_path).resolve(), mode))
        except OSError:
            continue
    return out


def _effective_access(path: Path, *, db_path: Path | None = None) -> Optional[str]:
    resolved = path.resolve()
    best_mode: Optional[str] = None
    best_len = -1
    for base, mode in _load_visible_directories(db_path=db_path):
        try:
            resolved.relative_to(base)
            candidate_len = len(str(base))
            if candidate_len > best_len:
                best_len = candidate_len
                best_mode = mode
        except ValueError:
            continue
    return best_mode


def ensure_visible(path: Path, *, db_path: Path | None = None) -> None:
    mode = _effective_access(path, db_path=db_path)
    if mode is None:
        raise ValueError(f"Path is not visible by policy: {path}")


def ensure_writable(path: Path, *, db_path: Path | None = None) -> None:
    mode = _effective_access(path, db_path=db_path)
    if mode is None:
        raise ValueError(f"Path is not visible by policy: {path}")
    if mode != "read_write":
        raise ValueError(f"Path is not writable by policy: {path}")
