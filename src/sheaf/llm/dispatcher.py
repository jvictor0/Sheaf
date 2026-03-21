"""LLM dispatch abstraction and provider implementations."""

from __future__ import annotations

import json
import uuid
from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterable
from urllib import error, request

from openai import OpenAI

from sheaf.config.settings import (
    SECRETS_FILE,
    configured_default_model,
    configured_ollama_base_url,
)
from sheaf.llm.model_registry import get_model_registry
from sheaf.llm.model_properties import ModelProperties, resolve_model_properties
from sheaf.tools import build_agent_tools
from sheaf.tools.simple_tool import SimpleTool


@dataclass
class Message:
    role: str
    content: str


@dataclass
class ToolCall:
    id: str
    name: str
    args: dict[str, Any]
    result: str
    is_error: bool


@dataclass
class GenerationResult:
    response: str
    tool_calls: list[ToolCall]


class UnsupportedModelError(RuntimeError):
    """Raised when a requested model is not present in the registry."""


class ProviderConfigurationError(RuntimeError):
    """Raised when required provider configuration is missing."""


class LLMDispatcher(ABC):
    """Abstract interface for model dispatch."""

    @abstractmethod
    def generate(self, messages: Iterable[Message], *, enable_tools: bool = True) -> str:
        """Generate assistant text from a message sequence."""

    @abstractmethod
    def generate_with_details(
        self, messages: Iterable[Message], *, enable_tools: bool = True
    ) -> GenerationResult:
        """Generate assistant text plus tool-call metadata."""

    @abstractmethod
    def stream_generate_with_details(
        self,
        messages: Iterable[Message],
        *,
        on_token: Callable[[str], None],
        on_thinking: Callable[[str], None] | None = None,
        enable_tools: bool = True,
    ) -> GenerationResult:
        """Stream assistant tokens and return final response metadata."""

    @property
    @abstractmethod
    def model_properties(self) -> ModelProperties:
        """Provider/model capabilities used by runtime planning."""


class OpenAIDispatcher(LLMDispatcher):
    """OpenAI dispatcher using direct chat completions API."""

    def __init__(self, api_key: str, model: str = "gpt-5-mini") -> None:
        self._client = OpenAI(api_key=api_key)
        self._model = model
        self._model_properties = resolve_model_properties(provider="openai", model=model)
        self._tools = {tool.name: tool for tool in build_agent_tools()}

    def generate(self, messages: Iterable[Message], *, enable_tools: bool = True) -> str:
        result = self.generate_with_details(messages, enable_tools=enable_tools)
        return result.response

    def generate_with_details(
        self, messages: Iterable[Message], *, enable_tools: bool = True
    ) -> GenerationResult:
        chunks: list[str] = []
        result = self.stream_generate_with_details(
            messages,
            on_token=chunks.append,
            on_thinking=None,
            enable_tools=enable_tools,
        )
        return GenerationResult(response=("".join(chunks) or result.response), tool_calls=result.tool_calls)

    def stream_generate_with_details(
        self,
        messages: Iterable[Message],
        *,
        on_token: Callable[[str], None],
        on_thinking: Callable[[str], None] | None = None,
        enable_tools: bool = True,
    ) -> GenerationResult:
        payload: list[dict[str, Any]] = [
            {
                "role": item.role,
                "content": item.content,
            }
            for item in messages
        ]
        collected_calls: list[ToolCall] = []
        max_rounds = 8

        for round_no in range(max_rounds):
            if on_thinking is not None:
                on_thinking(f"openai_request_started_round_{round_no + 1}")

            stream = self._client.chat.completions.create(
                model=self._model,
                messages=payload,
                tools=self._openai_tool_definitions() if enable_tools else None,
                stream=True,
            )
            chunks: list[str] = []
            finish_reason = None
            raw_calls: dict[int, dict[str, Any]] = {}

            for event in stream:
                choice = event.choices[0] if event.choices else None
                if choice is None:
                    continue
                if getattr(choice, "finish_reason", None) is not None:
                    finish_reason = choice.finish_reason

                delta = getattr(choice, "delta", None)
                if delta is None:
                    continue

                token = getattr(delta, "content", None)
                if isinstance(token, str) and token:
                    chunks.append(token)
                    on_token(token)

                tool_call_deltas = getattr(delta, "tool_calls", None)
                if tool_call_deltas:
                    for item in tool_call_deltas:
                        idx = int(getattr(item, "index", 0) or 0)
                        slot = raw_calls.setdefault(
                            idx,
                            {
                                "id": "",
                                "type": "function",
                                "name": "",
                                "arguments": "",
                            },
                        )
                        call_id = getattr(item, "id", None)
                        if isinstance(call_id, str) and call_id:
                            slot["id"] = call_id

                        fn = getattr(item, "function", None)
                        if fn is not None:
                            name = getattr(fn, "name", None)
                            if isinstance(name, str) and name:
                                slot["name"] = name
                            arguments = getattr(fn, "arguments", None)
                            if isinstance(arguments, str) and arguments:
                                slot["arguments"] += arguments

            if on_thinking is not None:
                on_thinking(f"openai_request_completed_round_{round_no + 1}")

            if enable_tools and raw_calls and finish_reason == "tool_calls":
                assistant_tool_calls: list[dict[str, Any]] = []
                for idx in sorted(raw_calls.keys()):
                    call = raw_calls[idx]
                    call_id = call["id"] or f"tool-call-{uuid.uuid4().hex[:8]}"
                    assistant_tool_calls.append(
                        {
                            "id": call_id,
                            "type": "function",
                            "function": {
                                "name": call["name"],
                                "arguments": call["arguments"],
                            },
                        }
                    )

                payload.append(
                    {
                        "role": "assistant",
                        "content": "".join(chunks) or None,
                        "tool_calls": assistant_tool_calls,
                    }
                )

                for call in assistant_tool_calls:
                    executed = self._execute_tool_call(
                        name=str(call["function"]["name"]),
                        raw_arguments=str(call["function"]["arguments"]),
                    )
                    collected_calls.append(executed)
                    payload.append(
                        {
                            "role": "tool",
                            "tool_call_id": call["id"],
                            "content": executed.result,
                        }
                    )
                    if on_thinking is not None:
                        on_thinking(f"tool_call_{executed.name}")
                continue

            text = "".join(chunks).strip()
            if not text:
                raise RuntimeError("OpenAI returned an empty response")
            return GenerationResult(response=text, tool_calls=collected_calls)

        raise RuntimeError("OpenAI tool loop exceeded max rounds")

    def _execute_tool_call(self, *, name: str, raw_arguments: str) -> ToolCall:
        tool = self._tools.get(name)
        if tool is None:
            return ToolCall(
                id=f"tool-{uuid.uuid4().hex[:8]}",
                name=name,
                args={},
                result=f"Tool error: unknown tool '{name}'",
                is_error=True,
            )

        args: dict[str, Any] = {}
        if raw_arguments.strip():
            try:
                parsed = json.loads(raw_arguments)
                if isinstance(parsed, dict):
                    args = parsed
            except json.JSONDecodeError:
                return ToolCall(
                    id=f"tool-{uuid.uuid4().hex[:8]}",
                    name=name,
                    args={},
                    result=f"Tool error: invalid arguments JSON for '{name}'",
                    is_error=True,
                )

        try:
            result = str(tool.invoke(args))
            return ToolCall(
                id=f"tool-{uuid.uuid4().hex[:8]}",
                name=name,
                args=args,
                result=result,
                is_error=False,
            )
        except Exception as exc:  # noqa: BLE001
            return ToolCall(
                id=f"tool-{uuid.uuid4().hex[:8]}",
                name=name,
                args=args,
                result=f"Tool error: {exc}",
                is_error=True,
            )

    def _openai_tool_definitions(self) -> list[dict[str, Any]]:
        return [self._tool_definition_from_instance(tool) for tool in self._tools.values()]

    def _tool_definition_from_instance(self, tool: SimpleTool) -> dict[str, Any]:
        return {
            "type": "function",
            "function": {
                "name": tool.name,
                "description": tool.description,
                "parameters": tool.parameters_schema,
            },
        }

    @property
    def model_properties(self) -> ModelProperties:
        return self._model_properties


class OllamaDispatcher(LLMDispatcher):
    """Ollama-backed dispatcher using the local/network ollama server."""

    def __init__(self, *, base_url: str, model: str) -> None:
        self._base_url = base_url.rstrip("/")
        self._model = model
        self._model_properties = resolve_model_properties(provider="ollama", model=model)

    def generate(self, messages: Iterable[Message], *, enable_tools: bool = True) -> str:
        result = self.generate_with_details(messages, enable_tools=enable_tools)
        return result.response

    def generate_with_details(
        self, messages: Iterable[Message], *, enable_tools: bool = True
    ) -> GenerationResult:
        payload = {
            "model": self._model,
            "stream": False,
            "messages": [
                {
                    "role": item.role,
                    "content": item.content,
                }
                for item in messages
            ],
        }
        raw = json.dumps(payload).encode("utf-8")
        req = request.Request(
            f"{self._base_url}/api/chat",
            data=raw,
            headers={"Content-Type": "application/json"},
            method="POST",
        )

        try:
            with request.urlopen(req, timeout=60) as response:  # noqa: S310
                body = response.read().decode("utf-8")
        except error.URLError as exc:
            raise RuntimeError(f"Ollama request failed: {exc}") from exc

        try:
            parsed = json.loads(body)
        except json.JSONDecodeError as exc:
            raise RuntimeError("Ollama returned invalid JSON") from exc

        message = parsed.get("message") if isinstance(parsed, dict) else None
        content = message.get("content") if isinstance(message, dict) else None
        text = content.strip() if isinstance(content, str) else ""
        if not text:
            raise RuntimeError("Ollama returned an empty response")

        return GenerationResult(response=text, tool_calls=[])

    def stream_generate_with_details(
        self,
        messages: Iterable[Message],
        *,
        on_token: Callable[[str], None],
        on_thinking: Callable[[str], None] | None = None,
        enable_tools: bool = True,
    ) -> GenerationResult:
        payload = {
            "model": self._model,
            "stream": True,
            "messages": [
                {
                    "role": item.role,
                    "content": item.content,
                }
                for item in messages
            ],
        }
        raw = json.dumps(payload).encode("utf-8")
        req = request.Request(
            f"{self._base_url}/api/chat",
            data=raw,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        if on_thinking is not None:
            on_thinking("ollama_request_started")

        chunks: list[str] = []
        try:
            with request.urlopen(req, timeout=120) as response:  # noqa: S310
                for raw_line in response:
                    line = raw_line.decode("utf-8").strip()
                    if not line:
                        continue
                    parsed = json.loads(line)
                    message = parsed.get("message") if isinstance(parsed, dict) else None
                    token = message.get("content") if isinstance(message, dict) else None
                    if isinstance(token, str) and token:
                        chunks.append(token)
                        on_token(token)
                    if on_thinking is not None:
                        for thinking in self._extract_ollama_thinking(parsed):
                            on_thinking(thinking)
        except error.URLError as exc:
            raise RuntimeError(f"Ollama request failed: {exc}") from exc
        except json.JSONDecodeError as exc:
            raise RuntimeError("Ollama returned invalid JSON stream") from exc

        text = "".join(chunks).strip()
        if not text:
            raise RuntimeError("Ollama returned an empty response")
        if on_thinking is not None:
            on_thinking("ollama_request_completed")
        return GenerationResult(response=text, tool_calls=[])

    def _extract_ollama_thinking(self, parsed: dict[str, Any]) -> list[str]:
        out: list[str] = []

        def _append_value(value: Any) -> None:
            if isinstance(value, str):
                text = value.strip()
                if text:
                    out.append(text)
            elif isinstance(value, list):
                for item in value:
                    _append_value(item)

        message = parsed.get("message") if isinstance(parsed, dict) else None
        if isinstance(message, dict):
            _append_value(message.get("thinking"))
            _append_value(message.get("reasoning"))
        _append_value(parsed.get("thinking"))
        _append_value(parsed.get("reasoning"))
        return out

    @property
    def model_properties(self) -> ModelProperties:
        return self._model_properties


def _load_json_file(path: Path) -> dict[str, object]:
    if not path.exists():
        return {}
    raw = json.loads(path.read_text(encoding="utf-8"))
    return raw if isinstance(raw, dict) else {}


def _openai_api_key_from_file() -> str:
    secrets = _load_json_file(SECRETS_FILE)
    openai_cfg = secrets.get("openai") if isinstance(secrets, dict) else None
    if isinstance(openai_cfg, dict):
        key = openai_cfg.get("api_key")
        if isinstance(key, str) and key:
            return key

    raise ProviderConfigurationError("Missing OpenAI API key. Add openai.api_key in .secrets.json")


def build_dispatcher(model_override: str | None = None) -> LLMDispatcher:
    requested_model = (model_override or "").strip() or configured_default_model()
    registry = get_model_registry()
    descriptor = registry.resolve_model(requested_model)
    if descriptor is None:
        raise UnsupportedModelError(f"Unsupported model '{requested_model}'")

    if descriptor.provider == "openai":
        api_key = _openai_api_key_from_file()
        return OpenAIDispatcher(api_key=api_key, model=descriptor.name)
    if descriptor.provider == "ollama":
        return OllamaDispatcher(base_url=configured_ollama_base_url(), model=descriptor.name)
    raise UnsupportedModelError(f"Unsupported LLM provider '{descriptor.provider}' for model '{descriptor.name}'")
