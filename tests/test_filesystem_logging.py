from __future__ import annotations

import sqlite3
from pathlib import Path

import pytest

from sheaf.server.runtime import RewriteRuntime
from sheaf.tools.filesystem import apply_patch_tool, create_directory_tool, create_file_tool, delete_path_tool, move_path_tool
import sheaf.server.runtime as rr
import sheaf.tools.visibility as visibility
import sheaf.vaults.runtime as vault_runtime
from sheaf.vaults.logging import rebuild_files_table


@pytest.fixture
def filesystem_env(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> dict[str, Path]:
    data_dir = tmp_path / "data"
    server_db_path = data_dir / "server.sqlite3"
    vault_db_path = data_dir / "vaults.sqlite3"
    repo_root = tmp_path / "repo"
    repo_root.mkdir(parents=True, exist_ok=True)

    monkeypatch.setattr(rr, "DATA_DIR", data_dir)
    monkeypatch.setattr(rr, "SERVER_DB_PATH", server_db_path)
    monkeypatch.setattr(rr, "USER_DBS_DIR", data_dir / "user_dbs")
    monkeypatch.setattr(rr, "SYSTEM_PROMPTS_DIR", data_dir / "system_prompts")
    monkeypatch.setattr(visibility, "REPO_ROOT", repo_root)
    monkeypatch.setattr(visibility, "SERVER_DB_PATH", server_db_path)
    monkeypatch.setattr(vault_runtime, "VAULT_DB_PATH", vault_db_path)

    runtime = RewriteRuntime()
    runtime.initialize()

    vault_root = repo_root / "vault"
    with sqlite3.connect(server_db_path) as conn:
        conn.execute(
            "INSERT INTO visible_directories(path, access_mode, created_at, updated_at) VALUES (?, 'read_write', ?, ?)",
            (str(vault_root.resolve()), "t", "t"),
        )
        conn.commit()

    created = runtime.create_vault(root_path=str(vault_root))
    return {
        "repo_root": repo_root,
        "vault_root": vault_root,
        "server_db_path": server_db_path,
        "vault_db_path": vault_db_path,
        "vault_id": Path(str(created["vault_id"])),
    }


def test_create_patch_move_delete_file_are_logged(filesystem_env: dict[str, Path]) -> None:
    vault_root = filesystem_env["vault_root"]
    file_path = vault_root / "note.txt"

    assert "Wrote" in create_file_tool.invoke({"path": str(file_path), "content": "one\n"})
    patch = "--- note.txt\n+++ note.txt\n@@ -1,1 +1,1 @@\n-one\n+two"
    assert "Patched" in apply_patch_tool.invoke({"path": str(file_path), "patch": patch})
    moved = vault_root / "renamed.txt"
    assert "Moved" in move_path_tool.invoke({"source_path": str(file_path), "destination_path": str(moved)})
    assert "Deleted" in delete_path_tool.invoke({"path": str(moved)})

    with sqlite3.connect(filesystem_env["vault_db_path"]) as conn:
        conn.row_factory = sqlite3.Row
        rows = conn.execute(
            "SELECT action, target_kind, name, new_name FROM log_records ORDER BY lsn"
        ).fetchall()
        assert [(row["action"], row["target_kind"]) for row in rows] == [
            ("create", "file"),
            ("patch", "file"),
            ("create", "file"),
            ("delete", "file"),
            ("delete", "file"),
        ]
        assert rows[2]["name"] == "renamed.txt"
        assert rows[3]["name"] == "note.txt"
        count = conn.execute("SELECT COUNT(*) FROM files").fetchone()[0]
        assert count == 0


def test_directory_operations_and_rebuild(filesystem_env: dict[str, Path]) -> None:
    vault_root = filesystem_env["vault_root"]
    folder = vault_root / "a"
    child = folder / "child.txt"

    create_directory_tool.invoke({"path": str(folder)})
    create_file_tool.invoke({"path": str(child), "content": "child\n"})
    move_path_tool.invoke({"source_path": str(folder), "destination_path": str(vault_root / "b")})

    with sqlite3.connect(filesystem_env["vault_db_path"]) as conn:
        conn.row_factory = sqlite3.Row
        vault_id = int(conn.execute("SELECT id FROM vaults LIMIT 1").fetchone()[0])
        conn.execute("DELETE FROM files")
        rebuild_files_table(conn, vault_id=vault_id)
        conn.commit()
        names = [row["name"] for row in conn.execute("SELECT name FROM files ORDER BY name").fetchall()]
        assert names == ["b/child.txt"]


def test_rebuild_files_table_only_rebuilds_target_vault(filesystem_env: dict[str, Path]) -> None:
    runtime = RewriteRuntime()
    runtime.initialize()

    first_root = filesystem_env["vault_root"]
    second_root = filesystem_env["repo_root"] / "vault-two"
    second_created = runtime.create_vault(root_path=str(second_root))

    create_file_tool.invoke({"path": str(first_root / "first.md"), "content": "first\n"})
    create_file_tool.invoke({"path": str(second_root / "second.md"), "content": "second\n"})

    with sqlite3.connect(filesystem_env["vault_db_path"]) as conn:
        conn.row_factory = sqlite3.Row
        first_vault_id = int(conn.execute("SELECT id FROM vaults WHERE root_path = ?", (str(first_root.resolve()),)).fetchone()[0])
        second_vault_id = int(second_created["vault_id"])
        conn.execute("DELETE FROM files WHERE vault_id = ?", (first_vault_id,))
        rebuild_files_table(conn, vault_id=first_vault_id)
        conn.commit()

        first_names = [
            row["name"]
            for row in conn.execute("SELECT name FROM files WHERE vault_id = ? ORDER BY name", (first_vault_id,)).fetchall()
        ]
        second_names = [
            row["name"]
            for row in conn.execute("SELECT name FROM files WHERE vault_id = ? ORDER BY name", (second_vault_id,)).fetchall()
        ]

    assert first_names == ["first.md"]
    assert second_names == ["second.md"]


def test_create_vault_endpoint_rejects_overlapping_roots(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    data_dir = tmp_path / "data"
    rr.DATA_DIR = data_dir
    rr.SERVER_DB_PATH = data_dir / "server.sqlite3"
    rr.USER_DBS_DIR = data_dir / "user_dbs"
    rr.SYSTEM_PROMPTS_DIR = data_dir / "system_prompts"
    monkeypatch.setattr(vault_runtime, "VAULT_DB_PATH", data_dir / "vaults.sqlite3")
    runtime = RewriteRuntime()
    runtime.initialize()

    parent = tmp_path / "vaults" / "parent"
    runtime.create_vault(root_path=str(parent))
    with pytest.raises(ValueError, match="overlaps existing vault"):
        runtime.create_vault(root_path=str(parent / "child"))


def test_create_vault_registers_read_write_visible_directory(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    data_dir = tmp_path / "data"
    rr.DATA_DIR = data_dir
    rr.SERVER_DB_PATH = data_dir / "server.sqlite3"
    rr.USER_DBS_DIR = data_dir / "user_dbs"
    rr.SYSTEM_PROMPTS_DIR = data_dir / "system_prompts"
    monkeypatch.setattr(vault_runtime, "VAULT_DB_PATH", data_dir / "vaults.sqlite3")
    runtime = RewriteRuntime()
    runtime.initialize()

    root = tmp_path / "vaults" / "visible"
    runtime.create_vault(root_path=str(root))

    with sqlite3.connect(rr.SERVER_DB_PATH) as conn:
        row = conn.execute(
            "SELECT access_mode FROM visible_directories WHERE path = ?",
            (str(root.resolve()),),
        ).fetchone()
    assert row is not None
    assert row[0] == "read_write"
