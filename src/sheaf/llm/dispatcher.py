"""LLM dispatch abstraction and provider implementations."""

from __future__ import annotations

import json
import os
from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from sheaf.agent.langchain_chain import invoke_chat_chain
from sheaf.config.settings import SECRETS_FILE
from sheaf.llm.model_properties import ModelProperties, resolve_model_properties


@dataclass
class Message:
    role: str
    content: str


class LLMDispatcher(ABC):
    """Abstract interface for model dispatch."""

    @abstractmethod
    def generate(self, messages: Iterable[Message], *, enable_tools: bool = True) -> str:
        """Generate assistant text from a message sequence."""

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
        return invoke_chat_chain(
            api_key=self._api_key,
            model=self._model,
            messages=messages,
            enable_tools=enable_tools,
        )

    @property
    def model_properties(self) -> ModelProperties:
        return self._model_properties


def _load_json_file(path: Path) -> dict[str, object]:
    if not path.exists():
        return {}
    raw = json.loads(path.read_text(encoding="utf-8"))
    return raw if isinstance(raw, dict) else {}


def _openai_api_key_from_env_or_file() -> str:
    env_key = os.getenv("OPENAI_API_KEY")
    if env_key:
        return env_key

    secrets = _load_json_file(SECRETS_FILE)
    openai_cfg = secrets.get("openai") if isinstance(secrets, dict) else None
    if isinstance(openai_cfg, dict):
        key = openai_cfg.get("api_key")
        if isinstance(key, str) and key:
            return key

    raise RuntimeError(
        "Missing OpenAI API key. Set OPENAI_API_KEY or add openai.api_key in .secrets.json"
    )


def build_dispatcher() -> LLMDispatcher:
    provider = os.getenv("SHEAF_LLM_PROVIDER", "openai").strip().lower()
    if provider != "openai":
        raise RuntimeError(f"Unsupported LLM provider: {provider}")

    api_key = _openai_api_key_from_env_or_file()
    model = os.getenv("SHEAF_OPENAI_MODEL", "gpt-4.1-mini")
    return LangChainOpenAIDispatcher(api_key=api_key, model=model)
