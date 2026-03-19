"""Model discovery and routing metadata for server and clients."""

from __future__ import annotations

import json
import threading
import time
from dataclasses import dataclass
from typing import Any
from urllib import error, request

from sheaf.config.settings import (
    configured_default_model,
    configured_openai_model,
    configured_ollama_base_url,
    configured_ollama_cache_ttl_seconds,
)

_BUILTIN_OPENAI_MODELS: tuple[str, ...] = (
    "gpt-5-mini",
    "gpt-5.2",
    "gpt-5.3-codex",
    "gpt-5.4",
)


@dataclass(frozen=True)
class ModelDescriptor:
    name: str
    provider: str
    source: str
    metadata: dict[str, Any]
    is_default: bool = False

    def as_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "provider": self.provider,
            "source": self.source,
            "metadata": self.metadata,
            "is_default": self.is_default,
        }


class ModelRegistry:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._ollama_models: dict[str, ModelDescriptor] = {}
        self._cache_expiry_ts = 0.0

    def list_models(self) -> list[ModelDescriptor]:
        self._refresh_ollama_models(force=False)
        return self._merged_models()

    def resolve_model(self, name: str, *, allow_refresh: bool = True) -> ModelDescriptor | None:
        resolved_name = name.strip()
        if not resolved_name:
            return None

        for model in self._merged_models():
            if model.name == resolved_name:
                return model

        if allow_refresh:
            self._refresh_ollama_models(force=True)
            for model in self._merged_models():
                if model.name == resolved_name:
                    return model

        probed = self._probe_ollama_model(resolved_name)
        if probed is not None:
            with self._lock:
                self._ollama_models[probed.name] = probed
            return self._with_default_flag(probed)
        return None

    def _merged_models(self) -> list[ModelDescriptor]:
        default_model = configured_default_model()
        configured_openai = configured_openai_model()
        openai_models = dict.fromkeys([*_BUILTIN_OPENAI_MODELS, configured_openai])

        builtins = [
            ModelDescriptor(
                name=model,
                provider="openai",
                source="builtin",
                metadata={},
                is_default=(model == default_model),
            )
            for model in openai_models
            if isinstance(model, str) and model.strip()
        ]

        with self._lock:
            ollama_models = list(self._ollama_models.values())

        decorated_ollama = [self._with_default_flag(model) for model in ollama_models]
        merged = {item.name: item for item in [*builtins, *decorated_ollama]}
        return sorted(merged.values(), key=lambda item: item.name)

    def _with_default_flag(self, model: ModelDescriptor) -> ModelDescriptor:
        default_model = configured_default_model()
        return ModelDescriptor(
            name=model.name,
            provider=model.provider,
            source=model.source,
            metadata=model.metadata,
            is_default=(model.name == default_model),
        )

    def _refresh_ollama_models(self, *, force: bool) -> None:
        now = time.time()
        with self._lock:
            if not force and now < self._cache_expiry_ts:
                return

        try:
            discovered = self._run_ollama_list()
        except Exception:  # noqa: BLE001
            ttl = configured_ollama_cache_ttl_seconds()
            with self._lock:
                self._cache_expiry_ts = now + ttl
            return

        ttl = configured_ollama_cache_ttl_seconds()
        with self._lock:
            self._ollama_models = {item.name: item for item in discovered}
            self._cache_expiry_ts = now + ttl

    def _run_ollama_list(self) -> list[ModelDescriptor]:
        endpoint = configured_ollama_base_url().rstrip("/") + "/api/tags"
        req = request.Request(endpoint, method="GET")
        try:
            with request.urlopen(req, timeout=8) as response:  # noqa: S310
                raw = response.read().decode("utf-8")
        except error.URLError as exc:
            raise RuntimeError(f"ollama tags failed: {exc}") from exc

        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise RuntimeError("ollama tags returned invalid JSON") from exc

        items = parsed.get("models", []) if isinstance(parsed, dict) else []
        if not isinstance(items, list):
            return []

        models: list[ModelDescriptor] = []
        for item in items:
            if not isinstance(item, dict):
                continue
            name = item.get("name")
            if not isinstance(name, str) or not name.strip():
                continue

            metadata: dict[str, Any] = {}
            digest = item.get("digest")
            if isinstance(digest, str) and digest:
                metadata["id"] = digest
            size = item.get("size")
            if isinstance(size, int):
                metadata["size_bytes"] = size
            modified = item.get("modified_at")
            if isinstance(modified, str) and modified:
                metadata["modified_at"] = modified

            models.append(
                ModelDescriptor(
                    name=name.strip(),
                    provider="ollama",
                    source="ollama",
                    metadata=metadata,
                )
            )
        return models

    def _probe_ollama_model(self, model_name: str) -> ModelDescriptor | None:
        payload = json.dumps({"name": model_name}).encode("utf-8")
        endpoint = configured_ollama_base_url().rstrip("/") + "/api/show"
        req = request.Request(
            endpoint,
            data=payload,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with request.urlopen(req, timeout=8) as response:  # noqa: S310
                raw = response.read().decode("utf-8")
        except error.HTTPError as exc:
            if exc.code == 404:
                return None
            return None
        except (error.URLError, TimeoutError, OSError):
            return None

        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError:
            parsed = {}

        metadata: dict[str, Any] = {}
        if isinstance(parsed, dict):
            digest = parsed.get("digest")
            if isinstance(digest, str) and digest:
                metadata["id"] = digest
            size = parsed.get("size")
            if isinstance(size, int):
                metadata["size_bytes"] = size

        return ModelDescriptor(
            name=model_name,
            provider="ollama",
            source="ollama",
            metadata=metadata,
        )


_registry = ModelRegistry()


def get_model_registry() -> ModelRegistry:
    return _registry
