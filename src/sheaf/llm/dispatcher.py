"""LLM dispatch abstraction and provider implementations."""

from __future__ import annotations

import json
from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable
from urllib import error, request

from sheaf.agent.langchain_chain import invoke_chat_chain
from sheaf.config.settings import (
    SECRETS_FILE,
    configured_default_model,
    configured_ollama_base_url,
)
from sheaf.llm.model_registry import get_model_registry
from sheaf.llm.model_properties import ModelProperties, resolve_model_properties


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

    @property
    @abstractmethod
    def model_properties(self) -> ModelProperties:
        """Provider/model capabilities used by runtime planning."""


class LangChainOpenAIDispatcher(LLMDispatcher):
    """LangChain-backed OpenAI dispatcher using a chat chain."""

    def __init__(self, api_key: str, model: str = "gpt-5-mini") -> None:
        self._api_key = api_key
        self._model = model
        self._model_properties = resolve_model_properties(provider="openai", model=model)

    def generate(self, messages: Iterable[Message], *, enable_tools: bool = True) -> str:
        result = self.generate_with_details(messages, enable_tools=enable_tools)
        return result.response

    def generate_with_details(
        self, messages: Iterable[Message], *, enable_tools: bool = True
    ) -> GenerationResult:
        chain_result = invoke_chat_chain(
            api_key=self._api_key,
            model=self._model,
            messages=messages,
            enable_tools=enable_tools,
        )
        return GenerationResult(
            response=chain_result.response,
            tool_calls=[
                ToolCall(
                    id=item.id,
                    name=item.name,
                    args=item.args,
                    result=item.result,
                    is_error=item.is_error,
                )
                for item in chain_result.tool_calls
            ],
        )

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

    raise RuntimeError("Missing OpenAI API key. Add openai.api_key in .secrets.json")


def build_dispatcher(model_override: str | None = None) -> LLMDispatcher:
    requested_model = (model_override or "").strip() or configured_default_model()
    registry = get_model_registry()
    descriptor = registry.resolve_model(requested_model)
    if descriptor is None:
        raise RuntimeError(f"Unsupported model '{requested_model}'")

    if descriptor.provider == "openai":
        api_key = _openai_api_key_from_file()
        return LangChainOpenAIDispatcher(api_key=api_key, model=descriptor.name)
    if descriptor.provider == "ollama":
        return OllamaDispatcher(base_url=configured_ollama_base_url(), model=descriptor.name)
    raise RuntimeError(f"Unsupported LLM provider '{descriptor.provider}' for model '{descriptor.name}'")
