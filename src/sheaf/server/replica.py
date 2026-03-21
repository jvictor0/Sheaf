from __future__ import annotations

import asyncio
import json
import sqlite3
import threading
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional

from fastapi import WebSocket

import sheaf.server.runtime as server_runtime
from sheaf.server.runtime import utc_now
from sheaf.vaults.checksums import sha256_text
from sheaf.vaults.logging import reconstruct_file_content
from sheaf.vaults.paths import canonicalize_path, validate_distinct_root
from sheaf.vaults.runtime import db as vault_db
from sheaf.vaults.runtime import initialize as initialize_vault_db

REPLICA_PROTOCOL_VERSION = 1
REPLICA_HEARTBEAT_SECONDS = 15
_REPLICA_POLL_SECONDS = 1.0


class ReplicaProtocolError(RuntimeError):
    pass


@dataclass
class ReplicaSessionInfo:
    session_id: str
    vault_id: int
    vault_name: str
    next_lsn: int
    websocket: Optional[WebSocket] = None
    last_acked_lsn: Optional[int] = None
    inflight_lsn: Optional[int] = None
    replication_paused: bool = False
    caught_up_sent: bool = False


class ReplicaService:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._sessions: dict[str, ReplicaSessionInfo] = {}
        self._server_runtime = server_runtime.RewriteRuntime()

    def initialize(self) -> None:
        initialize_vault_db()
        self._server_runtime.initialize()

    def _parse_metadata_json(self, metadata_json: str | None) -> dict[str, Any]:
        if not metadata_json:
            return {}
        try:
            parsed = json.loads(metadata_json)
        except json.JSONDecodeError:
            return {}
        return parsed if isinstance(parsed, dict) else {}

    def _find_vault_by_name(self, conn: sqlite3.Connection, vault_name: str) -> sqlite3.Row | None:
        rows = conn.execute(
            "SELECT id, root_path, metadata_json, created_at, updated_at FROM vaults WHERE is_active = 1 ORDER BY id ASC"
        ).fetchall()
        for row in rows:
            metadata = self._parse_metadata_json(row["metadata_json"])
            if str(metadata.get("vault_name", "")).strip() == vault_name:
                return row
        return None

    def _default_root_path_for_vault(self, vault_name: str) -> Path:
        safe_name = "".join(char if char.isalnum() or char in {"-", "_", "."} else "_" for char in vault_name).strip("._")
        if not safe_name:
            safe_name = "vault"
        return server_runtime.DATA_DIR / "vaults" / safe_name

    def _ensure_vault(
        self,
        *,
        vault_name: str,
        create_if_missing: bool,
        root_path: str | None,
        metadata_json: str | None,
    ) -> tuple[sqlite3.Row, bool]:
        self.initialize()
        normalized_name = vault_name.strip()
        if not normalized_name:
            raise ValueError("vault_name is required")

        with vault_db() as conn:
            existing = self._find_vault_by_name(conn, normalized_name)
            if existing is not None:
                return existing, False
            if not create_if_missing:
                raise ValueError(f"Unknown replica vault '{normalized_name}'")
            resolved_root = (
                canonicalize_path(root_path)
                if root_path is not None and root_path.strip()
                else canonicalize_path(self._default_root_path_for_vault(normalized_name))
            )
            resolved_root.parent.mkdir(parents=True, exist_ok=True)
            if resolved_root.exists() and not resolved_root.is_dir():
                raise ValueError(f"Vault root must be a directory path: {resolved_root}")
            resolved_root.mkdir(parents=True, exist_ok=True)
            validate_distinct_root(conn, resolved_root)

            metadata = self._parse_metadata_json(metadata_json)
            metadata["vault_name"] = normalized_name
            now = utc_now()
            cursor = conn.execute(
                """
                INSERT INTO vaults(root_path, metadata_json, created_at, updated_at, is_active)
                VALUES (?, ?, ?, ?, 1)
                """,
                (str(resolved_root), json.dumps(metadata, separators=(",", ":"), ensure_ascii=False), now, now),
            )
            conn.commit()
            with self._server_runtime._db() as server_conn:
                self._server_runtime._register_visible_directory(
                    server_conn,
                    resolved_root,
                    access_mode="read_write",
                )
                server_conn.commit()
            created = conn.execute(
                "SELECT id, root_path, metadata_json, created_at, updated_at FROM vaults WHERE id = ?",
                (int(cursor.lastrowid),),
            ).fetchone()
            if created is None:
                raise RuntimeError("Failed to create replica vault")
            return created, True

    def start_session(
        self,
        *,
        vault_name: str,
        next_lsn: int,
        create_if_missing: bool,
        root_path: str | None = None,
        metadata_json: str | None = None,
    ) -> dict[str, Any]:
        row, created = self._ensure_vault(
            vault_name=vault_name,
            create_if_missing=create_if_missing,
            root_path=root_path,
            metadata_json=metadata_json,
        )
        session = ReplicaSessionInfo(
            session_id=str(uuid.uuid4()),
            vault_id=int(row["id"]),
            vault_name=vault_name.strip(),
            next_lsn=max(0, int(next_lsn)),
        )
        with self._lock:
            self._sessions[session.session_id] = session
        return {
            "session_id": session.session_id,
            "vault_id": session.vault_id,
            "vault_name": session.vault_name,
            "root_path": str(row["root_path"]),
            "created": created,
            "next_lsn": session.next_lsn,
        }

    def attach_websocket(self, session_id: str, websocket: WebSocket) -> ReplicaSessionInfo:
        with self._lock:
            session = self._sessions.get(session_id)
            if session is None:
                raise ReplicaProtocolError("Unknown replica session")
            session.websocket = websocket
            return session

    def detach_websocket(self, session_id: str) -> None:
        with self._lock:
            self._sessions.pop(session_id, None)

    def _record_checksum(self, conn: sqlite3.Connection, row: sqlite3.Row) -> str:
        action = str(row["action"])
        if action in {"create", "delete"}:
            return sha256_text(str(row["data"] or ""))
        return sha256_text(
            reconstruct_file_content(
                conn,
                vault_id=int(row["vault_id"]),
                name=str(row["name"]),
                up_to_lsn=int(row["lsn"]),
            )
        )

    def list_log_records(self, *, vault_id: int, next_lsn: int) -> list[dict[str, Any]]:
        self.initialize()
        with vault_db() as conn:
            rows = conn.execute(
                """
                SELECT lsn, vault_id, name, action, data, recorded_at
                FROM log_records
                WHERE vault_id = ? AND target_kind = 'file' AND action IN ('create', 'patch', 'delete') AND lsn >= ?
                ORDER BY lsn ASC
                """,
                (vault_id, max(0, int(next_lsn))),
            ).fetchall()
            records: list[dict[str, Any]] = []
            for row in rows:
                action = str(row["action"])
                payload: dict[str, Any] = {}
                if action == "create":
                    payload["content"] = str(row["data"] or "")
                elif action == "patch":
                    payload["patch"] = str(row["data"] or "")
                records.append(
                    {
                        "lsn": int(row["lsn"]),
                        "path": str(row["name"]),
                        "action": action,
                        "checksum": self._record_checksum(conn, row),
                        "payload": payload,
                        "recorded_at": str(row["recorded_at"]),
                    }
                )
            return records

    def get_path_state(self, *, vault_id: int, path: str) -> dict[str, Any]:
        normalized = path.strip().lstrip("/")
        if not normalized:
            raise ValueError("path is required")
        self.initialize()
        with vault_db() as conn:
            row = conn.execute(
                "SELECT name, checksum, last_lsn FROM files WHERE vault_id = ? AND name = ?",
                (vault_id, normalized),
            ).fetchone()
            if row is None:
                return {
                    "path": normalized,
                    "exists": False,
                    "deleted": True,
                    "checksum": None,
                    "content": None,
                    "last_lsn": None,
                }
            content = reconstruct_file_content(
                conn,
                vault_id=vault_id,
                name=normalized,
                up_to_lsn=int(row["last_lsn"]),
            )
            checksum = str(row["checksum"]) or sha256_text(content)
            return {
                "path": normalized,
                "exists": True,
                "deleted": False,
                "checksum": checksum,
                "content": content,
                "last_lsn": int(row["last_lsn"]),
            }

    async def send_frame(self, websocket: WebSocket, session_id: str, frame_type: str, payload: dict[str, Any]) -> None:
        await websocket.send_json(
            {
                "protocol_version": REPLICA_PROTOCOL_VERSION,
                "type": frame_type,
                "session_id": session_id,
                "server_time": utc_now(),
                **payload,
            }
        )

    async def _send_next_record(self, session: ReplicaSessionInfo) -> None:
        websocket = session.websocket
        if websocket is None:
            return
        if session.replication_paused:
            return
        if session.inflight_lsn is not None:
            return
        records = self.list_log_records(vault_id=session.vault_id, next_lsn=session.next_lsn)
        if not records:
            if not session.caught_up_sent:
                await self.send_frame(websocket, session.session_id, "sync_caught_up", {"next_lsn": session.next_lsn})
                session.caught_up_sent = True
            return
        record = records[0]
        await self.send_frame(websocket, session.session_id, "log_record", record)
        session.inflight_lsn = int(record["lsn"])
        session.caught_up_sent = False

    async def stream_session(self, session: ReplicaSessionInfo) -> None:
        websocket = session.websocket
        if websocket is None:
            raise ReplicaProtocolError("Replica websocket is not attached")
        await self.send_frame(
            websocket,
            session.session_id,
            "sync_hello",
            {
                "vault_id": session.vault_id,
                "vault_name": session.vault_name,
                "next_lsn": session.next_lsn,
                "heartbeat_seconds": REPLICA_HEARTBEAT_SECONDS,
            },
        )
        await self._send_next_record(session)

        while True:
            try:
                message = await asyncio.wait_for(websocket.receive_json(), timeout=_REPLICA_POLL_SECONDS)
            except asyncio.TimeoutError:
                await self._send_next_record(session)
                continue

            frame_type = str(message.get("type", ""))
            if frame_type == "ack":
                ack_lsn = int(message.get("lsn", 0))
                if session.inflight_lsn is not None and ack_lsn == session.inflight_lsn:
                    session.last_acked_lsn = ack_lsn
                    session.next_lsn = ack_lsn + 1
                    session.inflight_lsn = None
                await self._send_next_record(session)
                continue

            if frame_type == "retransmit_record":
                session.replication_paused = True
                session.inflight_lsn = None
                await self.send_frame(
                    websocket,
                    session.session_id,
                    "replication_paused",
                    {"next_lsn": session.next_lsn},
                )
                lsn = int(message.get("lsn", 0))
                records = self.list_log_records(vault_id=session.vault_id, next_lsn=lsn)
                record = next((item for item in records if int(item["lsn"]) == lsn), None)
                if record is None:
                    await self.send_frame(
                        websocket,
                        session.session_id,
                        "resync_required",
                        {"reason": f"Missing record for lsn={lsn}"},
                    )
                    continue
                await self.send_frame(websocket, session.session_id, "log_record", record)
                continue

            if frame_type == "fetch_raw_file":
                if not session.replication_paused:
                    session.replication_paused = True
                    session.inflight_lsn = None
                    await self.send_frame(
                        websocket,
                        session.session_id,
                        "replication_paused",
                        {"next_lsn": session.next_lsn},
                    )
                path_state = self.get_path_state(vault_id=session.vault_id, path=str(message.get("path", "")))
                await self.send_frame(websocket, session.session_id, "raw_file_response", path_state)
                continue

            if frame_type == "query_path_state":
                if not session.replication_paused:
                    session.replication_paused = True
                    session.inflight_lsn = None
                    await self.send_frame(
                        websocket,
                        session.session_id,
                        "replication_paused",
                        {"next_lsn": session.next_lsn},
                    )
                path_state = self.get_path_state(vault_id=session.vault_id, path=str(message.get("path", "")))
                await self.send_frame(
                    websocket,
                    session.session_id,
                    "path_state_response",
                    {key: value for key, value in path_state.items() if key != "content"},
                )
                continue

            if frame_type == "resume_replication":
                session.replication_paused = False
                session.inflight_lsn = None
                session.next_lsn = max(0, int(message.get("next_lsn", session.next_lsn)))
                session.caught_up_sent = False
                await self.send_frame(
                    websocket,
                    session.session_id,
                    "replication_resumed",
                    {"next_lsn": session.next_lsn},
                )
                await self._send_next_record(session)
                continue

            if frame_type == "heartbeat":
                await self.send_frame(websocket, session.session_id, "heartbeat", {"interval_seconds": REPLICA_HEARTBEAT_SECONDS})
                continue

            await self.send_frame(
                websocket,
                session.session_id,
                "error",
                {"message": f"Unsupported replica frame type '{frame_type}'"},
            )
