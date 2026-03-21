from __future__ import annotations

import json
from urllib import request

from sheaf.llm.dispatcher import Message, OllamaDispatcher


class _FakeHTTPResponse:
    def __init__(self, lines: list[str]) -> None:
        self._lines = [line.encode("utf-8") for line in lines]

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False

    def __iter__(self):
        return iter(self._lines)


def test_ollama_streaming_emits_reasoning_traces(monkeypatch) -> None:
    lines = [
        json.dumps({"message": {"thinking": "plan step 1"}}),
        json.dumps({"message": {"content": "Hello"}}),
        json.dumps({"reasoning": ["step 2", "step 3"]}),
    ]

    def _fake_urlopen(req, timeout=0):  # noqa: ANN001, ARG001
        return _FakeHTTPResponse(lines)

    monkeypatch.setattr(request, "urlopen", _fake_urlopen)

    dispatcher = OllamaDispatcher(base_url="http://localhost:11434", model="test-model")
    tokens: list[str] = []
    thinking: list[str] = []
    result = dispatcher.stream_generate_with_details(
        [Message(role="user", content="hi")],
        on_token=tokens.append,
        on_thinking=thinking.append,
        enable_tools=False,
    )

    assert result.response == "Hello"
    assert tokens == ["Hello"]
    assert thinking[0] == "ollama_request_started"
    assert "plan step 1" in thinking
    assert "step 2" in thinking
    assert "step 3" in thinking
    assert thinking[-1] == "ollama_request_completed"
