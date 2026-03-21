from __future__ import annotations

import sheaf.config.settings as settings


def test_configured_openai_model_falls_back_to_gpt5_mini_for_unsupported_model(monkeypatch) -> None:
    monkeypatch.setattr(
        settings,
        "load_server_config",
        lambda: {"llm": {"openai_model": "gpt-4.1-mini"}},
    )
    assert settings.configured_openai_model() == "gpt-5-mini"
