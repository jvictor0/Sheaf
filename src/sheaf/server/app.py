"""FastAPI app entrypoint for sheaf server."""

from __future__ import annotations

import os
import uuid
from datetime import datetime, timezone
from pathlib import Path

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel

from sheaf.storage.checkpoints import (
    create_chat,
    get_chat_metadata,
    get_message_range,
    list_chats,
    run_chat_turn,
)

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


class ChatMessageResponse(BaseModel):
    chat_id: str
    response: str
    checkpoint_id: str


def _validate_chat_id(chat_id: str) -> None:
    try:
        uuid.UUID(chat_id)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=f"Invalid chat_id: {chat_id}") from exc


@app.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok"}


@app.post("/chats")
def create_chat_endpoint() -> dict[str, str]:
    return {"chat_id": create_chat()}


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
    reboot_file = os.getenv("SHEAF_REBOOT_FILE", "").strip()
    if not reboot_file:
        raise HTTPException(status_code=503, detail="Reboot is unavailable: no supervisor configured.")

    path = Path(reboot_file)
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(datetime.now(timezone.utc).isoformat(), encoding="utf-8")
    except OSError as exc:
        raise HTTPException(status_code=500, detail=f"Failed to request reboot: {exc}") from exc

    return {"status": "reboot_requested"}


@app.post("/chats/{chat_id}/messages", response_model=ChatMessageResponse)
def chat(chat_id: str, payload: ChatMessageRequest) -> ChatMessageResponse:
    _validate_chat_id(chat_id)
    try:
        assistant_text, checkpoint_id = run_chat_turn(chat_id, payload.message)
    except RuntimeError as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc

    return ChatMessageResponse(chat_id=chat_id, response=assistant_text, checkpoint_id=checkpoint_id)


def main() -> None:
    import uvicorn

    port = int(os.getenv("SHEAF_PORT", "2731"))
    uvicorn.run("sheaf.server.app:app", host="127.0.0.1", port=port, reload=True)


if __name__ == "__main__":
    main()
