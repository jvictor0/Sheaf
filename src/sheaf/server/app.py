"""FastAPI app entrypoint for rewritten sheaf server."""

from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from typing import Optional

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel

from sheaf.server.runtime import (
    HEARTBEAT_SECONDS,
    PROTOCOL_VERSION,
    ProtocolError,
    runtime,
)
from sheaf.server.replica import (
    REPLICA_PROTOCOL_VERSION,
    ReplicaProtocolError,
    ReplicaService,
)
from sheaf.config.settings import load_server_config

@asynccontextmanager
async def _lifespan(_app: FastAPI):
    runtime.initialize()
    await runtime.start_worker()
    try:
        yield
    finally:
        await runtime.stop_worker()


app = FastAPI(title="sheaf", version="0.2.0", lifespan=_lifespan)
replica_service = ReplicaService()

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


class CreateThreadRequest(BaseModel):
    name: Optional[str] = None
    thread_id: Optional[str] = None
    prev_thread_id: Optional[str] = None
    start_turn_id: Optional[str] = None


class EnterChatRequest(BaseModel):
    protocol_version: int
    known_tail_turn_id: Optional[str] = None


class EnterChatResponse(BaseModel):
    session_id: str
    websocket_url: str
    accepted_protocol_version: int


class ArchiveResponse(BaseModel):
    status: str


class CreateVaultRequest(BaseModel):
    root_path: str
    metadata_json: Optional[str] = None


class StartReplicaSessionRequest(BaseModel):
    vault_name: str
    next_lsn: int = 0
    create_if_missing: bool = False
    root_path: Optional[str] = None
    metadata_json: Optional[str] = None


class StartReplicaSessionResponse(BaseModel):
    session_id: str
    websocket_url: str
    accepted_protocol_version: int
    vault_id: int
    vault_name: str
    root_path: str
    created: bool
    next_lsn: int


@app.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok"}


@app.get("/models")
def list_models_endpoint() -> dict[str, object]:
    return {"models": runtime.list_models()}


@app.post("/models/updateLocalModelList")
def update_local_model_list() -> dict[str, object]:
    models = runtime.refresh_local_models()
    return {"count": len(models), "models": models}


@app.post("/threads")
def create_thread_endpoint(payload: Optional[CreateThreadRequest] = None) -> dict[str, str]:
    args = payload or CreateThreadRequest()
    try:
        thread_id = runtime.create_thread(
            thread_id=args.thread_id,
            name=args.name,
            prev_thread_id=args.prev_thread_id,
            start_turn_id=args.start_turn_id,
        )
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    return {"thread_id": thread_id}


@app.post("/vaults")
def create_vault_endpoint(payload: CreateVaultRequest) -> dict[str, object]:
    try:
        return runtime.create_vault(root_path=payload.root_path, metadata_json=payload.metadata_json)
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@app.post("/replica/sessions", response_model=StartReplicaSessionResponse)
def start_replica_session(payload: StartReplicaSessionRequest) -> StartReplicaSessionResponse:
    try:
        session = replica_service.start_session(
            vault_name=payload.vault_name,
            next_lsn=payload.next_lsn,
            create_if_missing=payload.create_if_missing,
            root_path=payload.root_path,
            metadata_json=payload.metadata_json,
        )
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    return StartReplicaSessionResponse(
        session_id=str(session["session_id"]),
        websocket_url=f"/ws/replica/{session['session_id']}",
        accepted_protocol_version=REPLICA_PROTOCOL_VERSION,
        vault_id=int(session["vault_id"]),
        vault_name=str(session["vault_name"]),
        root_path=str(session["root_path"]),
        created=bool(session["created"]),
        next_lsn=int(session["next_lsn"]),
    )


@app.get("/threads")
def list_threads_endpoint() -> dict[str, object]:
    return {"threads": runtime.list_threads()}


@app.post("/threads/{thread_id}/archive", response_model=ArchiveResponse)
def archive_thread_endpoint(thread_id: str) -> ArchiveResponse:
    runtime.archive_thread(thread_id, archived=True)
    return ArchiveResponse(status="archived")


@app.post("/threads/{thread_id}/unarchive", response_model=ArchiveResponse)
def unarchive_thread_endpoint(thread_id: str) -> ArchiveResponse:
    runtime.archive_thread(thread_id, archived=False)
    return ArchiveResponse(status="unarchived")


@app.post("/threads/{thread_id}/enter-chat", response_model=EnterChatResponse)
def enter_chat(thread_id: str, payload: EnterChatRequest) -> EnterChatResponse:
    if payload.protocol_version != PROTOCOL_VERSION:
        raise HTTPException(
            status_code=409,
            detail=f"Unsupported protocol_version={payload.protocol_version}; expected {PROTOCOL_VERSION}",
        )
    session = runtime.create_session(thread_id, payload.known_tail_turn_id)
    return EnterChatResponse(
        session_id=session.session_id,
        websocket_url=f"/ws/chat/{session.session_id}",
        accepted_protocol_version=PROTOCOL_VERSION,
    )


async def _heartbeat_task(websocket: WebSocket, session_id: str) -> None:
    while True:
        await asyncio.sleep(HEARTBEAT_SECONDS)
        await runtime.send_frame(websocket, session_id, "heartbeat", {"interval_seconds": HEARTBEAT_SECONDS})


@app.websocket("/ws/chat/{session_id}")
async def chat_ws(websocket: WebSocket, session_id: str) -> None:
    await websocket.accept()
    try:
        session = runtime.attach_websocket(session_id, websocket)
    except ProtocolError:
        await runtime.send_frame(
            websocket,
            session_id,
            "error",
            {"message": "Unknown session. Call enter-chat first."},
        )
        await websocket.close(code=1008)
        return

    heartbeat = asyncio.create_task(_heartbeat_task(websocket, session_id))
    try:
        await runtime.stream_handshake(session, websocket)
        while True:
            message = await websocket.receive_json()
            frame_type = str(message.get("type", ""))
            if frame_type != "submit_message":
                await runtime.send_frame(
                    websocket,
                    session_id,
                    "error",
                    {"message": f"Unsupported frame type '{frame_type}'"},
                )
                continue

            protocol_version = int(message.get("protocol_version", 0))
            if protocol_version != PROTOCOL_VERSION:
                await runtime.send_frame(
                    websocket,
                    session_id,
                    "error",
                    {"message": f"protocol_version mismatch; expected {PROTOCOL_VERSION}"},
                )
                continue

            try:
                queue_id = runtime.enqueue_message(
                    thread_id=str(message.get("thread_id", session.thread_id)),
                    text=str(message.get("text", "")).strip(),
                    model_name=str(message.get("model_name", "")).strip(),
                    in_response_to_turn_id=message.get("in_response_to_turn_id"),
                    client_message_id=message.get("client_message_id"),
                    session_id=session.session_id,
                )
            except Exception as exc:  # noqa: BLE001
                await runtime.send_frame(
                    websocket,
                    session_id,
                    "error",
                    {"message": str(exc)},
                )
                continue

            await runtime.send_frame(
                websocket,
                session_id,
                "message_durable_ack",
                {
                    "queue_id": queue_id,
                    "client_message_id": message.get("client_message_id"),
                },
            )
    except WebSocketDisconnect:
        return
    finally:
        heartbeat.cancel()
        runtime.detach_websocket(session_id)


@app.websocket("/ws/replica/{session_id}")
async def replica_ws(websocket: WebSocket, session_id: str) -> None:
    await websocket.accept()
    try:
        session = replica_service.attach_websocket(session_id, websocket)
    except ReplicaProtocolError:
        await replica_service.send_frame(
            websocket,
            session_id,
            "error",
            {"message": "Unknown replica session. Call POST /replica/sessions first."},
        )
        await websocket.close(code=1008)
        return

    try:
        await replica_service.stream_session(session)
    except WebSocketDisconnect:
        return
    finally:
        replica_service.detach_websocket(session_id)


def main() -> None:
    import uvicorn

    config = load_server_config()
    host = "127.0.0.1"
    port = 2731
    try:
        server = config.get("server", {})
        if isinstance(server, dict):
            parsed_host = server.get("host", "127.0.0.1")
            if isinstance(parsed_host, str) and parsed_host.strip():
                host = parsed_host.strip()
            parsed_port = int(server.get("api_port", 2731))
            if 1 <= parsed_port <= 65535:
                port = parsed_port
    except (TypeError, ValueError):
        pass
    uvicorn.run("sheaf.server.app:app", host=host, port=port, reload=True)


if __name__ == "__main__":
    main()
