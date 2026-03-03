"""Chainlit UI for sheaf chat server."""

from __future__ import annotations

import json
import os
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin
from urllib.request import Request, urlopen

import chainlit as cl
from chainlit.chat_context import chat_context

BASE_URL = os.getenv("SHEAF_API_BASE_URL", "http://127.0.0.1:2731")
HISTORY_PREVIEW_SIZE = int(os.getenv("SHEAF_CHAT_PREVIEW_MESSAGES", "12"))


def _post_json(url: str, data: dict[str, Any] | None = None) -> dict[str, Any]:
    body = json.dumps(data or {}).encode("utf-8")
    req = Request(url, data=body, headers={"Content-Type": "application/json"}, method="POST")
    with urlopen(req, timeout=30) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _get_json(url: str) -> dict[str, Any]:
    req = Request(url, headers={"Accept": "application/json"}, method="GET")
    with urlopen(req, timeout=30) as resp:
        return json.loads(resp.read().decode("utf-8"))


def create_chat() -> str:
    response = _post_json(urljoin(BASE_URL, "/chats"))
    chat_id = response.get("chat_id")
    if not isinstance(chat_id, str) or not chat_id:
        raise RuntimeError(f"Invalid create response: {response}")
    return chat_id


def list_chats() -> list[dict[str, Any]]:
    response = _get_json(urljoin(BASE_URL, "/chats"))
    chats = response.get("chats")
    if not isinstance(chats, list):
        raise RuntimeError(f"Invalid list response: {response}")
    return [item for item in chats if isinstance(item, dict)]


def send_message(chat_id: str, message: str) -> str:
    response = _post_json(
        urljoin(BASE_URL, f"/chats/{chat_id}/messages"),
        {"message": message},
    )
    text = response.get("response")
    if not isinstance(text, str):
        raise RuntimeError(f"Invalid message response: {response}")
    return text


def get_chat_metadata(chat_id: str) -> dict[str, Any]:
    response = _get_json(urljoin(BASE_URL, f"/chats/{chat_id}/metadata"))
    if not isinstance(response, dict):
        raise RuntimeError(f"Invalid metadata response: {response}")
    return response


def get_message_range(chat_id: str, start: int, end: int) -> list[dict[str, Any]]:
    response = _get_json(urljoin(BASE_URL, f"/chats/{chat_id}/messages?start={start}&end={end}"))
    messages = response.get("messages") if isinstance(response, dict) else None
    if not isinstance(messages, list):
        raise RuntimeError(f"Invalid messages response: {response}")
    return [item for item in messages if isinstance(item, dict)]


async def _clear_canvas() -> None:
    # Remove currently rendered messages before switching chats.
    for msg in chat_context.get():
        try:
            await msg.remove()
        except Exception:
            pass
    chat_context.clear()


async def _hydrate_canvas(chat_id: str) -> None:
    try:
        meta = get_chat_metadata(chat_id)
    except HTTPError as exc:
        # Lazy chat creation: metadata may not exist until first message is sent.
        if exc.code == 404:
            await cl.Message(content=f"Active chat: `{chat_id}`").send()
            return
        raise
    raw_count = meta.get("message_count", 0)
    if isinstance(raw_count, int):
        total = raw_count
    elif isinstance(raw_count, str) and raw_count.isdigit():
        total = int(raw_count)
    else:
        total = 0
    start = max(0, total - HISTORY_PREVIEW_SIZE)
    messages = get_message_range(chat_id, start=start, end=total)
    if not messages:
        await cl.Message(content=f"Chat `{chat_id}` has no messages yet.").send()
        return
    for item in messages:
        role = item.get("role")
        content = item.get("content")
        if not isinstance(role, str) or not isinstance(content, str):
            continue
        if role == "user":
            await cl.Message(content=content, author="you", type="user_message").send()
        else:
            await cl.Message(content=content, author="sheaf", type="assistant_message").send()


async def _switch_chat(target: str) -> bool:
    try:
        chats = list_chats()
    except (HTTPError, URLError, TimeoutError, RuntimeError):
        return False

    known = {item.get("chat_id") for item in chats if isinstance(item.get("chat_id"), str)}
    if target not in known:
        return False

    cl.user_session.set("chat_id", target)
    await _clear_canvas()
    try:
        await _hydrate_canvas(target)
        await cl.send_window_message({"type": "sheaf_active_chat", "chat_id": target})
    except (HTTPError, URLError, TimeoutError, RuntimeError) as exc:
        await cl.Message(content=f"Failed to load history for `{target}`: {exc}").send()
    return True


@cl.on_chat_start
async def on_chat_start() -> None:
    try:
        chat_id = create_chat()
        cl.user_session.set("chat_id", chat_id)
        await _hydrate_canvas(chat_id)
    except (HTTPError, URLError, TimeoutError, RuntimeError) as exc:
        await cl.Message(content=f"Failed to start chat: {exc}").send()


@cl.on_window_message
async def on_window_message(payload: Any) -> None:
    if not isinstance(payload, dict):
        return

    msg_type = payload.get("type")
    if msg_type == "sheaf_switch_chat":
        target = payload.get("chat_id")
        if not isinstance(target, str) or not target:
            return
        switched = await _switch_chat(target)
        if not switched:
            await cl.Message(content=f"Chat not found: `{target}`").send()
        return

    if msg_type == "sheaf_new_chat":
        try:
            chat_id = create_chat()
            cl.user_session.set("chat_id", chat_id)
            await _clear_canvas()
            await _hydrate_canvas(chat_id)
            await cl.send_window_message({"type": "sheaf_active_chat", "chat_id": chat_id})
        except (HTTPError, URLError, TimeoutError, RuntimeError) as exc:
            await cl.Message(content=f"Failed to create chat: {exc}").send()


@cl.on_message
async def on_message(message: cl.Message) -> None:
    text = message.content.strip()
    current_chat_id = cl.user_session.get("chat_id")

    if text == "/new":
        try:
            chat_id = create_chat()
            cl.user_session.set("chat_id", chat_id)
            await _clear_canvas()
            await _hydrate_canvas(chat_id)
            await cl.send_window_message({"type": "sheaf_active_chat", "chat_id": chat_id})
        except (HTTPError, URLError, TimeoutError, RuntimeError) as exc:
            await cl.Message(content=f"Failed to create chat: {exc}").send()
        return

    if text == "/list":
        try:
            chats = list_chats()
        except (HTTPError, URLError, TimeoutError, RuntimeError) as exc:
            await cl.Message(content=f"Failed to list chats: {exc}").send()
            return

        if not chats:
            await cl.Message(content="No chats found.").send()
            return

        lines = ["Chats:"]
        for item in chats:
            chat_id = item.get("chat_id")
            if not isinstance(chat_id, str):
                continue
            marker = " (current)" if chat_id == current_chat_id else ""
            lines.append(f"- `{chat_id}`{marker}")
        await cl.Message(content="\n".join(lines)).send()
        return

    if text.startswith("/use "):
        target = text[len("/use ") :].strip()
        if not target:
            await cl.Message(content="Usage: `/use <chat_id>`").send()
            return
        switched = await _switch_chat(target)
        if not switched:
            await cl.Message(content=f"Chat not found: `{target}`").send()
            return

        return

    if not current_chat_id:
        try:
            current_chat_id = create_chat()
            cl.user_session.set("chat_id", current_chat_id)
            await _clear_canvas()
            await _hydrate_canvas(current_chat_id)
            await cl.send_window_message({"type": "sheaf_active_chat", "chat_id": current_chat_id})
        except (HTTPError, URLError, TimeoutError, RuntimeError) as exc:
            await cl.Message(content=f"No active chat and failed to create one: {exc}").send()
            return

    try:
        reply = send_message(current_chat_id, text)
    except (HTTPError, URLError, TimeoutError, RuntimeError) as exc:
        await cl.Message(content=f"Request failed: {exc}").send()
        return

    await cl.Message(content=reply).send()
