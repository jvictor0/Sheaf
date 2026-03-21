from __future__ import annotations

import asyncio
import json
import logging
import re
import sqlite3
import threading
import time
import uuid
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Optional

from fastapi import WebSocket

from sheaf.config.settings import (
    DATA_DIR,
    REPO_ROOT,
    SERVER_DB_PATH,
    SYSTEM_PROMPTS_DIR,
    USER_DBS_DIR,
    configured_default_model,
    configured_system_prompt_file,
)
from sheaf.llm.dispatcher import (
    Message,
    ProviderConfigurationError,
    UnsupportedModelError,
    build_dispatcher,
)
from sheaf.llm.model_properties import resolve_model_properties
from sheaf.llm.model_registry import get_model_registry
from sheaf.vaults.paths import canonicalize_path, validate_distinct_root
from sheaf.vaults.runtime import db as vault_db
from sheaf.vaults.runtime import initialize as initialize_vault_db

try:
    from openai import AuthenticationError as OpenAIAuthenticationError
    from openai import BadRequestError as OpenAIBadRequestError
    from openai import NotFoundError as OpenAINotFoundError
    from openai import PermissionDeniedError as OpenAIPermissionDeniedError
except Exception:  # noqa: BLE001
    OpenAIAuthenticationError = None
    OpenAIBadRequestError = None
    OpenAINotFoundError = None
    OpenAIPermissionDeniedError = None

PROTOCOL_VERSION = 1
HEARTBEAT_SECONDS = 15
_WORKER_POLL_SECONDS = 0.25
_RETRY_BASE_SECONDS = 0.5
_RETRY_MAX_SECONDS = 10.0
logger = logging.getLogger(__name__)
_BOOTSTRAP_SQL_PATH = REPO_ROOT / "src" / "sheaf" / "server" / "migrations" / "001_bootstrap.sql"
_STREAM_PROGRESS_TRACE = re.compile(r"^streamed_\d+_chunks$")
_DEFAULT_SYSTEM_PROMPT = """You are Sheaf, a local-first assistant.

Follow these rules:
- Be accurate, concise, and explicit about uncertainty.
- Prefer using available tools when they materially improve correctness.
- When using tools, explain results clearly and cite concrete outputs.
- Keep responses actionable; avoid unnecessary filler.
- Preserve user intent and constraints exactly.
"""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def utc_after(seconds: float) -> str:
    return (datetime.now(timezone.utc) + timedelta(seconds=seconds)).isoformat()


def _json(data: Any) -> str:
    return json.dumps(data, separators=(",", ":"), ensure_ascii=False)


class ProtocolError(RuntimeError):
    pass


class FatalExecutionError(RuntimeError):
    pass


@dataclass
class SessionInfo:
    session_id: str
    thread_id: str
    known_tail_turn_id: Optional[str]
    websocket: Optional[WebSocket] = None


class RewriteRuntime:
    def __init__(self) -> None:
        self._init_lock = threading.Lock()
        self._initialized = False

        self._session_lock = threading.Lock()
        self._sessions: dict[str, SessionInfo] = {}
        self._queue_delivery_map: dict[int, Optional[str]] = {}
        self._queue_request_map: dict[int, str] = {}

        self._worker_task: asyncio.Task[None] | None = None
        self._worker_stop: asyncio.Event | None = None
        self._worker_wake: asyncio.Event | None = None
        self._worker_loop_event_loop: asyncio.AbstractEventLoop | None = None
        self._worker_id = f"worker-{uuid.uuid4().hex[:8]}"

    def initialize(self) -> None:
        with self._init_lock:
            if self._initialized:
                return
            DATA_DIR.mkdir(parents=True, exist_ok=True)
            USER_DBS_DIR.mkdir(parents=True, exist_ok=True)
            SYSTEM_PROMPTS_DIR.mkdir(parents=True, exist_ok=True)
            self._ensure_default_system_prompt()
            SERVER_DB_PATH.parent.mkdir(parents=True, exist_ok=True)
            initialize_vault_db()
            with self._db() as conn:
                self._apply_database_pragmas(conn)
                self._bootstrap_schema(conn)
                self._reset_queue_locks(conn)
                self._seed_models(conn)
                self._seed_visible_directories(conn)
            self._initialized = True

    async def start_worker(self) -> None:
        self.initialize()
        if self._worker_task is not None and not self._worker_task.done():
            return
        self._worker_stop = asyncio.Event()
        self._worker_wake = asyncio.Event()
        self._worker_loop_event_loop = asyncio.get_running_loop()
        self._worker_task = asyncio.create_task(self._worker_loop(), name="sheaf-queue-worker")

    async def stop_worker(self) -> None:
        if self._worker_stop is not None:
            self._worker_stop.set()
        task = self._worker_task
        if task is not None:
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
        self._worker_task = None
        self._worker_wake = None
        self._worker_loop_event_loop = None

    async def _worker_loop(self) -> None:
        while self._worker_stop is None or not self._worker_stop.is_set():
            try:
                processed = await self.process_next_runnable()
            except Exception:  # noqa: BLE001
                logger.exception("Unhandled exception in worker loop")
                processed = False
            if not processed:
                wake = self._worker_wake
                if wake is None:
                    await asyncio.sleep(_WORKER_POLL_SECONDS)
                else:
                    try:
                        await asyncio.wait_for(wake.wait(), timeout=_WORKER_POLL_SECONDS)
                        wake.clear()
                    except asyncio.TimeoutError:
                        pass

    def _connect(self) -> sqlite3.Connection:
        conn = sqlite3.connect(SERVER_DB_PATH)
        conn.row_factory = sqlite3.Row
        self._apply_connection_pragmas(conn)
        return conn

    @contextmanager
    def _db(self):
        conn = self._connect()
        try:
            yield conn
        finally:
            conn.close()

    def _apply_connection_pragmas(self, conn: sqlite3.Connection) -> None:
        conn.execute("PRAGMA synchronous=NORMAL;")
        conn.execute("PRAGMA busy_timeout=5000;")

    def _apply_database_pragmas(self, conn: sqlite3.Connection) -> None:
        conn.execute("PRAGMA journal_mode=WAL;")
        self._apply_connection_pragmas(conn)

    def _bootstrap_schema(self, conn: sqlite3.Connection) -> None:
        try:
            bootstrap_sql = _BOOTSTRAP_SQL_PATH.read_text(encoding="utf-8")
        except OSError as exc:
            raise RuntimeError(f"Failed to read bootstrap schema file: {_BOOTSTRAP_SQL_PATH}") from exc
        conn.executescript(bootstrap_sql)
        conn.commit()

    def _reset_queue_locks(self, conn: sqlite3.Connection) -> None:
        conn.execute("UPDATE message_queue SET locked_by = NULL, locked_at = NULL")
        conn.commit()

    def _seed_models(self, conn: sqlite3.Connection) -> None:
        now = utc_now()
        for model in get_model_registry().list_models():
            conn.execute(
                """
                INSERT INTO models(
                    name, provider, api_model_id, is_local, local_url,
                    context_window_tokens, max_output_tokens, metadata_json,
                    created_at, updated_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(name) DO UPDATE SET
                    provider=excluded.provider,
                    api_model_id=excluded.api_model_id,
                    is_local=excluded.is_local,
                    local_url=excluded.local_url,
                    metadata_json=excluded.metadata_json,
                    updated_at=excluded.updated_at
                """,
                (
                    model.name,
                    model.provider,
                    model.name,
                    1 if model.provider == "ollama" else 0,
                    model.metadata.get("base_url") if isinstance(model.metadata, dict) else None,
                    None,
                    None,
                    _json(model.metadata),
                    now,
                    now,
                ),
            )
        conn.commit()

    def _seed_visible_directories(self, conn: sqlite3.Connection) -> None:
        now = utc_now()
        defaults = [
            (str(REPO_ROOT.resolve()), "read_only"),
            (str((DATA_DIR / "system_prompts").resolve()), "read_only"),
        ]
        for path, mode in defaults:
            conn.execute(
                """
                INSERT INTO visible_directories(path, access_mode, created_at, updated_at)
                VALUES (?, ?, ?, ?)
                ON CONFLICT(path) DO NOTHING
                """,
                (path, mode, now, now),
            )
        conn.commit()

    def _register_visible_directory(self, conn: sqlite3.Connection, path: Path, *, access_mode: str) -> None:
        now = utc_now()
        conn.execute(
            """
            INSERT INTO visible_directories(path, access_mode, created_at, updated_at)
            VALUES (?, ?, ?, ?)
            ON CONFLICT(path) DO UPDATE SET
                access_mode=excluded.access_mode,
                updated_at=excluded.updated_at
            """,
            (str(path.resolve()), access_mode, now, now),
        )

    def refresh_local_models(self) -> list[dict[str, Any]]:
        self.initialize()
        with self._db() as conn:
            self._seed_models(conn)
            rows = conn.execute("SELECT * FROM models WHERE is_local = 1 ORDER BY name").fetchall()
            return [self._model_row_to_dict(row, include_client_shape=True) for row in rows]

    def list_models(self) -> list[dict[str, Any]]:
        self.initialize()
        with self._db() as conn:
            rows = conn.execute("SELECT * FROM models ORDER BY name").fetchall()
            return [self._model_row_to_dict(row, include_client_shape=True) for row in rows]

    def _model_row_to_dict(self, row: sqlite3.Row, *, include_client_shape: bool = False) -> dict[str, Any]:
        metadata: dict[str, Any] = {}
        if row["metadata_json"]:
            try:
                parsed = json.loads(row["metadata_json"])
                if isinstance(parsed, dict):
                    metadata = parsed
            except json.JSONDecodeError:
                metadata = {}

        item: dict[str, Any] = {
            "name": row["name"],
            "provider": row["provider"],
            "api_model_id": row["api_model_id"],
            "is_local": bool(row["is_local"]),
            "local_url": row["local_url"],
            "context_window_tokens": row["context_window_tokens"],
            "max_output_tokens": row["max_output_tokens"],
            "metadata_json": row["metadata_json"],
        }
        if include_client_shape:
            item.update(
                {
                    "source": "registry",
                    "metadata": metadata,
                    "is_default": row["name"] == configured_default_model(),
                }
            )
        return item

    def create_thread(
        self,
        *,
        thread_id: Optional[str] = None,
        name: Optional[str] = None,
        prev_thread_id: Optional[str] = None,
        start_turn_id: Optional[str] = None,
    ) -> str:
        self.initialize()
        resolved = thread_id or str(uuid.uuid4())
        resolved_name = (name or "").strip() or f"Thread {resolved[:8]}"
        now = utc_now()
        with self._db() as conn:
            conn.execute(
                """
                INSERT INTO threads(id, name, prev_thread_id, start_turn_id, is_archived, created_at, updated_at, tail_turn_id)
                VALUES (?, ?, ?, ?, 0, ?, ?, NULL)
                """,
                (resolved, resolved_name, prev_thread_id, start_turn_id, now, now),
            )
            conn.commit()
        return resolved

    def list_threads(self) -> list[dict[str, Any]]:
        self.initialize()
        with self._db() as conn:
            rows = conn.execute(
                """
                SELECT id, name, prev_thread_id, start_turn_id, is_archived, tail_turn_id, created_at, updated_at
                FROM threads
                WHERE is_archived = 0
                ORDER BY updated_at DESC
                """
            ).fetchall()
        return [
            {
                "thread_id": row["id"],
                "name": row["name"],
                "prev_thread_id": row["prev_thread_id"],
                "start_turn_id": row["start_turn_id"],
                "is_archived": bool(row["is_archived"]),
                "tail_turn_id": row["tail_turn_id"],
                "created_at": row["created_at"],
                "updated_at": row["updated_at"],
            }
            for row in rows
        ]

    def create_vault(self, *, root_path: str, metadata_json: str | None = None) -> dict[str, Any]:
        self.initialize()
        resolved_root = canonicalize_path(root_path)
        ensure_parent = resolved_root.parent
        ensure_parent.mkdir(parents=True, exist_ok=True)
        if resolved_root.exists() and not resolved_root.is_dir():
            raise ValueError(f"Vault root must be a directory path: {resolved_root}")
        resolved_root.mkdir(parents=True, exist_ok=True)
        now = utc_now()
        with vault_db() as conn:
            validate_distinct_root(conn, resolved_root)
            cursor = conn.execute(
                """
                INSERT INTO vaults(root_path, metadata_json, created_at, updated_at, is_active)
                VALUES (?, ?, ?, ?, 1)
                """,
                (str(resolved_root), metadata_json, now, now),
            )
            conn.commit()
        with self._db() as conn:
            self._register_visible_directory(conn, resolved_root, access_mode="read_write")
            conn.commit()
        return {
            "vault_id": int(cursor.lastrowid),
            "root_path": str(resolved_root),
            "created": True,
        }

    def archive_thread(self, thread_id: str, *, archived: bool) -> None:
        self.initialize()
        with self._db() as conn:
            conn.execute(
                "UPDATE threads SET is_archived = ?, updated_at = ? WHERE id = ?",
                (1 if archived else 0, utc_now(), thread_id),
            )
            conn.commit()

    def create_session(self, thread_id: str, known_tail_turn_id: Optional[str]) -> SessionInfo:
        self.initialize()
        with self._db() as conn:
            exists = conn.execute("SELECT 1 FROM threads WHERE id = ?", (thread_id,)).fetchone()
        if exists is None:
            raise ProtocolError(f"Unknown thread '{thread_id}'")

        session = SessionInfo(
            session_id=str(uuid.uuid4()),
            thread_id=thread_id,
            known_tail_turn_id=known_tail_turn_id,
        )
        with self._session_lock:
            self._sessions[session.session_id] = session
        return session

    def attach_websocket(self, session_id: str, websocket: WebSocket) -> SessionInfo:
        with self._session_lock:
            session = self._sessions.get(session_id)
            if session is None:
                raise ProtocolError("Unknown session")
            session.websocket = websocket
            return session

    def detach_websocket(self, session_id: str) -> None:
        self._drop_session(session_id)

    def _drop_session(self, session_id: str) -> None:
        with self._session_lock:
            self._sessions.pop(session_id, None)
            stale_queue_ids = [queue_id for queue_id, mapped in self._queue_delivery_map.items() if mapped == session_id]
            for queue_id in stale_queue_ids:
                self._queue_delivery_map.pop(queue_id, None)

    async def send_frame(self, websocket: WebSocket, session_id: str, frame_type: str, payload: dict[str, Any]) -> None:
        frame = {
            "protocol_version": PROTOCOL_VERSION,
            "type": frame_type,
            "session_id": session_id,
            "server_time": utc_now(),
            **payload,
        }
        await websocket.send_json(frame)

    async def _send_to_session(self, session: Optional[SessionInfo], frame_type: str, payload: dict[str, Any]) -> None:
        if session is None or session.websocket is None:
            return
        try:
            await self.send_frame(session.websocket, session.session_id, frame_type, payload)
        except Exception:  # noqa: BLE001
            self.detach_websocket(session.session_id)

    async def stream_handshake(self, session: SessionInfo, websocket: WebSocket) -> None:
        await self.send_frame(
            websocket,
            session.session_id,
            "handshake_snapshot_begin",
            {"thread_id": session.thread_id},
        )
        with self._db() as conn:
            turns = self._fetch_handshake_turns(conn, session.thread_id, session.known_tail_turn_id)
        for turn in turns:
            await self.send_frame(websocket, session.session_id, "committed_turn", {"turn": turn})

        context_messages = [
            Message(role=str(turn["speaker"]), content=str(turn["message_text"]))
            for turn in turns
            if str(turn.get("speaker", "")) in {"assistant", "user", "system"}
        ]
        provider = self._model_provider(configured_default_model())
        model_limits = resolve_model_properties(provider=provider, model=configured_default_model()).limits
        await self.send_frame(
            websocket,
            session.session_id,
            "context_budget",
            {
                "context_size": self._estimate_message_tokens(context_messages),
                "max_context_size": model_limits.context_window_tokens,
            },
        )

        await self.drain_thread_outstanding(session.thread_id)
        await self.send_frame(websocket, session.session_id, "handshake_ready", {})

    def _fetch_handshake_turns(
        self,
        conn: sqlite3.Connection,
        thread_id: str,
        known_tail_turn_id: Optional[str],
    ) -> list[dict[str, Any]]:
        if known_tail_turn_id:
            rows = self._walk_turn_chain(conn, thread_id, stop_at_turn_id=known_tail_turn_id, limit=None)
            if rows is None:
                rows = self._walk_turn_chain(conn, thread_id, stop_at_turn_id=None, limit=20) or []
            return [self._turn_row_to_dict(row) for row in rows]

        rows = self._walk_turn_chain(conn, thread_id, stop_at_turn_id=None, limit=20) or []
        return [self._turn_row_to_dict(row) for row in rows]

    def _walk_turn_chain(
        self,
        conn: sqlite3.Connection,
        thread_id: str,
        *,
        stop_at_turn_id: Optional[str],
        limit: Optional[int],
    ) -> Optional[list[sqlite3.Row]]:
        tail_row = conn.execute("SELECT tail_turn_id FROM threads WHERE id = ?", (thread_id,)).fetchone()
        if tail_row is None or tail_row["tail_turn_id"] is None:
            return []

        rows_reversed: list[sqlite3.Row] = []
        current_id: Optional[str] = str(tail_row["tail_turn_id"])
        seen: set[str] = set()
        found_stop = stop_at_turn_id is None

        while current_id is not None:
            if current_id in seen:
                break
            seen.add(current_id)

            row = conn.execute(
                "SELECT * FROM turns WHERE id = ? AND thread_id = ?",
                (current_id, thread_id),
            ).fetchone()
            if row is None:
                break

            if stop_at_turn_id is not None and current_id == stop_at_turn_id:
                found_stop = True
                break

            rows_reversed.append(row)
            if limit is not None and len(rows_reversed) >= limit:
                break

            prev_turn = row["prev_turn_id"]
            current_id = str(prev_turn) if prev_turn is not None else None

        if stop_at_turn_id is not None and not found_stop:
            return None
        rows_reversed.reverse()
        return rows_reversed

    def _turn_row_to_dict(self, row: sqlite3.Row) -> dict[str, Any]:
        payload: dict[str, Any] = {
            "id": row["id"],
            "thread_id": row["thread_id"],
            "prev_turn_id": row["prev_turn_id"],
            "speaker": row["speaker"],
            "message_text": row["message_text"],
            "model_name": row["model_name"],
            "created_at": row["created_at"],
        }
        if row["stats_json"]:
            try:
                stats = json.loads(row["stats_json"])
                if isinstance(stats, dict) and "tool_calls" in stats:
                    payload["tool_calls"] = stats["tool_calls"]
            except json.JSONDecodeError:
                pass
        return payload

    def enqueue_message(
        self,
        *,
        thread_id: str,
        text: str,
        model_name: str,
        in_response_to_turn_id: Optional[str],
        client_message_id: Optional[str],
        session_id: Optional[str],
    ) -> int:
        self.initialize()
        now = utc_now()
        resolved_model = model_name.strip() or configured_default_model()
        with self._db() as conn:
            exists = conn.execute("SELECT id FROM threads WHERE id = ?", (thread_id,)).fetchone()
            if exists is None:
                raise ProtocolError(f"Unknown thread '{thread_id}'")
            cur = conn.execute(
                """
                INSERT INTO message_queue(
                    thread_id, response_to_turn_id, sender, message_text, model_name,
                    client_message_id, enqueued_at, available_at
                ) VALUES (?, ?, 'user', ?, ?, ?, ?, ?)
                """,
                (thread_id, in_response_to_turn_id, text, resolved_model, client_message_id, now, now),
            )
            conn.commit()
            queue_id = int(cur.lastrowid)

        with self._session_lock:
            self._queue_delivery_map[queue_id] = session_id
        wake = self._worker_wake
        loop = self._worker_loop_event_loop
        if wake is not None:
            if loop is not None:
                loop.call_soon_threadsafe(wake.set)
            else:
                wake.set()
        return queue_id

    def _bind_request_to_queue(self, queue_id: int, request_id: str) -> None:
        with self._session_lock:
            self._queue_request_map[queue_id] = request_id

    def _clear_request_for_queue(self, queue_id: int) -> None:
        with self._session_lock:
            self._queue_request_map.pop(queue_id, None)

    def _request_id_for_queue(self, queue_id: int) -> Optional[str]:
        with self._session_lock:
            return self._queue_request_map.get(queue_id)

    def _mark_request_failed_for_queue(self, queue_id: int, message: str) -> None:
        request_id = self._request_id_for_queue(queue_id)
        if request_id is None:
            return
        with self._db() as conn:
            conn.execute(
                """
                UPDATE requests
                SET error_text = ?, completed_at = COALESCE(completed_at, ?)
                WHERE id = ? AND turn_id IS NULL
                """,
                (message, utc_now(), request_id),
            )
            conn.commit()
        self._clear_request_for_queue(queue_id)

    async def process_next_runnable(self) -> bool:
        self.initialize()
        claimed = self._claim_next_runnable_row()
        if claimed is None:
            return False
        await self._execute_claimed_row(claimed)
        return True

    async def drain_thread_outstanding(self, thread_id: str) -> None:
        while True:
            claimed = self._claim_next_runnable_row(thread_id=thread_id)
            if claimed is None:
                return
            await self._execute_claimed_row(claimed)

    def _claim_next_runnable_row(self, *, thread_id: Optional[str] = None) -> Optional[sqlite3.Row]:
        now = utc_now()
        with self._db() as conn:
            if thread_id is None:
                row = conn.execute(
                    """
                    SELECT id FROM message_queue
                    WHERE locked_by IS NULL AND available_at <= ?
                    ORDER BY available_at ASC, enqueued_at ASC
                    LIMIT 1
                    """,
                    (now,),
                ).fetchone()
            else:
                row = conn.execute(
                    """
                    SELECT id FROM message_queue
                    WHERE thread_id = ? AND locked_by IS NULL AND available_at <= ?
                    ORDER BY available_at ASC, enqueued_at ASC
                    LIMIT 1
                    """,
                    (thread_id, now),
                ).fetchone()

            if row is None:
                return None

            queue_id = int(row["id"])
            updated = conn.execute(
                """
                UPDATE message_queue
                SET locked_by = ?, locked_at = ?
                WHERE id = ? AND locked_by IS NULL
                """,
                (self._worker_id, now, queue_id),
            )
            conn.commit()
            if updated.rowcount == 0:
                return None

            claimed = conn.execute("SELECT * FROM message_queue WHERE id = ?", (queue_id,)).fetchone()
            return claimed

    async def _execute_claimed_row(self, queue_row: sqlite3.Row) -> None:
        queue_id = int(queue_row["id"])
        thread_id = str(queue_row["thread_id"])
        expected_tail = queue_row["response_to_turn_id"]

        session = self._session_for_queue(queue_id=queue_id, thread_id=thread_id)

        with self._db() as conn:
            thread_row = conn.execute("SELECT tail_turn_id FROM threads WHERE id = ?", (thread_id,)).fetchone()

        if thread_row is None:
            await self._move_to_fatal_error(
                queue_row,
                "thread_validation",
                FatalExecutionError(f"Unknown thread '{thread_id}'"),
            )
            return

        actual_tail = thread_row["tail_turn_id"]
        if expected_tail != actual_tail:
            with self._db() as conn:
                conn.execute("DELETE FROM message_queue WHERE id = ?", (queue_id,))
                conn.commit()
            self._mark_request_failed_for_queue(queue_id, "execution_conflict: stale thread tail before execution")
            await self._send_to_session(
                session,
                "execution_conflict",
                {
                    "queue_id": queue_id,
                    "expected_tail_turn_id": expected_tail,
                    "actual_tail_turn_id": actual_tail,
                },
            )
            self._clear_delivery_map(queue_id)
            return

        await self._send_to_session(
            session,
            "turn_event",
            {"queue_id": queue_id, "event": "execution_started"},
        )

        user_turn_id = str(uuid.uuid4())
        assistant_turn_id = str(uuid.uuid4())
        model_name = str(queue_row["model_name"] or configured_default_model())
        request_id = str(uuid.uuid4())

        with self._db() as conn:
            prompt_messages = self._load_thread_messages(conn, thread_id)
        prompt_messages.append(Message(role="user", content=str(queue_row["message_text"])))
        compacted_messages, compaction_info, context_size, max_context_size = await self._maybe_compact_messages(
            prompt_messages=prompt_messages,
            model_name=model_name,
        )
        prompt_messages = compacted_messages
        input_tokens = self._estimate_message_tokens(prompt_messages)
        assistant_turn_context = _json(
            {"messages": [{"role": m.role, "content": m.content} for m in prompt_messages]}
        )

        await self._send_to_session(
            session,
            "context_budget",
            {"context_size": context_size, "max_context_size": max_context_size},
        )
        if compaction_info is not None:
            await self._send_to_session(
                session,
                "turn_event",
                {"queue_id": queue_id, "event": "context_compaction", "payload": compaction_info},
            )

        with self._db() as conn:
            conn.execute(
                """
                INSERT INTO requests(id, turn_id, model_name, request_json, input_tokens, created_at)
                VALUES (?, ?, ?, ?, ?, ?)
                """,
                (
                    request_id,
                    None,
                    model_name,
                    _json(
                        {
                            "model": model_name,
                            "messages": [{"role": m.role, "content": m.content} for m in prompt_messages],
                            "stream": True,
                        }
                    ),
                    input_tokens,
                    utc_now(),
                ),
            )
            conn.commit()
        self._bind_request_to_queue(queue_id, request_id)

        assistant_text, thinking_traces, tool_calls, elapsed_ms = await self._run_generation_with_stream(
            prompt_messages=prompt_messages,
            model_name=model_name,
            queue_id=queue_id,
            session=session,
        )

        if assistant_text is None:
            return

        now = utc_now()
        try:
            with self._db() as conn:
                try:
                    conn.execute("BEGIN IMMEDIATE")
                    latest = conn.execute("SELECT tail_turn_id FROM threads WHERE id = ?", (thread_id,)).fetchone()
                    if latest is None or latest["tail_turn_id"] != expected_tail:
                        conn.execute("ROLLBACK")
                        cleanup_error: Exception | None = None
                        try:
                            conn.execute("DELETE FROM message_queue WHERE id = ?", (queue_id,))
                            conn.commit()
                        except Exception as exc:  # noqa: BLE001
                            cleanup_error = exc
                            conn.rollback()
                        if cleanup_error is not None:
                            raise FatalExecutionError(
                                f"failed to clean up queue row after commit conflict: {cleanup_error}"
                            ) from cleanup_error
                        self._mark_request_failed_for_queue(queue_id, "execution_conflict: tail changed before commit")
                        await self._send_to_session(
                            session,
                            "execution_conflict",
                            {
                                "queue_id": queue_id,
                                "expected_tail_turn_id": expected_tail,
                                "actual_tail_turn_id": latest["tail_turn_id"] if latest else None,
                            },
                        )
                        self._clear_delivery_map(queue_id)
                        return

                    conn.execute(
                        """
                        INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
                        VALUES (?, ?, ?, 'user', ?, NULL, NULL, NULL, ?)
                        """,
                        (user_turn_id, thread_id, expected_tail, str(queue_row["message_text"]), now),
                    )

                    conn.execute(
                        """
                        INSERT INTO turns(id, thread_id, prev_turn_id, speaker, message_text, turn_context, stats_json, model_name, created_at)
                        VALUES (?, ?, ?, 'assistant', ?, ?, ?, ?, ?)
                        """,
                        (
                            assistant_turn_id,
                            thread_id,
                            user_turn_id,
                            assistant_text,
                            assistant_turn_context,
                            _json({"tool_calls": tool_calls}),
                            model_name,
                            now,
                        ),
                    )

                    conn.execute(
                        """
                        INSERT INTO turn_events(turn_id, event_type, payload_json, created_at)
                        VALUES (?, 'model_request', ?, ?)
                        """,
                        (assistant_turn_id, _json({"model_name": model_name, "queue_id": queue_id}), now),
                    )

                    conn.execute(
                        """
                        INSERT INTO turn_events(turn_id, event_type, payload_json, created_at)
                        VALUES (?, 'assistant_stream_complete', ?, ?)
                        """,
                        (assistant_turn_id, _json({"queue_id": queue_id, "token_count": self._estimate_tokens(assistant_text)}), now),
                    )
                    if compaction_info is not None:
                        conn.execute(
                            """
                            INSERT INTO turn_events(turn_id, event_type, payload_json, created_at)
                            VALUES (?, 'context_compaction', ?, ?)
                            """,
                            (assistant_turn_id, _json(compaction_info), now),
                        )

                    for call in tool_calls:
                        conn.execute(
                            """
                            INSERT INTO turn_events(turn_id, event_type, tool_name, tool_args_json, payload_json, created_at)
                            VALUES (?, 'tool_use', ?, ?, ?, ?)
                            """,
                            (
                                assistant_turn_id,
                                call.get("name"),
                                _json(call.get("args", {})),
                                _json(call),
                                now,
                            ),
                        )

                    for idx, trace in enumerate(thinking_traces, start=1):
                        conn.execute(
                            """
                            INSERT INTO thinking_traces(turn_id, sequence_no, trace_text, created_at)
                            VALUES (?, ?, ?, ?)
                            """,
                            (assistant_turn_id, idx, trace, now),
                        )

                    conn.execute(
                        "UPDATE threads SET tail_turn_id = ?, updated_at = ? WHERE id = ?",
                        (assistant_turn_id, now, thread_id),
                    )
                    conn.execute("DELETE FROM message_queue WHERE id = ?", (queue_id,))
                    conn.execute(
                        "UPDATE requests SET turn_id = ?, response_json = ?, output_tokens = ?, latency_ms = ?, completed_at = ? WHERE id = ?",
                        (
                            assistant_turn_id,
                            _json({"response": assistant_text, "tool_calls": tool_calls}),
                            self._estimate_tokens(assistant_text),
                            elapsed_ms,
                            utc_now(),
                            request_id,
                        ),
                    )
                    conn.execute("COMMIT")
                    self._clear_request_for_queue(queue_id)
                except Exception:
                    try:
                        conn.rollback()
                    except sqlite3.Error:
                        pass
                    raise
        except Exception as exc:  # noqa: BLE001
            if self._is_fatal_error(exc):
                await self._move_to_fatal_error(queue_row, "commit", exc)
            else:
                await self._retry_nonfatal(queue_row, exc)
            return

        await self._send_to_session(
            session,
            "committed_turn",
            {
                "turn": {
                    "id": user_turn_id,
                    "thread_id": thread_id,
                    "prev_turn_id": expected_tail,
                    "speaker": "user",
                    "message_text": str(queue_row["message_text"]),
                    "model_name": None,
                    "created_at": now,
                }
            },
        )
        await self._send_to_session(
            session,
            "committed_turn",
            {
                "turn": {
                    "id": assistant_turn_id,
                    "thread_id": thread_id,
                    "prev_turn_id": user_turn_id,
                    "speaker": "assistant",
                    "message_text": assistant_text,
                    "model_name": model_name,
                    "created_at": now,
                    "tool_calls": tool_calls,
                }
            },
        )
        await self._send_to_session(session, "turn_finalized", {"queue_id": queue_id, "turn_id": assistant_turn_id})
        post_commit_context_size = self._estimate_message_tokens(
            [*prompt_messages, Message(role="assistant", content=assistant_text)]
        )
        await self._send_to_session(
            session,
            "context_budget",
            {"context_size": post_commit_context_size, "max_context_size": max_context_size},
        )
        self._clear_delivery_map(queue_id)

    async def _run_generation_with_stream(
        self,
        *,
        prompt_messages: list[Message],
        model_name: str,
        queue_id: int,
        session: Optional[SessionInfo],
    ) -> tuple[Optional[str], list[str], list[dict[str, Any]], int]:
        token_queue: asyncio.Queue[tuple[str, Any]] = asyncio.Queue()
        loop = asyncio.get_running_loop()
        started = time.perf_counter()

        def _on_token(token: str) -> None:
            loop.call_soon_threadsafe(token_queue.put_nowait, ("token", token))

        def _on_thinking(trace: str) -> None:
            loop.call_soon_threadsafe(token_queue.put_nowait, ("thinking", trace))

        def _run_sync() -> tuple[str, list[dict[str, Any]]]:
            dispatcher = build_dispatcher(model_override=model_name)
            result = dispatcher.stream_generate_with_details(
                prompt_messages,
                on_token=_on_token,
                on_thinking=_on_thinking,
                enable_tools=True,
            )
            calls = [
                {
                    "id": item.id,
                    "name": item.name,
                    "args": item.args,
                    "result": item.result,
                    "is_error": item.is_error,
                }
                for item in result.tool_calls
            ]
            return result.response, calls

        worker = asyncio.create_task(asyncio.to_thread(_run_sync))

        full: list[str] = []
        traces: list[str] = []
        try:
            while True:
                if worker.done() and token_queue.empty():
                    break
                try:
                    kind, value = await asyncio.wait_for(token_queue.get(), timeout=0.1)
                except asyncio.TimeoutError:
                    continue
                if kind == "token":
                    token = str(value)
                    full.append(token)
                    await self._send_to_session(
                        session,
                        "assistant_token",
                        {"queue_id": queue_id, "chunk": token},
                    )
                    if len(full) % 25 == 0:
                        trace = f"streamed_{len(full)}_chunks"
                        await self._send_to_session(
                            session,
                            "turn_event",
                            {
                                "queue_id": queue_id,
                                "event": "thinking_trace",
                                "trace": trace,
                                "trace_kind": "operational",
                            },
                        )
                elif kind == "thinking":
                    trace = str(value)
                    is_reasoning = self._is_reasoning_trace(trace)
                    if is_reasoning:
                        traces.append(trace)
                    await self._send_to_session(
                        session,
                        "turn_event",
                        {
                            "queue_id": queue_id,
                            "event": "thinking_trace",
                            "trace": trace,
                            "trace_kind": "model_reasoning" if is_reasoning else "operational",
                        },
                    )

            response, tool_calls = await worker
            elapsed_ms = int((time.perf_counter() - started) * 1000)
            return response, traces, tool_calls, elapsed_ms
        except Exception as exc:  # noqa: BLE001
            worker.cancel()
            if self._is_fatal_error(exc):
                await self._move_to_fatal_error_from_values(
                    queue_id=queue_id,
                    stage="generation",
                    error=exc,
                )
                return None, [], [], 0
            await self._retry_nonfatal_by_id(queue_id, exc)
            await self._send_to_session(
                session,
                "error",
                {"queue_id": queue_id, "message": str(exc)},
            )
            return None, [], [], 0

    def _is_fatal_error(self, exc: Exception) -> bool:
        fatal_types: tuple[type[BaseException], ...] = (
            ProtocolError,
            FatalExecutionError,
            UnsupportedModelError,
            ProviderConfigurationError,
        )
        openai_types = tuple(
            t
            for t in (
                OpenAIAuthenticationError,
                OpenAIBadRequestError,
                OpenAINotFoundError,
                OpenAIPermissionDeniedError,
            )
            if t is not None
        )
        return isinstance(exc, fatal_types + openai_types)

    def _is_reasoning_trace(self, trace: str) -> bool:
        operational_prefixes = (
            "openai_request_started_round_",
            "openai_request_completed_round_",
            "tool_call_",
            "ollama_request_started",
            "ollama_request_completed",
        )
        if trace.startswith(operational_prefixes):
            return False
        if _STREAM_PROGRESS_TRACE.match(trace):
            return False
        return True

    async def _retry_nonfatal(self, queue_row: sqlite3.Row, exc: Exception) -> None:
        queue_id = int(queue_row["id"])
        attempts = int(queue_row["attempts"]) + 1
        delay = min(_RETRY_MAX_SECONDS, _RETRY_BASE_SECONDS * (2 ** max(0, attempts - 1)))
        with self._db() as conn:
            conn.execute(
                """
                UPDATE message_queue
                SET attempts = ?,
                    available_at = ?,
                    locked_by = NULL,
                    locked_at = NULL,
                    last_error = ?
                WHERE id = ?
                """,
                (attempts, utc_after(delay), str(exc), queue_id),
            )
            conn.commit()
        self._mark_request_failed_for_queue(queue_id, f"retry_scheduled: {exc}")

        session = self._session_for_queue(queue_id=queue_id, thread_id=str(queue_row["thread_id"]))
        await self._send_to_session(
            session,
            "turn_event",
            {
                "queue_id": queue_id,
                "event": "retry_scheduled",
                "attempt": attempts,
                "retry_in_seconds": delay,
                "error": str(exc),
            },
        )

    async def _retry_nonfatal_by_id(self, queue_id: int, exc: Exception) -> None:
        with self._db() as conn:
            row = conn.execute("SELECT * FROM message_queue WHERE id = ?", (queue_id,)).fetchone()
        if row is None:
            return
        await self._retry_nonfatal(row, exc)

    async def _move_to_fatal_error(self, queue_row: sqlite3.Row, stage: str, error: Exception) -> None:
        queue_id = int(queue_row["id"])
        self._mark_request_failed_for_queue(queue_id, f"fatal_error[{stage}]: {error}")
        with self._db() as conn:
            conn.execute(
                """
                INSERT INTO queue_errors(
                    queue_id, thread_id, response_to_turn_id, model_name, message_text,
                    attempts, error_type, error_text, failure_stage, created_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    queue_id,
                    str(queue_row["thread_id"]),
                    queue_row["response_to_turn_id"],
                    str(queue_row["model_name"]),
                    str(queue_row["message_text"]),
                    int(queue_row["attempts"]),
                    error.__class__.__name__,
                    str(error),
                    stage,
                    utc_now(),
                ),
            )
            conn.execute("DELETE FROM message_queue WHERE id = ?", (queue_id,))
            conn.commit()

        session = self._session_for_queue(queue_id=queue_id, thread_id=str(queue_row["thread_id"]))
        await self._send_to_session(
            session,
            "error",
            {
                "queue_id": queue_id,
                "message": str(error),
                "fatal": True,
            },
        )
        self._clear_delivery_map(queue_id)

    async def _move_to_fatal_error_from_values(self, *, queue_id: int, stage: str, error: Exception) -> None:
        with self._db() as conn:
            row = conn.execute("SELECT * FROM message_queue WHERE id = ?", (queue_id,)).fetchone()
        if row is not None:
            await self._move_to_fatal_error(row, stage, error)

    def _session_for_queue(self, *, queue_id: int, thread_id: str) -> Optional[SessionInfo]:
        with self._session_lock:
            session_id = self._queue_delivery_map.get(queue_id)
            if session_id is not None:
                session = self._sessions.get(session_id)
                if session is not None and session.websocket is not None:
                    return session
            for session in self._sessions.values():
                if session.thread_id == thread_id and session.websocket is not None:
                    return session
        return None

    def _clear_delivery_map(self, queue_id: int) -> None:
        with self._session_lock:
            self._queue_delivery_map.pop(queue_id, None)

    def _load_thread_messages(self, conn: sqlite3.Connection, thread_id: str) -> list[Message]:
        rows = self._walk_turn_chain(conn, thread_id, stop_at_turn_id=None, limit=None) or []
        anchor_idx, base_messages = self._nearest_context_anchor(rows)

        out: list[Message] = []
        if base_messages is None:
            system_prompt = self._load_active_system_prompt()
            if system_prompt:
                out.append(Message(role="system", content=system_prompt))
            start_idx = 0
        else:
            out.extend(base_messages)
            start_idx = anchor_idx if anchor_idx is not None else len(rows)

        for row in rows[start_idx:]:
            speaker = str(row["speaker"])
            if speaker in {"assistant", "user", "system"}:
                out.append(Message(role=speaker, content=str(row["message_text"])))
        return out

    def _nearest_context_anchor(self, rows: list[sqlite3.Row]) -> tuple[Optional[int], Optional[list[Message]]]:
        for idx in range(len(rows) - 1, -1, -1):
            raw_context = rows[idx]["turn_context"]
            if not isinstance(raw_context, str) or not raw_context.strip():
                continue
            parsed = self._parse_turn_context_messages(raw_context)
            if parsed is not None:
                return idx, parsed
        return None, None

    def _parse_turn_context_messages(self, raw_context: str) -> Optional[list[Message]]:
        try:
            payload = json.loads(raw_context)
        except json.JSONDecodeError:
            return None
        if not isinstance(payload, dict):
            return None
        raw_messages = payload.get("messages")
        if not isinstance(raw_messages, list):
            return None
        out: list[Message] = []
        for item in raw_messages:
            if not isinstance(item, dict):
                return None
            role = item.get("role")
            content = item.get("content")
            if not isinstance(role, str) or role not in {"assistant", "user", "system"}:
                return None
            if not isinstance(content, str):
                return None
            out.append(Message(role=role, content=content))
        return out

    def _ensure_default_system_prompt(self) -> None:
        default_path = SYSTEM_PROMPTS_DIR / "sheaf_default.md"
        if default_path.exists():
            return
        default_path.write_text(_DEFAULT_SYSTEM_PROMPT.strip() + "\n", encoding="utf-8")

    def _load_active_system_prompt(self) -> str:
        configured_name = Path(configured_system_prompt_file()).name
        configured_path = SYSTEM_PROMPTS_DIR / configured_name
        if configured_path.exists():
            try:
                return configured_path.read_text(encoding="utf-8").strip()
            except OSError:
                return ""

        fallback = SYSTEM_PROMPTS_DIR / "sheaf_default.md"
        if fallback.exists():
            try:
                return fallback.read_text(encoding="utf-8").strip()
            except OSError:
                return ""
        return ""

    def _model_provider(self, model_name: str) -> str:
        descriptor = get_model_registry().resolve_model(model_name, allow_refresh=False)
        if descriptor is not None and descriptor.provider:
            return str(descriptor.provider)
        return "openai"

    def _estimate_tokens(self, text: str) -> int:
        # Deterministic approximation to avoid provider-specific tokenizers in worker hot path.
        return max(1, (len(text) + 3) // 4)

    def _estimate_message_tokens(self, messages: list[Message]) -> int:
        return sum(self._estimate_tokens(message.content) + 4 for message in messages)

    def _build_summary(self, messages: list[Message]) -> str:
        parts: list[str] = []
        for message in messages:
            flattened = " ".join(message.content.split())
            if len(flattened) > 240:
                flattened = flattened[:240] + "..."
            parts.append(f"{message.role}: {flattened}")
        return " | ".join(parts)

    async def _maybe_compact_messages(
        self,
        *,
        prompt_messages: list[Message],
        model_name: str,
    ) -> tuple[list[Message], Optional[dict[str, Any]], int, int]:
        provider = self._model_provider(model_name)
        properties = resolve_model_properties(provider=provider, model=model_name)
        limits = properties.limits
        max_context_size = limits.context_window_tokens
        original_context_size = self._estimate_message_tokens(prompt_messages)

        trigger = int(max_context_size * limits.compaction_trigger_ratio)
        if original_context_size <= trigger or len(prompt_messages) <= 2:
            return prompt_messages, None, original_context_size, max_context_size

        keep_recent = max(1, limits.recent_messages_to_keep)
        if keep_recent >= len(prompt_messages):
            keep_recent = len(prompt_messages) - 1
        dropped = prompt_messages[:-keep_recent]
        kept = prompt_messages[-keep_recent:]
        summary = await self._summarize_for_compaction(dropped=dropped, model_name=model_name)
        summary_source = "llm"
        if not summary:
            summary = self._build_summary(dropped)
            summary_source = "deterministic_fallback"
        summary_message = Message(
            role="system",
            content=(
                "Context compaction summary of earlier turns: "
                f"{summary}"
            ),
        )
        compacted = [summary_message, *kept]

        target = int(max_context_size * limits.compaction_target_ratio)
        compacted_size = self._estimate_message_tokens(compacted)
        if compacted_size > target and summary_message.content:
            overflow = compacted_size - target
            trim_chars = overflow * 4
            summary_content = summary_message.content
            min_len = 80
            if len(summary_content) > min_len:
                clipped = summary_content[: max(min_len, len(summary_content) - trim_chars)]
                if len(clipped) < len(summary_content):
                    clipped = clipped.rstrip() + "..."
                summary_message = Message(role="system", content=clipped)
                compacted = [summary_message, *kept]
                compacted_size = self._estimate_message_tokens(compacted)

        payload = {
            "original_context_size": original_context_size,
            "compacted_context_size": compacted_size,
            "max_context_size": max_context_size,
            "dropped_message_count": len(dropped),
            "kept_recent_message_count": len(kept),
            "trigger_ratio": limits.compaction_trigger_ratio,
            "target_ratio": limits.compaction_target_ratio,
            "summary_source": summary_source,
        }
        return compacted, payload, compacted_size, max_context_size

    async def _summarize_for_compaction(self, *, dropped: list[Message], model_name: str) -> Optional[str]:
        if not dropped:
            return None
        loop = asyncio.get_running_loop()

        def _run() -> Optional[str]:
            dispatcher = build_dispatcher(model_override=model_name)
            generator = getattr(dispatcher, "generate", None)
            if not callable(generator):
                return None
            transcript = "\n".join(f"{m.role}: {m.content}" for m in dropped)
            prompt = [
                Message(
                    role="system",
                    content=(
                        "Summarize the conversation transcript for future context retention. "
                        "Preserve facts, decisions, constraints, and open tasks. "
                        "Do not fabricate information."
                    ),
                ),
                Message(role="user", content=transcript),
            ]
            summary = generator(prompt, enable_tools=False)
            text = summary.strip()
            return text or None

        try:
            return await asyncio.to_thread(_run)
        except Exception:  # noqa: BLE001
            return None


runtime = RewriteRuntime()
