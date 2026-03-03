"""Model capability/config metadata used by runtime planning decisions."""

from __future__ import annotations

import os
from dataclasses import dataclass


@dataclass(frozen=True)
class ModelLimits:
    context_window_tokens: int
    max_output_tokens: int
    reserved_output_tokens: int = 4096
    safety_margin_tokens: int = 2048
    compaction_trigger_ratio: float = 0.8
    compaction_target_ratio: float = 0.55
    recent_messages_to_keep: int = 8


@dataclass(frozen=True)
class ModelProperties:
    provider: str
    model: str
    limits: ModelLimits


_KNOWN_LIMITS: dict[tuple[str, str], ModelLimits] = {
    ("openai", "gpt-4.1-mini"): ModelLimits(
        context_window_tokens=1_047_576,
        max_output_tokens=32_768,
    ),
}

_FALLBACK_LIMITS = ModelLimits(
    context_window_tokens=128_000,
    max_output_tokens=16_384,
)


def _env_int(name: str, default: int) -> int:
    raw = os.getenv(name)
    if raw is None:
        return default
    try:
        parsed = int(raw)
    except ValueError:
        return default
    return parsed if parsed > 0 else default


def _env_float(name: str, default: float) -> float:
    raw = os.getenv(name)
    if raw is None:
        return default
    try:
        parsed = float(raw)
    except ValueError:
        return default
    return parsed if 0.05 <= parsed <= 0.95 else default


def resolve_model_properties(*, provider: str, model: str) -> ModelProperties:
    p = provider.strip().lower()
    m = model.strip()
    limits = _KNOWN_LIMITS.get((p, m), _FALLBACK_LIMITS)

    trigger_ratio = _env_float("SHEAF_COMPACTION_TRIGGER_RATIO", limits.compaction_trigger_ratio)
    target_ratio = _env_float("SHEAF_COMPACTION_TARGET_RATIO", limits.compaction_target_ratio)
    if target_ratio >= trigger_ratio:
        target_ratio = max(0.05, trigger_ratio - 0.1)

    resolved = ModelLimits(
        context_window_tokens=_env_int("SHEAF_MODEL_CONTEXT_WINDOW_TOKENS", limits.context_window_tokens),
        max_output_tokens=_env_int("SHEAF_MODEL_MAX_OUTPUT_TOKENS", limits.max_output_tokens),
        reserved_output_tokens=_env_int(
            "SHEAF_MODEL_RESERVED_OUTPUT_TOKENS", limits.reserved_output_tokens
        ),
        safety_margin_tokens=_env_int("SHEAF_MODEL_SAFETY_MARGIN_TOKENS", limits.safety_margin_tokens),
        compaction_trigger_ratio=trigger_ratio,
        compaction_target_ratio=target_ratio,
        recent_messages_to_keep=_env_int("SHEAF_COMPACTION_RECENT_MESSAGES", limits.recent_messages_to_keep),
    )
    return ModelProperties(provider=p, model=m, limits=resolved)
