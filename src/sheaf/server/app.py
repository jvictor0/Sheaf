"""FastAPI app entrypoint for sheaf server."""

from __future__ import annotations

import re
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, HTTPException, Request
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel

from sheaf.storage.checkpoints import (
    create_chat,
    get_chat_metadata,
    get_message_range,
    list_chats,
    run_chat_turn,
)
from sheaf.config.settings import REBOOT_REQUEST_FILE

app = FastAPI(title="sheaf", version="0.1.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=[
        "http://127.0.0.1:2732",
        "http://localhost:2732",
    ],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


class ChatMessageRequest(BaseModel):
    message: str


class CreateChatRequest(BaseModel):
    name: Optional[str] = None


class ToolCallResponse(BaseModel):
    id: str
    name: str
    args: dict[str, object]
    result: str
    is_error: bool


class ChatMessageResponse(BaseModel):
    chat_id: str
    response: str
    checkpoint_id: str
    tool_calls: list[ToolCallResponse]


CHAT_ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
IDEMPOTENCY_TTL_SECONDS = 30 * 60
_idempotency_lock = threading.Lock()
_idempotent_responses: dict[tuple[str, str], tuple[float, ChatMessageResponse]] = {}


def _validate_chat_id(chat_id: str) -> None:
    # Backward compatible: UUIDs are valid.
    try:
        uuid.UUID(chat_id)
        return
    except ValueError:
        pass

    # Allow readable IDs for integrations (for example Zulip stream-scoped chats).
    if CHAT_ID_PATTERN.match(chat_id):
        return

    raise HTTPException(status_code=400, detail=f"Invalid chat_id: {chat_id}")


def _prune_idempotency_cache(now: float) -> None:
    stale_keys = [
        key for key, (timestamp, _) in _idempotent_responses.items()
        if now - timestamp > IDEMPOTENCY_TTL_SECONDS
    ]
    for key in stale_keys:
        _idempotent_responses.pop(key, None)


@app.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok"}


@app.post("/chats")
def create_chat_endpoint(payload: Optional[CreateChatRequest] = None) -> dict[str, str]:
    requested_name = (payload.name if payload else None) or ""
    requested_name = requested_name.strip()
    if not requested_name:
        return {"chat_id": create_chat()}

    _validate_chat_id(requested_name)
    try:
        chat_id = create_chat(chat_id=requested_name, eager=True)
    except FileExistsError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc

    return {"chat_id": chat_id}


@app.get("/chats")
def list_chats_endpoint() -> dict[str, list[dict[str, str]]]:
    return {"chats": list_chats()}


@app.get("/chats/{chat_id}/metadata")
def chat_metadata_endpoint(chat_id: str) -> dict[str, object]:
    _validate_chat_id(chat_id)
    try:
        return get_chat_metadata(chat_id)
    except FileNotFoundError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc


@app.get("/chats/{chat_id}/messages")
def chat_messages_range_endpoint(chat_id: str, start: int = 0, end: int = 20) -> dict[str, object]:
    _validate_chat_id(chat_id)
    if start < 0 or end < 0:
        raise HTTPException(status_code=400, detail="start and end must be non-negative")
    if end < start:
        raise HTTPException(status_code=400, detail="end must be >= start")
    try:
        return get_message_range(chat_id, start=start, end=end)
    except FileNotFoundError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc


@app.post("/admin/reboot")
def reboot_services_endpoint() -> dict[str, str]:
    path = REBOOT_REQUEST_FILE
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(datetime.now(timezone.utc).isoformat(), encoding="utf-8")
    except OSError as exc:
        raise HTTPException(status_code=500, detail=f"Failed to request reboot: {exc}") from exc

    return {"status": "reboot_requested"}


@app.post("/chats/{chat_id}/messages", response_model=ChatMessageResponse)
def chat(chat_id: str, payload: ChatMessageRequest, request: Request) -> ChatMessageResponse:
    _validate_chat_id(chat_id)
    idempotency_key = request.headers.get("x-idempotency-key")
    cache_key = (chat_id, idempotency_key) if idempotency_key else None
    now = time.time()

    if cache_key:
        with _idempotency_lock:
            _prune_idempotency_cache(now)
            cached = _idempotent_responses.get(cache_key)
            if cached is not None:
                return cached[1]

    try:
        assistant_text, checkpoint_id, tool_calls = run_chat_turn(chat_id, payload.message)
    except RuntimeError as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc

    response = ChatMessageResponse(
        chat_id=chat_id,
        response=assistant_text,
        checkpoint_id=checkpoint_id,
        tool_calls=tool_calls,
    )
    if cache_key:
        with _idempotency_lock:
            _prune_idempotency_cache(now)
            _idempotent_responses[cache_key] = (now, response)

    return response


def main() -> None:
    import uvicorn

    config_path = Path(__file__).resolve().parents[3] / "sheaf_server.config"
    port = 2731
    if config_path.exists():
        try:
            import json

            raw = json.loads(config_path.read_text(encoding="utf-8"))
            if isinstance(raw, dict):
                server = raw.get("server", {})
                if isinstance(server, dict):
                    parsed = int(server.get("api_port", 2731))
                    if 1 <= parsed <= 65535:
                        port = parsed
        except (OSError, json.JSONDecodeError, TypeError, ValueError):
            pass
    uvicorn.run("sheaf.server.app:app", host="127.0.0.1", port=port, reload=True)


if __name__ == "__main__":
    main()
