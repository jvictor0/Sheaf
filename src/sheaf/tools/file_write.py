"""Tooling for constrained note writes under data/notes."""

from __future__ import annotations

from pathlib import Path

from langchain_core.tools import tool

from sheaf.config.settings import NOTES_DIR


def resolve_note_path(relative_path: str) -> Path:
    path_text = relative_path.strip()
    if not path_text:
        raise ValueError("relative_path must not be empty")

    root = NOTES_DIR.resolve()
    candidate = (root / path_text).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as exc:
        raise ValueError("Path escapes allowed notes directory") from exc
    if candidate == root:
        raise ValueError("relative_path must point to a file under data/notes")
    return candidate


@tool("write_note")
def write_note_tool(relative_path: str, content: str, overwrite: bool = True) -> str:
    """Write UTF-8 text into data/notes/<relative_path>.

    Use this when you need to save durable notes or outputs.
    The path must stay under data/notes and parent folders will be created automatically.
    """

    target = resolve_note_path(relative_path)
    if target.exists() and target.is_dir():
        raise ValueError("Target path points to a directory, expected a file")
    if target.exists() and not overwrite:
        raise ValueError("Target file already exists and overwrite is false")

    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content, encoding="utf-8")
    bytes_written = len(content.encode("utf-8"))
    return f"Wrote {target} ({bytes_written} bytes)"
