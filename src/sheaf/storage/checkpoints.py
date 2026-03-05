"""Chat storage backed by LangGraph SQLite checkpoints."""

from __future__ import annotations

import json
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

from langchain_core.messages import HumanMessage
from langgraph.checkpoint.sqlite import SqliteSaver

from sheaf.agent.langgraph_runtime import compile_chat_graph
from sheaf.config.settings import CHATS_DIR, ensure_data_dirs


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _chat_dir(chat_id: str) -> Path:
    return CHATS_DIR / chat_id


def _checkpoints_dir(chat_id: str) -> Path:
    return _chat_dir(chat_id) / "checkpoints"


def _chat_meta_path(chat_id: str) -> Path:
    return _chat_dir(chat_id) / "chat.json"


def _db_path(chat_id: str) -> Path:
    return _checkpoints_dir(chat_id) / "langgraph.sqlite"


def _read_json(path: Path) -> dict[str, object]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    return raw if isinstance(raw, dict) else {}


def _message_content_to_text(content: object) -> str:
    if isinstance(content, str):
        return content
    return str(content)


def _ensure_chat_initialized(chat_id: str) -> None:
    ensure_data_dirs()
    cdir = _chat_dir(chat_id)
    cpdir = _checkpoints_dir(chat_id)
    cpdir.mkdir(parents=True, exist_ok=True)

    meta_file = _chat_meta_path(chat_id)
    if not meta_file.exists():
        now = _utc_now()
        meta = {
            "chat_id": chat_id,
            "created_at": now,
            "updated_at": now,
        }
        meta_file.write_text(json.dumps(meta, indent=2), encoding="utf-8")


def _require_chat_initialized(chat_id: str) -> None:
    if not _chat_meta_path(chat_id).exists():
        raise FileNotFoundError(f"Chat not found: {chat_id}")


def create_chat(chat_id: Optional[str] = None, *, eager: bool = False) -> str:
    ensure_data_dirs()
    resolved_chat_id = chat_id or str(uuid.uuid4())

    if eager:
        if _chat_meta_path(resolved_chat_id).exists():
            raise FileExistsError(f"Chat already exists: {resolved_chat_id}")
        _ensure_chat_initialized(resolved_chat_id)

    # Lazy mode: allocate only an ID. Directory/checkpoint DB are created at first message.
    return resolved_chat_id


def list_chats() -> list[dict[str, str]]:
    ensure_data_dirs()
    result: list[dict[str, str]] = []
    if not CHATS_DIR.exists():
        return result

    for path in sorted(CHATS_DIR.iterdir()):
        if not path.is_dir():
            continue
        meta_file = path / "chat.json"
        if not meta_file.exists():
            continue
        raw = _read_json(meta_file)
        result.append(
            {
                "chat_id": str(raw.get("chat_id", path.name)),
                "created_at": str(raw.get("created_at", "")),
                "updated_at": str(raw.get("updated_at", "")),
            }
        )
    return result


def _load_indexed_messages(chat_id: str) -> tuple[list[dict[str, object]], str]:
    _require_chat_initialized(chat_id)
    db = _db_path(chat_id)
    conn = str(db)
    cfg = {"configurable": {"thread_id": chat_id}}

    with SqliteSaver.from_conn_string(conn) as saver:
        graph = compile_chat_graph(saver=saver)
        snapshot = graph.get_state(cfg)

    values = snapshot.values or {}
    raw_messages = values.get("messages", []) if isinstance(values, dict) else []
    out: list[dict[str, object]] = []
    if isinstance(raw_messages, list):
        for idx, msg in enumerate(raw_messages):
            role = getattr(msg, "type", "system")
            if role == "human":
                mapped = "user"
            elif role == "ai":
                mapped = "assistant"
            else:
                mapped = "system"

            tool_calls: list[dict[str, object]] = []
            if mapped == "assistant":
                raw_kwargs = getattr(msg, "additional_kwargs", {})
                if isinstance(raw_kwargs, dict):
                    raw_calls = raw_kwargs.get("tool_calls_made", [])
                    if isinstance(raw_calls, list):
                        for item in raw_calls:
                            if not isinstance(item, dict):
                                continue
                            args = item.get("args", {})
                            tool_calls.append(
                                {
                                    "id": str(item.get("id", "")),
                                    "name": str(item.get("name", "")),
                                    "args": args if isinstance(args, dict) else {},
                                    "result": str(item.get("result", "")),
                                    "is_error": bool(item.get("is_error", False)),
                                }
                            )

            out.append(
                {
                    "index": idx,
                    "role": mapped,
                    "content": _message_content_to_text(getattr(msg, "content", "")),
                    "tool_calls": tool_calls,
                }
            )

    cfg_out = snapshot.config if isinstance(snapshot.config, dict) else {}
    cfg_inner = cfg_out.get("configurable") if isinstance(cfg_out, dict) else {}
    checkpoint_id = ""
    if isinstance(cfg_inner, dict):
        checkpoint_id = str(cfg_inner.get("checkpoint_id", ""))

    return out, checkpoint_id


def get_chat_metadata(chat_id: str) -> dict[str, object]:
    _require_chat_initialized(chat_id)
    meta = _read_json(_chat_meta_path(chat_id))
    messages, checkpoint_id = _load_indexed_messages(chat_id)
    return {
        "chat_id": str(meta.get("chat_id", chat_id)),
        "created_at": str(meta.get("created_at", "")),
        "updated_at": str(meta.get("updated_at", "")),
        "latest_checkpoint_id": checkpoint_id,
        "message_count": len(messages),
    }


def get_message_range(chat_id: str, start: int, end: int) -> dict[str, object]:
    messages, _ = _load_indexed_messages(chat_id)
    total = len(messages)
    clamped_start = max(0, min(start, total))
    clamped_end = max(clamped_start, min(end, total))
    return {
        "chat_id": chat_id,
        "start": clamped_start,
        "end": clamped_end,
        "total": total,
        "messages": messages[clamped_start:clamped_end],
    }


def run_chat_turn(chat_id: str, user_message: str) -> tuple[str, str, list[dict[str, object]]]:
    _ensure_chat_initialized(chat_id)

    db = _db_path(chat_id)
    conn = str(db)
    cfg = {"configurable": {"thread_id": chat_id}}

    with SqliteSaver.from_conn_string(conn) as saver:
        graph = compile_chat_graph(saver=saver)
        out = graph.invoke({"messages": [HumanMessage(content=user_message)]}, cfg)
        snapshot = graph.get_state(cfg)

    messages = out.get("messages", []) if isinstance(out, dict) else []
    assistant_text = ""
    tool_calls: list[dict[str, object]] = []
    if isinstance(messages, list):
        for msg in reversed(messages):
            if getattr(msg, "type", "") == "ai":
                assistant_text = _message_content_to_text(getattr(msg, "content", ""))
                raw_kwargs = getattr(msg, "additional_kwargs", {})
                if isinstance(raw_kwargs, dict):
                    raw_calls = raw_kwargs.get("tool_calls_made", [])
                    if isinstance(raw_calls, list):
                        for item in raw_calls:
                            if isinstance(item, dict):
                                tool_calls.append(
                                    {
                                        "id": str(item.get("id", "")),
                                        "name": str(item.get("name", "")),
                                        "args": item.get("args", {})
                                        if isinstance(item.get("args", {}), dict)
                                        else {},
                                        "result": str(item.get("result", "")),
                                        "is_error": bool(item.get("is_error", False)),
                                    }
                                )
                break

    if not assistant_text:
        raise RuntimeError("LangGraph did not produce an assistant response")

    cfg_out = snapshot.config if isinstance(snapshot.config, dict) else {}
    cfg_inner = cfg_out.get("configurable") if isinstance(cfg_out, dict) else {}
    checkpoint_id = ""
    if isinstance(cfg_inner, dict):
        checkpoint_id = str(cfg_inner.get("checkpoint_id", ""))

    meta_file = _chat_meta_path(chat_id)
    raw = _read_json(meta_file)
    raw["updated_at"] = _utc_now()
    meta_file.write_text(json.dumps(raw, indent=2), encoding="utf-8")

    return assistant_text, checkpoint_id, tool_calls
