"""Application settings and path helpers."""

from __future__ import annotations

import json
from functools import lru_cache
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
CONFIG_PATH = REPO_ROOT / "sheaf_server.config"
_SUPPORTED_OPENAI_MODELS = {
    "gpt-5-mini",
    "gpt-5.2",
    "gpt-5.3-codex",
    "gpt-5.4",
}


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
DATA_ARCHIVE_DIR = REPO_ROOT / "data_archive"
USER_DBS_DIR = DATA_DIR / "user_dbs"
SYSTEM_PROMPTS_DIR = DATA_DIR / "system_prompts"
SERVER_DB_PATH = _path_config_value("server_db_path", DATA_DIR / "server.sqlite3")
SECRETS_FILE = _path_config_value("secrets_file", REPO_ROOT / ".secrets.json")
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
            configured = model.strip()
            if configured in _SUPPORTED_OPENAI_MODELS:
                return configured
    return "gpt-5-mini"


def configured_default_model() -> str:
    cfg = load_server_config()
    llm = cfg.get("llm")
    if isinstance(llm, dict):
        model = llm.get("default_model")
        if isinstance(model, str) and model.strip():
            return model.strip()
    return configured_openai_model()


def configured_system_prompt_file() -> str:
    cfg = load_server_config()
    llm = cfg.get("llm")
    if isinstance(llm, dict):
        prompt_file = llm.get("system_prompt_file")
        if isinstance(prompt_file, str) and prompt_file.strip():
            return prompt_file.strip()
    return "sheaf_default.md"


def configured_ollama_base_url() -> str:
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


def ensure_data_dirs() -> None:
    USER_DBS_DIR.mkdir(parents=True, exist_ok=True)
    SYSTEM_PROMPTS_DIR.mkdir(parents=True, exist_ok=True)
