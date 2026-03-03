"""Simple CLI loop for sheaf chat API."""

from __future__ import annotations

import json
import sys
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin
from urllib.request import Request, urlopen

DEFAULT_BASE_URL = "http://127.0.0.1:2731"


def _post_json(url: str, data: dict[str, object] | None = None) -> dict[str, object]:
    body = json.dumps(data or {}).encode("utf-8")
    req = Request(url, data=body, headers={"Content-Type": "application/json"}, method="POST")
    with urlopen(req, timeout=10) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _get_json(url: str) -> dict[str, object]:
    req = Request(url, headers={"Accept": "application/json"}, method="GET")
    with urlopen(req, timeout=10) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _create_chat(base_url: str) -> str:
    response = _post_json(urljoin(base_url, "/chats"))
    chat_id = response.get("chat_id")
    if not isinstance(chat_id, str) or not chat_id:
        raise RuntimeError(f"Invalid create chat response: {response}")
    return chat_id


def _send_message(base_url: str, chat_id: str, message: str) -> str:
    response = _post_json(
        urljoin(base_url, f"/chats/{chat_id}/messages"),
        {"message": message},
    )
    text = response.get("response")
    if not isinstance(text, str):
        raise RuntimeError(f"Invalid chat response: {response}")
    return text


def _list_chats(base_url: str) -> list[dict[str, object]]:
    response = _get_json(urljoin(base_url, "/chats"))
    chats = response.get("chats")
    if not isinstance(chats, list):
        raise RuntimeError(f"Invalid list chats response: {response}")
    return [item for item in chats if isinstance(item, dict)]


def main() -> None:
    base_url = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BASE_URL
    print(f"sheaf client -> {base_url}")

    try:
        chat_id = _create_chat(base_url)
    except (HTTPError, URLError, TimeoutError, RuntimeError) as exc:
        print(f"failed to create chat: {exc}")
        raise SystemExit(1) from exc

    print(f"chat created: {chat_id}")
    print(
        "type '/new' to create a new chat, '/list' to list chats, "
        "'/use <chat_id>' to switch chats, '/quit' to exit"
    )

    while True:
        try:
            user_text = input("you> ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\nbye")
            break

        if not user_text:
            continue
        if user_text == "/quit":
            print("bye")
            break
        if user_text == "/new":
            try:
                chat_id = _create_chat(base_url)
            except (HTTPError, URLError, TimeoutError, RuntimeError) as exc:
                print(f"failed to create chat: {exc}")
                continue
            print(f"chat created: {chat_id}")
            continue
        if user_text == "/list":
            try:
                chats = _list_chats(base_url)
            except (HTTPError, URLError, TimeoutError, RuntimeError) as exc:
                print(f"failed to list chats: {exc}")
                continue
            if not chats:
                print("no chats found")
                continue
            print("chats:")
            for chat in chats:
                cid = chat.get("chat_id")
                if not isinstance(cid, str):
                    continue
                marker = " (current)" if cid == chat_id else ""
                print(f"- {cid}{marker}")
            continue
        if user_text.startswith("/use "):
            target_chat = user_text[len("/use ") :].strip()
            if not target_chat:
                print("usage: /use <chat_id>")
                continue
            try:
                chats = _list_chats(base_url)
            except (HTTPError, URLError, TimeoutError, RuntimeError) as exc:
                print(f"failed to list chats: {exc}")
                continue
            known_ids = {
                str(chat.get("chat_id"))
                for chat in chats
                if isinstance(chat.get("chat_id"), str)
            }
            if target_chat not in known_ids:
                print(f"chat not found: {target_chat}")
                continue
            chat_id = target_chat
            print(f"switched to chat: {chat_id}")
            continue

        try:
            reply = _send_message(base_url, chat_id, user_text)
        except (HTTPError, URLError, TimeoutError, RuntimeError) as exc:
            print(f"request failed: {exc}")
            continue

        print(f"sheaf> {reply}")


if __name__ == "__main__":
    main()
