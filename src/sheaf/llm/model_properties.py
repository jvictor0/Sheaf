"""Model capability/config metadata used by runtime planning decisions."""

from __future__ import annotations

from dataclasses import dataclass

from sheaf.config.settings import configured_model_tuning


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
    ("openai", "gpt-5-mini"): ModelLimits(
        context_window_tokens=400_000,
        max_output_tokens=128_000,
    ),
    ("openai", "gpt-5.2"): ModelLimits(
        context_window_tokens=400_000,
        max_output_tokens=128_000,
    ),
    ("openai", "gpt-5.3-codex"): ModelLimits(
        context_window_tokens=400_000,
        max_output_tokens=128_000,
    ),
    ("openai", "gpt-5.4"): ModelLimits(
        context_window_tokens=400_000,
        max_output_tokens=128_000,
    ),
}

_FALLBACK_LIMITS = ModelLimits(
    context_window_tokens=128_000,
    max_output_tokens=16_384,
)


def _cfg_int(raw: object, default: int) -> int:
    try:
        parsed = int(raw)
    except (TypeError, ValueError):
        return default
    return parsed if parsed > 0 else default


def _cfg_ratio(raw: object, default: float) -> float:
    try:
        parsed = float(raw)
    except (TypeError, ValueError):
        return default
    return parsed if 0.05 <= parsed <= 0.95 else default


def resolve_model_properties(*, provider: str, model: str) -> ModelProperties:
    p = provider.strip().lower()
    m = model.strip()
    limits = _KNOWN_LIMITS.get((p, m), _FALLBACK_LIMITS)
    limits_cfg, compaction_cfg = configured_model_tuning()

    trigger_ratio = _cfg_ratio(
        compaction_cfg.get("trigger_ratio"),
        limits.compaction_trigger_ratio,
    )
    target_ratio = _cfg_ratio(
        compaction_cfg.get("target_ratio"),
        limits.compaction_target_ratio,
    )
    if target_ratio >= trigger_ratio:
        target_ratio = max(0.05, trigger_ratio - 0.1)

    resolved = ModelLimits(
        context_window_tokens=_cfg_int(
            limits_cfg.get("context_window_tokens"),
            limits.context_window_tokens,
        ),
        max_output_tokens=_cfg_int(
            limits_cfg.get("max_output_tokens"),
            limits.max_output_tokens,
        ),
        reserved_output_tokens=_cfg_int(
            limits_cfg.get("reserved_output_tokens"),
            limits.reserved_output_tokens,
        ),
        safety_margin_tokens=_cfg_int(
            limits_cfg.get("safety_margin_tokens"),
            limits.safety_margin_tokens,
        ),
        compaction_trigger_ratio=trigger_ratio,
        compaction_target_ratio=target_ratio,
        recent_messages_to_keep=_cfg_int(
            compaction_cfg.get("recent_messages_to_keep"),
            limits.recent_messages_to_keep,
        ),
    )
    return ModelProperties(provider=p, model=m, limits=resolved)
