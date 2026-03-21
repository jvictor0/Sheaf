from __future__ import annotations

import sqlite3
from pathlib import Path

import pytest

from sheaf.tools.file_read_list import list_notes_tool, read_note_tool
from sheaf.tools.file_write import write_note_tool
import sheaf.tools.visibility as visibility


@pytest.fixture
def visible_env(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> dict[str, Path]:
    repo_root = tmp_path / "repo"
    repo_root.mkdir(parents=True, exist_ok=True)
    db_path = tmp_path / "server.sqlite3"

    conn = sqlite3.connect(db_path)
    conn.execute(
        """
        CREATE TABLE visible_directories (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            path TEXT NOT NULL UNIQUE,
            access_mode TEXT NOT NULL,
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        )
        """
    )
    conn.execute(
        "INSERT INTO visible_directories(path, access_mode, created_at, updated_at) VALUES (?, 'read_only', 't', 't')",
        (str(repo_root.resolve()),),
    )
    conn.commit()
    conn.close()

    monkeypatch.setattr(visibility, "REPO_ROOT", repo_root)
    monkeypatch.setattr(visibility, "SERVER_DB_PATH", db_path)
    return {"repo_root": repo_root, "db_path": db_path}


def test_read_and_list_respect_visible_directories_read_only(visible_env: dict[str, Path]) -> None:
    root = visible_env["repo_root"]
    note = root / "hello.txt"
    note.write_text("line1\nline2\n", encoding="utf-8")

    listing = list_notes_tool.invoke({"relative_dir": ".", "recursive": False})
    assert "hello.txt" in listing

    text = read_note_tool.invoke({"relative_path": "hello.txt"})
    assert "line1" in text


def test_write_blocked_when_only_read_only_visible(visible_env: dict[str, Path]) -> None:
    with pytest.raises(ValueError, match="not writable by policy"):
        write_note_tool.invoke({"relative_path": "denied.txt", "content": "x"})


def test_write_allowed_in_nested_read_write_override(visible_env: dict[str, Path]) -> None:
    root = visible_env["repo_root"]
    writable = root / "writable"
    writable.mkdir(parents=True, exist_ok=True)

    conn = sqlite3.connect(visible_env["db_path"])
    conn.execute(
        "INSERT INTO visible_directories(path, access_mode, created_at, updated_at) VALUES (?, 'read_write', 't', 't')",
        (str(writable.resolve()),),
    )
    conn.commit()
    conn.close()

    result = write_note_tool.invoke({"relative_path": "writable/ok.txt", "content": "ok"})
    assert "ok.txt" in result
    assert (writable / "ok.txt").exists()


def test_visibility_can_be_resolved_with_explicit_db_path(tmp_path: Path) -> None:
    from sheaf.tools.visibility import ensure_visible

    repo = tmp_path / "repo"
    repo.mkdir(parents=True, exist_ok=True)
    db_path = tmp_path / "server.sqlite3"
    target = repo / "sample.txt"
    target.write_text("ok", encoding="utf-8")

    conn = sqlite3.connect(db_path)
    conn.execute(
        """
        CREATE TABLE visible_directories (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            path TEXT NOT NULL UNIQUE,
            access_mode TEXT NOT NULL,
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        )
        """
    )
    conn.execute(
        "INSERT INTO visible_directories(path, access_mode, created_at, updated_at) VALUES (?, 'read_only', 't', 't')",
        (str(repo.resolve()),),
    )
    conn.commit()
    conn.close()

    ensure_visible(target, db_path=db_path)
