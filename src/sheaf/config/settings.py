"""Application settings and path helpers."""

from __future__ import annotations

import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
DATA_DIR = Path(os.getenv("SHEAF_DATA_DIR", str(REPO_ROOT / "data")))
CHATS_DIR = DATA_DIR / "chats"
NOTES_DIR = DATA_DIR / "notes"
SECRETS_FILE = Path(os.getenv("SHEAF_SECRETS_FILE", str(REPO_ROOT / ".secrets.json")))


def ensure_data_dirs() -> None:
    CHATS_DIR.mkdir(parents=True, exist_ok=True)
    NOTES_DIR.mkdir(parents=True, exist_ok=True)
