"""Application settings and path helpers."""

from __future__ import annotations

import json
import os
from functools import lru_cache
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
CONFIG_PATH = REPO_ROOT / "sheaf_server.config"


@lru_cache(maxsize=1)
def load_server_config() -> dict[str, object]:
    if not CONFIG_PATH.exists():
        return {}
    try:
        raw = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return raw if isinstance(raw, dict) else {}


def _path_config_value(key: str, default: Path) -> Path:
    cfg = load_server_config()
    value = cfg.get(key)
    if isinstance(value, str) and value.strip():
        return Path(value.strip()).expanduser()
    return default


DATA_DIR = _path_config_value("data_dir", REPO_ROOT / "data")
CHATS_DIR = DATA_DIR / "chats"
SECRETS_FILE = _path_config_value("secrets_file", REPO_ROOT / ".secrets.json")
DEFAULT_TOME_DIR = DATA_DIR / "notes"
TOME_DIR = _path_config_value("tome_dir", DEFAULT_TOME_DIR)
RUNTIME_DIR = REPO_ROOT / ".runtime"
REBOOT_REQUEST_FILE = RUNTIME_DIR / "reboot.request"


def configured_llm_provider() -> str:
    cfg = load_server_config()
    llm = cfg.get("llm")
    if isinstance(llm, dict):
        provider = llm.get("provider")
        if isinstance(provider, str) and provider.strip():
            return provider.strip().lower()
    return "openai"


def configured_openai_model() -> str:
    cfg = load_server_config()
    llm = cfg.get("llm")
    if isinstance(llm, dict):
        model = llm.get("openai_model")
        if isinstance(model, str) and model.strip():
            return model.strip()
    return "gpt-5-mini"


def configured_default_model() -> str:
    cfg = load_server_config()
    llm = cfg.get("llm")
    if isinstance(llm, dict):
        model = llm.get("default_model")
        if isinstance(model, str) and model.strip():
            return model.strip()
    return configured_openai_model()


def configured_ollama_base_url() -> str:
    env_value = os.environ.get("OLLAMA_HOST")
    if isinstance(env_value, str) and env_value.strip():
        raw = env_value.strip()
    else:
        cfg = load_server_config()
        llm = cfg.get("llm")
        raw = "http://127.0.0.1:11434"
        if isinstance(llm, dict):
            value = llm.get("ollama_base_url")
            if isinstance(value, str) and value.strip():
                raw = value.strip()

    if raw.startswith("http://") or raw.startswith("https://"):
        return raw
    return f"http://{raw}"


def configured_ollama_cache_ttl_seconds() -> int:
    cfg = load_server_config()
    llm = cfg.get("llm")
    if isinstance(llm, dict):
        raw = llm.get("ollama_cache_ttl_seconds")
        try:
            parsed = int(raw)
            if parsed > 0:
                return parsed
        except (TypeError, ValueError):
            pass
    return 30


def configured_model_tuning() -> tuple[dict[str, object], dict[str, object]]:
    cfg = load_server_config()
    llm = cfg.get("llm")
    if not isinstance(llm, dict):
        return {}, {}
    limits = llm.get("model_limits")
    compaction = llm.get("compaction")
    return (
        limits if isinstance(limits, dict) else {},
        compaction if isinstance(compaction, dict) else {},
    )


# Backward-compatible alias for existing imports/tool names.
NOTES_DIR = TOME_DIR


def ensure_data_dirs() -> None:
    CHATS_DIR.mkdir(parents=True, exist_ok=True)
    TOME_DIR.mkdir(parents=True, exist_ok=True)
