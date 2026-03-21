from __future__ import annotations

import sys
import types

from fastapi.testclient import TestClient

import sheaf.server.app as server_app


def test_create_and_list_threads_round_trip() -> None:
    client = TestClient(server_app.app)

    created = client.post("/threads", json={"name": "Roadmap Thread"})
    assert created.status_code == 200
    thread_id = created.json()["thread_id"]

    listed = client.get("/threads")
    assert listed.status_code == 200
    payload = listed.json()
    threads = payload["threads"]
    thread_ids = {item["thread_id"] for item in threads}
    assert thread_id in thread_ids
    created_thread = next(item for item in threads if item["thread_id"] == thread_id)
    assert created_thread["name"] == "Roadmap Thread"


def test_enter_chat_rejects_bad_protocol_version() -> None:
    client = TestClient(server_app.app)
    created = client.post("/threads", json={})
    thread_id = created.json()["thread_id"]

    response = client.post(
        f"/threads/{thread_id}/enter-chat",
        json={"protocol_version": 999, "known_tail_turn_id": None},
    )

    assert response.status_code == 409
    assert "Unsupported protocol_version" in response.json()["detail"]


def test_enter_chat_returns_session_for_supported_protocol() -> None:
    client = TestClient(server_app.app)
    created = client.post("/threads", json={})
    thread_id = created.json()["thread_id"]

    response = client.post(
        f"/threads/{thread_id}/enter-chat",
        json={"protocol_version": 1, "known_tail_turn_id": None},
    )

    assert response.status_code == 200
    body = response.json()
    assert body["session_id"]
    assert body["websocket_url"].startswith("/ws/chat/")
    assert body["accepted_protocol_version"] == 1


def test_create_thread_response_uses_canonical_thread_id_only() -> None:
    client = TestClient(server_app.app)
    response = client.post("/threads", json={"name": "Canonical"})
    assert response.status_code == 200
    payload = response.json()
    assert "thread_id" in payload
    assert "chat_id" not in payload


def test_models_endpoint_returns_model_objects() -> None:
    client = TestClient(server_app.app)

    response = client.get("/models")
    assert response.status_code == 200
    payload = response.json()
    assert isinstance(payload.get("models"), list)
    assert any(item.get("name") == "gpt-5-mini" for item in payload["models"])
    first = payload["models"][0]
    assert isinstance(first.get("metadata_json"), (str, type(None)))


def test_list_threads_endpoint_calls_runtime_once(monkeypatch) -> None:
    calls = {"count": 0}

    def _fake_list_threads():
        calls["count"] += 1
        return [{"thread_id": "t1", "name": "Thread 1"}]

    monkeypatch.setattr(server_app.runtime, "list_threads", _fake_list_threads)
    payload = server_app.list_threads_endpoint()

    assert calls["count"] == 1
    assert payload["threads"] == [{"thread_id": "t1", "name": "Thread 1"}]
    assert "chats" not in payload


def test_app_main_uses_configured_host_and_port(monkeypatch) -> None:
    seen: dict[str, object] = {}

    def _fake_run(*args, **kwargs):
        seen["args"] = args
        seen["kwargs"] = kwargs

    monkeypatch.setitem(sys.modules, "uvicorn", types.SimpleNamespace(run=_fake_run))
    monkeypatch.setattr(
        server_app,
        "load_server_config",
        lambda: {"server": {"host": "0.0.0.0", "api_port": 3842}},
    )

    server_app.main()

    kwargs = seen["kwargs"]
    assert kwargs["host"] == "0.0.0.0"
    assert kwargs["port"] == 3842
