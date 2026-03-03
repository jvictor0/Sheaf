#!/usr/bin/env python3
"""Poll Zulip messages, route through Sheaf, and post responses back."""

from __future__ import annotations

import base64
import argparse
import json
import re
import sqlite3
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _normalize_base_url(url: str) -> str:
    return url.rstrip("/")


def _slug(value: str) -> str:
    lowered = value.strip().lower()
    out = re.sub(r"[^a-z0-9]+", "-", lowered).strip("-")
    return out or "unknown"


def _narrow_for_register(narrow: list[dict[str, str]]) -> list[list[str]]:
    # Zulip /register expects narrow terms as legacy pair lists, not operator objects.
    return [[item["operator"], item["operand"]] for item in narrow]


@dataclass(frozen=True)
class Config:
    zulip_site: str
    zulip_email: str
    zulip_api_key: str
    sheaf_api_base_url: str
    sheaf_chat_id: str
    state_db_path: Path
    poll_seconds: float
    batch_size: int
    narrow: list[dict[str, str]]
    process_backlog_on_first_run: bool

    @staticmethod
    def from_file(path: Path) -> "Config":
        if not path.exists():
            raise ValueError(f"Config file not found: {path}")
        raw = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(raw, dict):
            raise ValueError("Config file must contain a JSON object")

        def required_str(key: str) -> str:
            value = raw.get(key)
            if not isinstance(value, str) or not value.strip():
                raise ValueError(f"Missing required config field: {key}")
            return value.strip()

        narrow_default = [{"operator": "is", "operand": "mentioned"}]
        narrow_raw = raw.get("narrow", narrow_default)
        if not isinstance(narrow_raw, list):
            raise ValueError("Config field 'narrow' must be a JSON list")
        narrow: list[dict[str, str]] = []
        for item in narrow_raw:
            if not isinstance(item, dict):
                raise ValueError("Each 'narrow' element must be an object")
            op = str(item.get("operator", "")).strip()
            operand = str(item.get("operand", "")).strip()
            if not op or not operand:
                raise ValueError("Each 'narrow' object needs operator and operand")
            narrow.append({"operator": op, "operand": operand})

        sheaf_chat = raw.get("sheaf_chat_id", "")
        if sheaf_chat is None:
            sheaf_chat = ""
        if not isinstance(sheaf_chat, str):
            raise ValueError("Config field 'sheaf_chat_id' must be a string")

        state_db = raw.get("state_db_path", "data/zulip_bot_state.sqlite3")
        if not isinstance(state_db, str) or not state_db.strip():
            raise ValueError("Config field 'state_db_path' must be a non-empty string")

        poll_seconds = raw.get("poll_seconds", 2.0)
        batch_size = raw.get("batch_size", 100)
        process_backlog = raw.get("process_backlog_on_first_run", False)
        if not isinstance(process_backlog, bool):
            raise ValueError("Config field 'process_backlog_on_first_run' must be true or false")

        return Config(
            zulip_site=_normalize_base_url(required_str("zulip_site")),
            zulip_email=required_str("zulip_bot_email"),
            zulip_api_key=required_str("zulip_bot_api_key"),
            sheaf_api_base_url=_normalize_base_url(str(raw.get("sheaf_api_base_url", "http://127.0.0.1:2731")).strip()),
            sheaf_chat_id=sheaf_chat.strip(),
            state_db_path=Path(state_db),
            poll_seconds=float(poll_seconds),
            batch_size=max(1, int(batch_size)),
            narrow=narrow,
            process_backlog_on_first_run=process_backlog,
        )


class BotState:
    def __init__(self, db_path: Path) -> None:
        self._db_path = db_path
        self._db_path.parent.mkdir(parents=True, exist_ok=True)
        self._conn = sqlite3.connect(str(db_path))
        self._conn.row_factory = sqlite3.Row
        self._initialize_schema()

    def close(self) -> None:
        self._conn.close()

    def _initialize_schema(self) -> None:
        with self._conn:
            self._conn.execute(
                """
                CREATE TABLE IF NOT EXISTS state (
                    key TEXT PRIMARY KEY,
                    value TEXT NOT NULL
                )
                """
            )
            self._conn.execute(
                """
                CREATE TABLE IF NOT EXISTS processed_messages (
                    message_id INTEGER PRIMARY KEY,
                    status TEXT NOT NULL,
                    attempts INTEGER NOT NULL DEFAULT 0,
                    last_error TEXT,
                    updated_at TEXT NOT NULL
                )
                """
            )

    def get_state(self, key: str) -> str | None:
        row = self._conn.execute("SELECT value FROM state WHERE key = ?", (key,)).fetchone()
        return None if row is None else str(row["value"])

    def set_state(self, key: str, value: str) -> None:
        with self._conn:
            self._conn.execute(
                """
                INSERT INTO state(key, value) VALUES(?, ?)
                ON CONFLICT(key) DO UPDATE SET value=excluded.value
                """,
                (key, value),
            )

    def get_last_message_id(self) -> int | None:
        raw = self.get_state("last_message_id")
        if raw is None:
            return None
        try:
            return int(raw)
        except ValueError:
            return None

    def set_last_message_id(self, message_id: int) -> None:
        self.set_state("last_message_id", str(message_id))

    def get_last_event_id(self) -> int | None:
        raw = self.get_state("last_event_id")
        if raw is None:
            return None
        try:
            return int(raw)
        except ValueError:
            return None

    def set_last_event_id(self, event_id: int) -> None:
        self.set_state("last_event_id", str(event_id))

    def status_for_message(self, message_id: int) -> str | None:
        row = self._conn.execute(
            "SELECT status FROM processed_messages WHERE message_id = ?", (message_id,)
        ).fetchone()
        return None if row is None else str(row["status"])

    def mark_processing(self, message_id: int) -> None:
        with self._conn:
            self._conn.execute(
                """
                INSERT INTO processed_messages(message_id, status, attempts, updated_at)
                VALUES(?, 'processing', 1, ?)
                ON CONFLICT(message_id)
                DO UPDATE SET
                    status='processing',
                    attempts=processed_messages.attempts + 1,
                    updated_at=excluded.updated_at,
                    last_error=NULL
                """,
                (message_id, _utc_now()),
            )

    def mark_done(self, message_id: int) -> None:
        with self._conn:
            self._conn.execute(
                """
                INSERT INTO processed_messages(message_id, status, attempts, updated_at)
                VALUES(?, 'done', 1, ?)
                ON CONFLICT(message_id)
                DO UPDATE SET
                    status='done',
                    updated_at=excluded.updated_at,
                    last_error=NULL
                """,
                (message_id, _utc_now()),
            )

    def mark_failed(self, message_id: int, error_text: str) -> None:
        with self._conn:
            self._conn.execute(
                """
                INSERT INTO processed_messages(message_id, status, attempts, last_error, updated_at)
                VALUES(?, 'failed', 1, ?, ?)
                ON CONFLICT(message_id)
                DO UPDATE SET
                    status='failed',
                    last_error=excluded.last_error,
                    updated_at=excluded.updated_at
                """,
                (message_id, error_text[:2000], _utc_now()),
            )


class ZulipClient:
    def __init__(self, site: str, email: str, api_key: str) -> None:
        self._base = site
        token = base64.b64encode(f"{email}:{api_key}".encode("utf-8")).decode("ascii")
        self._auth_header = f"Basic {token}"

    def _request(self, method: str, path: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        url = f"{self._base}{path}"
        payload: bytes | None = None
        headers = {
            "Authorization": self._auth_header,
            "User-Agent": "sheaf-zulip-poller/0.1",
        }

        if params is not None:
            encoded = urllib.parse.urlencode(params)
            if method.upper() == "GET":
                sep = "&" if "?" in url else "?"
                url = f"{url}{sep}{encoded}"
            else:
                payload = encoded.encode("utf-8")
                headers["Content-Type"] = "application/x-www-form-urlencoded"

        req = urllib.request.Request(url=url, data=payload, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                body = resp.read().decode("utf-8")
        except urllib.error.HTTPError as exc:
            body = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"Zulip HTTP {exc.code} for {path}: {body}") from exc
        except urllib.error.URLError as exc:
            raise RuntimeError(f"Zulip request failed for {path}: {exc}") from exc

        try:
            data = json.loads(body)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"Zulip returned non-JSON for {path}") from exc

        if data.get("result") != "success":
            msg = str(data.get("msg", "unknown error"))
            code = str(data.get("code", ""))
            raise ZulipAPIError(path=path, code=code, message=msg)

        return data

    def register_queue(self, narrow: list[dict[str, str]]) -> tuple[str, int]:
        data = self._request(
            "POST",
            "/api/v1/register",
            {
                "event_types": json.dumps(["message"]),
                "narrow": json.dumps(_narrow_for_register(narrow)),
            },
        )
        queue_id = str(data.get("queue_id", "")).strip()
        last_event_id_raw = data.get("last_event_id")
        if not queue_id:
            raise RuntimeError("Zulip register did not return queue_id")
        if not isinstance(last_event_id_raw, int):
            raise RuntimeError("Zulip register did not return numeric last_event_id")
        return queue_id, last_event_id_raw

    def get_events(self, queue_id: str, last_event_id: int) -> list[dict[str, Any]]:
        data = self._request(
            "GET",
            "/api/v1/events",
            {
                "queue_id": queue_id,
                "last_event_id": str(last_event_id),
                "dont_block": "false",
            },
        )
        events = data.get("events", [])
        if not isinstance(events, list):
            raise RuntimeError("Zulip returned invalid events payload")
        return [e for e in events if isinstance(e, dict)]

    def get_messages(self, anchor: int, num_after: int, narrow: list[dict[str, str]]) -> list[dict[str, Any]]:
        params = {
            "anchor": str(anchor),
            "num_before": "0",
            "num_after": str(num_after),
            "include_anchor": "false",
            "apply_markdown": "false",
            "narrow": json.dumps(narrow),
        }
        data = self._request("GET", "/api/v1/messages", params)
        messages = data.get("messages", [])
        if not isinstance(messages, list):
            raise RuntimeError("Zulip returned invalid messages payload")
        return [m for m in messages if isinstance(m, dict)]

    def get_newest_message_id(self, narrow: list[dict[str, str]]) -> int | None:
        data = self._request(
            "GET",
            "/api/v1/messages",
            {
                "anchor": "newest",
                "num_before": "0",
                "num_after": "1",
                "include_anchor": "true",
                "apply_markdown": "false",
                "narrow": json.dumps(narrow),
            },
        )
        messages = data.get("messages", [])
        if not isinstance(messages, list) or not messages:
            return None
        first = messages[0]
        if not isinstance(first, dict):
            return None
        message_id = first.get("id")
        if not isinstance(message_id, int):
            return None
        return message_id

    def send_reply(self, incoming: dict[str, Any], content: str) -> None:
        message_type = str(incoming.get("type", "")).strip()
        if message_type == "stream":
            stream = incoming.get("display_recipient")
            if not isinstance(stream, str) or not stream:
                raise RuntimeError("Incoming stream message missing display_recipient")
            topic = str(incoming.get("topic") or incoming.get("subject") or "")
            self._request(
                "POST",
                "/api/v1/messages",
                {
                    "type": "stream",
                    "to": stream,
                    "topic": topic,
                    "content": content,
                },
            )
            return

        if message_type == "private":
            sender_email = str(incoming.get("sender_email", "")).strip()
            if not sender_email:
                raise RuntimeError("Incoming private message missing sender_email")
            self._request(
                "POST",
                "/api/v1/messages",
                {
                    "type": "private",
                    "to": json.dumps([sender_email]),
                    "content": content,
                },
            )
            return

        raise RuntimeError(f"Unsupported message type: {message_type}")


@dataclass(frozen=True)
class ZulipAPIError(RuntimeError):
    path: str
    code: str
    message: str

    def __str__(self) -> str:
        if self.code:
            return f"Zulip API error for {self.path} ({self.code}): {self.message}"
        return f"Zulip API error for {self.path}: {self.message}"


def _http_json_request(method: str, url: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
    data_bytes: bytes | None = None
    headers = {"User-Agent": "sheaf-zulip-poller/0.1"}
    if payload is not None:
        data_bytes = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"

    req = urllib.request.Request(url=url, data=data_bytes, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            body = resp.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code} for {url}: {body}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Request failed for {url}: {exc}") from exc

    try:
        return json.loads(body)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Expected JSON response from {url}") from exc


def chat_id_for_message(config: Config, message: dict[str, Any]) -> str:
    fixed = config.sheaf_chat_id.strip()
    if fixed:
        return fixed

    message_type = str(message.get("type", "")).strip()
    if message_type == "stream":
        stream_name = str(message.get("display_recipient", "")).strip()
        stream_id = message.get("stream_id")
        if isinstance(stream_id, int):
            return f"zulip-stream-{stream_id}-{_slug(stream_name)}"
        return f"zulip-stream-{_slug(stream_name)}"

    if message_type == "private":
        recipient_id = message.get("recipient_id")
        if isinstance(recipient_id, int):
            return f"zulip-dm-{recipient_id}"
        sender_email = str(message.get("sender_email", "")).strip()
        return f"zulip-dm-{_slug(sender_email)}"

    return "zulip-fallback"


def call_sheaf(config: Config, chat_id: str, message_text: str) -> str:
    url = f"{config.sheaf_api_base_url}/chats/{chat_id}/messages"
    payload = {"message": message_text}
    data = _http_json_request("POST", url, payload)
    response = str(data.get("response", "")).strip()
    if not response:
        raise RuntimeError("Sheaf returned an empty response")
    return response


def _should_ignore_message(message: dict[str, Any], bot_email: str) -> bool:
    sender_email = str(message.get("sender_email", "")).strip().lower()
    if not sender_email:
        return True
    return sender_email == bot_email.lower()


def _log(msg: str) -> None:
    print(f"[{_utc_now()}] {msg}", flush=True)


def _bootstrap_last_message_id(config: Config, state: BotState, zulip: ZulipClient) -> int:
    last_id = state.get_last_message_id()
    if last_id is not None:
        return last_id

    if config.process_backlog_on_first_run:
        state.set_last_message_id(0)
        _log("No checkpoint found; starting from oldest available message (backlog mode).")
        return 0

    newest = zulip.get_newest_message_id(config.narrow)
    if newest is None:
        state.set_last_message_id(0)
        _log("No matching Zulip messages yet; initialized last_message_id=0.")
        return 0

    state.set_last_message_id(newest)
    _log(f"No checkpoint found; starting from newest message id={newest}.")
    return newest


def _process_one_message(
    *,
    message: dict[str, Any],
    bot_email: str,
    zulip: ZulipClient,
    config: Config,
    state: BotState,
    last_message_id: int,
) -> tuple[int, bool]:
    message_id_raw = message.get("id")
    if not isinstance(message_id_raw, int):
        return last_message_id, True
    message_id = message_id_raw

    if _should_ignore_message(message, bot_email):
        advanced = max(last_message_id, message_id)
        state.set_last_message_id(advanced)
        return advanced, True

    status = state.status_for_message(message_id)
    if status == "done":
        advanced = max(last_message_id, message_id)
        state.set_last_message_id(advanced)
        return advanced, True

    content = str(message.get("content", "")).strip()
    if not content:
        advanced = max(last_message_id, message_id)
        state.set_last_message_id(advanced)
        return advanced, True

    state.mark_processing(message_id)
    try:
        chat_id = chat_id_for_message(config, message)
        reply = call_sheaf(config, chat_id, content)
        zulip.send_reply(message, reply)
    except Exception as exc:  # noqa: BLE001
        state.mark_failed(message_id, str(exc))
        _log(f"Failed processing message_id={message_id}: {exc}")
        return last_message_id, False

    state.mark_done(message_id)
    advanced = max(last_message_id, message_id)
    state.set_last_message_id(advanced)
    _log(f"Processed message_id={message_id}")
    return advanced, True


def _catch_up_messages(config: Config, state: BotState, zulip: ZulipClient, last_message_id: int) -> int:
    cursor = last_message_id
    while True:
        messages = zulip.get_messages(
            anchor=cursor,
            num_after=config.batch_size,
            narrow=config.narrow,
        )
        messages.sort(key=lambda m: int(m.get("id", 0)))
        if not messages:
            return cursor

        for message in messages:
            cursor, ok = _process_one_message(
                message=message,
                bot_email=config.zulip_email,
                zulip=zulip,
                config=config,
                state=state,
                last_message_id=cursor,
            )
            if not ok:
                return cursor


def run_loop(config: Config) -> None:
    state = BotState(config.state_db_path)
    zulip = ZulipClient(config.zulip_site, config.zulip_email, config.zulip_api_key)

    try:
        if config.sheaf_chat_id.strip():
            _log(f"Using fixed Sheaf chat_id={config.sheaf_chat_id.strip()}")
        else:
            _log("Using Zulip-derived Sheaf chat IDs (stream/DM scoped).")

        last_message_id: int | None = state.get_last_message_id()
        last_event_id = state.get_last_event_id()
        queue_id = ""

        while True:
            try:
                if last_message_id is None:
                    last_message_id = _bootstrap_last_message_id(config, state, zulip)

                if not queue_id:
                    queue_id, server_last_event_id = zulip.register_queue(config.narrow)
                    last_event_id = server_last_event_id
                    state.set_last_event_id(last_event_id)
                    _log(f"Registered Zulip queue_id={queue_id} from last_event_id={last_event_id}")

                    # Catch up from message history before processing live queue events.
                    last_message_id = _catch_up_messages(config, state, zulip, last_message_id)

                events = zulip.get_events(queue_id=queue_id, last_event_id=last_event_id)
                if not events:
                    continue

                for event in events:
                    event_id = event.get("id")
                    if not isinstance(event_id, int):
                        continue

                    event_type = str(event.get("type", "")).strip()
                    if event_type != "message":
                        last_event_id = event_id
                        state.set_last_event_id(last_event_id)
                        continue

                    message = event.get("message")
                    if not isinstance(message, dict):
                        last_event_id = event_id
                        state.set_last_event_id(last_event_id)
                        continue

                    last_message_id, ok = _process_one_message(
                        message=message,
                        bot_email=config.zulip_email,
                        zulip=zulip,
                        config=config,
                        state=state,
                        last_message_id=last_message_id,
                    )
                    if not ok:
                        time.sleep(max(config.poll_seconds, 1.0))
                        break

                    last_event_id = event_id
                    state.set_last_event_id(last_event_id)

            except Exception as exc:  # noqa: BLE001
                if isinstance(exc, ZulipAPIError) and exc.code in {"BAD_EVENT_QUEUE_ID"}:
                    _log("Zulip event queue expired; re-registering queue.")
                    queue_id = ""
                    continue
                _log(f"Loop error: {exc}")
                time.sleep(max(config.poll_seconds, 2.0))

    finally:
        state.close()


def main() -> None:
    parser = argparse.ArgumentParser(description="Poll Zulip messages and route them to Sheaf.")
    parser.add_argument(
        "--config",
        default="sheaf_server.config",
        help="Path to JSON config file (default: sheaf_server.config)",
    )
    args = parser.parse_args()

    try:
        config = Config.from_file(Path(args.config))
    except Exception as exc:  # noqa: BLE001
        print(f"Configuration error: {exc}", file=sys.stderr)
        raise SystemExit(2) from exc

    _log(f"Starting Zulip poller with DB={config.state_db_path}")
    try:
        run_loop(config)
    except KeyboardInterrupt:
        _log("Shutting down on Ctrl+C")


if __name__ == "__main__":
    main()
