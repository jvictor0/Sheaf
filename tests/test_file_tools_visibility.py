from __future__ import annotations

import sqlite3
from pathlib import Path

import pytest

from sheaf.tools.filesystem import create_file_tool, list_directory_tool, read_file_tool
import sheaf.tools.visibility as visibility
import sheaf.vaults.runtime as vault_runtime


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
    monkeypatch.setattr(vault_runtime, "VAULT_DB_PATH", tmp_path / "vaults.sqlite3")
    vault_runtime.initialize()
    return {"repo_root": repo_root, "db_path": db_path}


def test_read_and_list_respect_visible_directories_read_only(visible_env: dict[str, Path]) -> None:
    root = visible_env["repo_root"]
    note = root / "hello.txt"
    note.write_text("line1\nline2\n", encoding="utf-8")

    listing = list_directory_tool.invoke({"path": ".", "recursive": False})
    assert "hello.txt" in listing

    text = read_file_tool.invoke({"path": "hello.txt"})
    assert "line1" in text


def test_write_blocked_when_only_read_only_visible(visible_env: dict[str, Path]) -> None:
    with pytest.raises(ValueError, match="not writable by policy"):
        create_file_tool.invoke({"path": "denied.txt", "content": "x"})


def test_write_allowed_in_nested_read_write_override(visible_env: dict[str, Path]) -> None:
    root = visible_env["repo_root"]
    writable = root / "writable"
    writable.mkdir(parents=True, exist_ok=True)

    conn = sqlite3.connect(visible_env["db_path"])
    conn.execute(
        "INSERT INTO visible_directories(path, access_mode, created_at, updated_at) VALUES (?, 'read_write', 't', 't')",
        (str(writable.resolve()),),
    )
    conn.execute(
        """
        ATTACH DATABASE ? AS vaults_db
        """,
        (str(vault_runtime.VAULT_DB_PATH),),
    )
    conn.execute(
        """
        INSERT INTO vaults_db.vaults(root_path, metadata_json, created_at, updated_at, is_active)
        VALUES (?, NULL, 't', 't', 1)
        """,
        (str(writable.resolve()),),
    )
    conn.commit()
    conn.close()

    result = create_file_tool.invoke({"path": "writable/ok.txt", "content": "ok"})
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
