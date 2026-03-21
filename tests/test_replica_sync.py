from __future__ import annotations

import json
import sqlite3
from pathlib import Path

from fastapi.testclient import TestClient

import sheaf.server.app as server_app
import sheaf.server.runtime as rr
import sheaf.vaults.runtime as vault_runtime
from sheaf.server.replica import ReplicaService


def _configure(tmp_path: Path) -> None:
    rr.DATA_DIR = tmp_path / "data"
    rr.DATA_ARCHIVE_DIR = tmp_path / "data_archive"
    rr.SERVER_DB_PATH = rr.DATA_DIR / "server.sqlite3"
    rr.USER_DBS_DIR = rr.DATA_DIR / "user_dbs"
    rr.SYSTEM_PROMPTS_DIR = rr.DATA_DIR / "system_prompts"
    vault_runtime.VAULT_DB_PATH = rr.DATA_DIR / "vaults.sqlite3"


def _insert_log_record(
    conn: sqlite3.Connection,
    *,
    vault_id: int,
    name: str,
    action: str,
    data: str | None,
    target_kind: str = "file",
) -> int:
    cursor = conn.execute(
        """
        INSERT INTO log_records(vault_id, name, target_kind, action, data, new_name, recorded_at)
        VALUES (?, ?, ?, ?, ?, NULL, ?)
        """,
        (vault_id, name, target_kind, action, data, rr.utc_now()),
    )
    return int(cursor.lastrowid)


def test_replica_session_creates_named_vault(tmp_path: Path) -> None:
    _configure(tmp_path)
    service = ReplicaService()

    session = service.start_session(
        vault_name="mobile-notes",
        next_lsn=0,
        create_if_missing=True,
        root_path=str(tmp_path / "vault"),
    )

    assert session["created"] is True
    with sqlite3.connect(vault_runtime.VAULT_DB_PATH) as conn:
        row = conn.execute("SELECT metadata_json FROM vaults WHERE id = ?", (session["vault_id"],)).fetchone()
    metadata = json.loads(str(row[0]))
    assert metadata["vault_name"] == "mobile-notes"
    with sqlite3.connect(rr.SERVER_DB_PATH) as conn:
        visible = conn.execute(
            "SELECT access_mode FROM visible_directories WHERE path = ?",
            (str(Path(str(session["root_path"])).resolve()),),
        ).fetchone()
    assert visible is not None
    assert visible[0] == "read_write"


def test_replica_session_defaults_root_path_under_data_vaults(tmp_path: Path) -> None:
    _configure(tmp_path)
    service = ReplicaService()

    session = service.start_session(
        vault_name="daily notes",
        next_lsn=0,
        create_if_missing=True,
    )

    expected = (rr.DATA_DIR / "vaults" / "daily_notes").resolve()
    assert Path(str(session["root_path"])).resolve() == expected


def test_replica_list_log_records_orders_file_events_only(tmp_path: Path) -> None:
    _configure(tmp_path)
    service = ReplicaService()
    created = service.start_session(
        vault_name="roadmap",
        next_lsn=0,
        create_if_missing=True,
        root_path=str(tmp_path / "vault"),
    )
    vault_id = int(created["vault_id"])

    with sqlite3.connect(vault_runtime.VAULT_DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        create_lsn = _insert_log_record(conn, vault_id=vault_id, name="note.md", action="create", data="one\n")
        _insert_log_record(conn, vault_id=vault_id, name="folder", action="create", data=None, target_kind="directory")
        patch_lsn = _insert_log_record(
            conn,
            vault_id=vault_id,
            name="note.md",
            action="patch",
            data="--- note.md\n+++ note.md\n@@ -1,1 +1,1 @@\n-one\n+two",
        )
        delete_lsn = _insert_log_record(conn, vault_id=vault_id, name="note.md", action="delete", data="two\n")
        conn.commit()

    records = service.list_log_records(vault_id=vault_id, next_lsn=patch_lsn)

    assert [item["lsn"] for item in records] == [patch_lsn, delete_lsn]
    assert [item["action"] for item in records] == ["patch", "delete"]
    assert records[0]["payload"]["patch"]
    assert records[1]["payload"] == {}
    assert create_lsn < patch_lsn < delete_lsn


def test_replica_list_log_records_isolated_to_requested_vault(tmp_path: Path) -> None:
    _configure(tmp_path)
    service = ReplicaService()
    first = service.start_session(
        vault_name="alpha",
        next_lsn=0,
        create_if_missing=True,
        root_path=str(tmp_path / "alpha"),
    )
    second = service.start_session(
        vault_name="beta",
        next_lsn=0,
        create_if_missing=True,
        root_path=str(tmp_path / "beta"),
    )

    with sqlite3.connect(vault_runtime.VAULT_DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        alpha_lsn = _insert_log_record(conn, vault_id=int(first["vault_id"]), name="alpha.md", action="create", data="a\n")
        beta_lsn = _insert_log_record(conn, vault_id=int(second["vault_id"]), name="beta.md", action="create", data="b\n")
        conn.commit()

    alpha_records = service.list_log_records(vault_id=int(first["vault_id"]), next_lsn=0)
    beta_records = service.list_log_records(vault_id=int(second["vault_id"]), next_lsn=0)

    assert [(record["lsn"], record["path"]) for record in alpha_records] == [(alpha_lsn, "alpha.md")]
    assert [(record["lsn"], record["path"]) for record in beta_records] == [(beta_lsn, "beta.md")]


def test_replica_path_state_returns_current_authoritative_content(tmp_path: Path) -> None:
    _configure(tmp_path)
    service = ReplicaService()
    created = service.start_session(
        vault_name="reader",
        next_lsn=0,
        create_if_missing=True,
        root_path=str(tmp_path / "vault"),
    )
    vault_id = int(created["vault_id"])

    with sqlite3.connect(vault_runtime.VAULT_DB_PATH) as conn:
        create_lsn = _insert_log_record(conn, vault_id=vault_id, name="note.md", action="create", data="one\n")
        patch_lsn = _insert_log_record(
            conn,
            vault_id=vault_id,
            name="note.md",
            action="patch",
            data="--- note.md\n+++ note.md\n@@ -1,1 +1,1 @@\n-one\n+two",
        )
        conn.execute(
            """
            INSERT INTO files(vault_id, name, created_lsn, last_lsn, checksum, created_at, updated_at)
            VALUES (?, ?, ?, ?, ?, ?, ?)
            """,
            (vault_id, "note.md", create_lsn, patch_lsn, "ignored", rr.utc_now(), rr.utc_now()),
        )
        conn.commit()

    state = service.get_path_state(vault_id=vault_id, path="note.md")
    missing = service.get_path_state(vault_id=vault_id, path="missing.md")

    assert state["exists"] is True
    assert state["content"] == "two\n"
    assert state["last_lsn"] == patch_lsn
    assert missing["exists"] is False
    assert missing["deleted"] is True


def test_replica_websocket_streams_backlog_and_raw_fetch(tmp_path: Path) -> None:
    _configure(tmp_path)
    server_app.runtime = rr.RewriteRuntime()
    server_app.replica_service = ReplicaService()

    with TestClient(server_app.app) as client:
        started = client.post(
            "/replica/sessions",
            json={
                "vault_name": "ws-vault",
                "next_lsn": 0,
                "create_if_missing": True,
                "root_path": str(tmp_path / "vault"),
            },
        )
        assert started.status_code == 200
        payload = started.json()
        vault_id = int(payload["vault_id"])

        with sqlite3.connect(vault_runtime.VAULT_DB_PATH) as conn:
            create_lsn = _insert_log_record(conn, vault_id=vault_id, name="note.md", action="create", data="hello\n")
            conn.execute(
                """
                INSERT INTO files(vault_id, name, created_lsn, last_lsn, checksum, created_at, updated_at)
                VALUES (?, ?, ?, ?, ?, ?, ?)
                """,
                (vault_id, "note.md", create_lsn, create_lsn, "ignored", rr.utc_now(), rr.utc_now()),
            )
            conn.commit()

        with client.websocket_connect(payload["websocket_url"]) as websocket:
            hello = websocket.receive_json()
            record = websocket.receive_json()
            websocket.send_json({"type": "fetch_raw_file", "path": "note.md"})
            paused = websocket.receive_json()
            raw = websocket.receive_json()
            websocket.send_json({"type": "resume_replication", "next_lsn": create_lsn + 1})
            resumed = websocket.receive_json()
            caught_up = websocket.receive_json()

        assert hello["type"] == "sync_hello"
        assert record["type"] == "log_record"
        assert record["path"] == "note.md"
        assert record["action"] == "create"
        assert paused["type"] == "replication_paused"
        assert caught_up["type"] == "sync_caught_up"
        assert raw["type"] == "raw_file_response"
        assert resumed["type"] == "replication_resumed"
        assert raw["content"] == "hello\n"
