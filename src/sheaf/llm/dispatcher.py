"""LLM dispatch abstraction and provider implementations."""

from __future__ import annotations

import json
from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

from sheaf.agent.langchain_chain import invoke_chat_chain
from sheaf.config.settings import (
    SECRETS_FILE,
    configured_llm_provider,
    configured_openai_model,
)
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

    def __init__(self, api_key: str, model: str = "gpt-4.1-mini") -> None:
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


def build_dispatcher() -> LLMDispatcher:
    provider = configured_llm_provider()
    if provider != "openai":
        raise RuntimeError(f"Unsupported LLM provider: {provider}")

    api_key = _openai_api_key_from_file()
    model = configured_openai_model()
    return LangChainOpenAIDispatcher(api_key=api_key, model=model)
