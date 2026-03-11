from __future__ import annotations

from fastapi.testclient import TestClient

import sheaf.server.app as server_app


def test_chat_endpoint_accepts_gpt_5_mini_and_forwards_model(monkeypatch) -> None:
    calls: list[dict[str, str]] = []

    def _fake_run_chat_turn(chat_id: str, user_message: str, *, model: str):
        calls.append({"chat_id": chat_id, "user_message": user_message, "model": model})
        return "ok", "cp-1", []

    monkeypatch.setattr(server_app, "run_chat_turn", _fake_run_chat_turn)
    client = TestClient(server_app.app)

    response = client.post(
        "/chats/test-chat/messages",
        json={"message": "hello", "model": "gpt-5-mini"},
    )

    assert response.status_code == 200
    assert calls == [{"chat_id": "test-chat", "user_message": "hello", "model": "gpt-5-mini"}]


def test_chat_endpoint_accepts_gpt_53_codex_and_forwards_model(monkeypatch) -> None:
    calls: list[dict[str, str]] = []

    def _fake_run_chat_turn(chat_id: str, user_message: str, *, model: str):
        calls.append({"chat_id": chat_id, "user_message": user_message, "model": model})
        return "ok", "cp-2", []

    monkeypatch.setattr(server_app, "run_chat_turn", _fake_run_chat_turn)
    client = TestClient(server_app.app)

    response = client.post(
        "/chats/test-chat/messages",
        json={"message": "hello", "model": "gpt-5.3-codex"},
    )

    assert response.status_code == 200
    assert calls == [{"chat_id": "test-chat", "user_message": "hello", "model": "gpt-5.3-codex"}]


def test_chat_endpoint_accepts_gpt_52_and_forwards_model(monkeypatch) -> None:
    calls: list[dict[str, str]] = []

    def _fake_run_chat_turn(chat_id: str, user_message: str, *, model: str):
        calls.append({"chat_id": chat_id, "user_message": user_message, "model": model})
        return "ok", "cp-2b", []

    monkeypatch.setattr(server_app, "run_chat_turn", _fake_run_chat_turn)
    client = TestClient(server_app.app)

    response = client.post(
        "/chats/test-chat/messages",
        json={"message": "hello", "model": "gpt-5.2"},
    )

    assert response.status_code == 200
    assert calls == [{"chat_id": "test-chat", "user_message": "hello", "model": "gpt-5.2"}]


def test_chat_endpoint_accepts_gpt_54_and_forwards_model(monkeypatch) -> None:
    calls: list[dict[str, str]] = []

    def _fake_run_chat_turn(chat_id: str, user_message: str, *, model: str):
        calls.append({"chat_id": chat_id, "user_message": user_message, "model": model})
        return "ok", "cp-2c", []

    monkeypatch.setattr(server_app, "run_chat_turn", _fake_run_chat_turn)
    client = TestClient(server_app.app)

    response = client.post(
        "/chats/test-chat/messages",
        json={"message": "hello", "model": "gpt-5.4"},
    )

    assert response.status_code == 200
    assert calls == [{"chat_id": "test-chat", "user_message": "hello", "model": "gpt-5.4"}]


def test_chat_endpoint_rejects_invalid_model(monkeypatch) -> None:
    def _fake_run_chat_turn(chat_id: str, user_message: str, *, model: str):
        return "ok", "cp-3", []

    monkeypatch.setattr(server_app, "run_chat_turn", _fake_run_chat_turn)
    client = TestClient(server_app.app)

    response = client.post(
        "/chats/test-chat/messages",
        json={"message": "hello", "model": "bad-model"},
    )

    assert response.status_code == 400
    detail = str(response.json().get("detail", ""))
    assert "Unsupported model" in detail
    assert "gpt-5-mini" in detail
    assert "gpt-5.2" in detail
    assert "gpt-5.3-codex" in detail
    assert "gpt-5.4" in detail


def test_chat_endpoint_uses_config_default_when_model_omitted(monkeypatch) -> None:
    calls: list[dict[str, str]] = []

    def _fake_run_chat_turn(chat_id: str, user_message: str, *, model: str):
        calls.append({"chat_id": chat_id, "user_message": user_message, "model": model})
        return "ok", "cp-4", []

    monkeypatch.setattr(server_app, "run_chat_turn", _fake_run_chat_turn)
    monkeypatch.setattr(server_app, "configured_openai_model", lambda: "gpt-5-mini")
    client = TestClient(server_app.app)

    response = client.post(
        "/chats/test-chat/messages",
        json={"message": "hello"},
    )

    assert response.status_code == 200
    assert calls == [{"chat_id": "test-chat", "user_message": "hello", "model": "gpt-5-mini"}]
